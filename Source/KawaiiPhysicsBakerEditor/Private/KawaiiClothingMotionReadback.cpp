#include "KawaiiClothingMotionReadback.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "KawaiiPhysicsBakePreviewComponent.h"
#include "KawaiiPhysicsBakePreviewCorrection.h"
#include "KawaiiPhysicsBakeSampling.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"

namespace KawaiiClothingReadback {
	FGuid AnimationRevision(const UAnimSequence* Animation) {
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Animation ? Animation->GetRawDataGuid() : FGuid();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}

	bool Decode(UAnimSequence& Animation, USkeletalMesh& Mesh, float Time, FFrame& Frame, FString& Error) {
		Frame = FFrame();
		if (!Mesh.GetSkeleton() || Animation.GetSkeleton() != Mesh.GetSkeleton() || Animation.IsValidAdditive() || !FMath::IsFinite(Time)) {
			Error = TEXT("回读需要与网格体使用同一骨架的非叠加动画。"); return false;
		}
		const auto& Reference = Mesh.GetRefSkeleton();
		TArray<FBoneIndexType> Indices;
		for (int32 Bone = 0; Bone < Reference.GetRawBoneNum(); ++Bone) { Indices.Add(Bone); }
		if (Indices.IsEmpty()) { Error = TEXT("网格体没有真实骨骼。"); return false; }
		FMemMark Mark(FMemStack::Get());
		FBoneContainer Bones(Indices, UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::None), Mesh);
		Bones.SetUseRAWData(true);
		FCompactPose Pose;
		Pose.SetBoneContainer(&Bones);
		Pose.ResetToRefPose();
		FBlendedCurve Curves;
		Curves.InitFrom(Bones);
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData Data(Pose, Curves, Attributes);
		FAnimExtractContext Context(FMath::Clamp(double(Time), 0., double(Animation.GetPlayLength())), false);
		Context.bIgnoreRootLock = true;
		Context.bExtractWithRootMotionProvider = false;
		Animation.GetBonePose(Data, Context, true);
		// Include reference virtual bones for the viewport; recorded proxy endpoints remain real bones.
		TArray<FTransform> ComponentPose = Reference.GetRefBonePose();
		for (const auto Index : Pose.ForEachBoneIndex()) { ComponentPose[Bones.MakeMeshPoseIndex(Index).GetInt()] = Pose[Index]; }
		for (int32 Bone = 0; Bone < ComponentPose.Num(); ++Bone) {
			const int32 Parent = Reference.GetParentIndex(Bone);
			if (Parent >= 0) { ComponentPose[Bone] *= ComponentPose[Parent]; }
			ComponentPose[Bone].NormalizeRotation();
			if (!KawaiiPhysicsBakeSampling::IsValidTransform(ComponentPose[Bone])) { Error = TEXT("动画回读产生无效变换。"); return false; }
			Frame.Pose.Add(FTransform3f(ComponentPose[Bone]));
		}
		Curves.ForEachElement([&](const UE::Anim::FCurveElement& Element) {
			if (Mesh.FindMorphTarget(Element.Name)) { Frame.MorphCurves.Add(Element.Name, Element.Value); }
			Frame.MaterialCurves.Add(Element.Name, Element.Value);
		});
		return true;
	}

	bool Build(const UKawaiiClothingProxyMotion& Original, UAnimSequence& Edited, UKawaiiClothingProxyMotion& Result, TArray<FFrame>& Frames, FString& Error) {
		Error.Reset(); Frames.Reset();
		if (&Original == &Result || Original.PartName.IsNone() || Original.PartName == TEXT("Main")) {
			Error = TEXT("请选择附加衣物的代理运动，并使用独立的临时回读结果。"); return false;
		}
		USkeletalMesh* Mesh = Original.SourceMesh.LoadSynchronous();
		UAnimSequence* Body = Original.BodyBakedAnimation.LoadSynchronous();
		UAnimSequence* Source = Original.SourceAnimation.LoadSynchronous();
		if (!Mesh || !Body || !Source || !Original.Definition || !Original.Definition->Validate(Error)) {
			if (Error.IsEmpty()) { Error = TEXT("代理运动缺少网格体、身体烘焙动画或源动作。请先完成初次烘焙。"); } return false;
		}
		if (Original.SourceDataGuid.IsValid() && Original.SourceDataGuid != AnimationRevision(Source)) {
			Error = TEXT("身体源动作已经改变，请先重新烘焙身体和衣物；仅回读衣物不能重新对齐不同动作。"); return false;
		}
		if (Original.SampleRate < 1 || Original.SampleRate > 240 || Original.SourceDuration <= 0.f || Original.SourceDuration > 600.f
			|| !FMath::IsFinite(Original.SourceDuration) || !KawaiiPhysicsBakeSampling::IsValidTransform(Original.RelativeTransform)
			|| Original.Definition->PreviewMesh.ToSoftObjectPath() != FSoftObjectPath(Mesh)) {
			Error = TEXT("原代理的采样时间、衣物网格体或相对变换无效。"); return false;
		}
		const int32 Count = KawaiiPhysicsBakeSampling::NumIntervals(Original.SourceDuration, Original.SampleRate) + 1;
		const float Duration = float(Count - 1) / Original.SampleRate;
		if (Edited.GetSkeleton() != Mesh->GetSkeleton() || Edited.IsValidAdditive() || &Edited == Body
			|| !FMath::IsNearlyEqual(Edited.GetPlayLength(), Duration, 1.e-4f)
			|| FMath::Abs(Body->GetPlayLength() - Duration) > 1.f / Original.SampleRate + 1.e-4f) {
			Error = TEXT("修整动画必须是该衣物骨架的非叠加动画，并保持原烘焙时长。不能把身体动画当作修整衣物动画。"); return false;
		}
		Result.Definition = Original.Definition;
		Result.DefinitionSignature = Original.Definition->ComputeSignature();
		Result.SourceMesh = Mesh;
		Result.SourceAnimation = Source;
		Result.SourceDataGuid = AnimationRevision(Source);
		Result.BakedAnimation = &Edited;
		Result.BakedAnimationDataGuid = AnimationRevision(&Edited);
		Result.BodyBakedAnimation = Body;
		Result.BakeGroup = Original.BakeGroup;
		Result.MotionRevision = FGuid::NewGuid();
		Result.PartName = Original.PartName;
		Result.RelativeTransform = Original.RelativeTransform;
		Result.SourceSkeletonSignature = KawaiiClothingProxy::ComputeSkeletonSignature(Mesh->GetRefSkeleton());
		Result.SampleRate = Original.SampleRate;
		Result.NumFrames = Count;
		Result.SourceDuration = Original.SourceDuration;
		Result.Shapes = Original.Definition->Shapes;
		TArray<int32> MeshIndices;
		if (!KawaiiClothingProxy::BuildMinimalHierarchy(Mesh->GetRefSkeleton(), Result.Shapes, Result.BoneNames, Result.ParentIndices, MeshIndices, Error)) { return false; }
		if (int64(Mesh->GetRefSkeleton().GetNum() + MeshIndices.Num()) * Count > KawaiiClothingProxy::MaxKeys) {
			Error = TEXT("回读超过 400 万个骨骼样本，请缩短动画或降低初次烘焙帧率。"); return false;
		}
		Result.LocalKeys.Reset();
		FScopedSlowTask Progress(Count, FText::FromString(TEXT("读取修整后的衣物动画")));
		if (!IsRunningCommandlet()) { Progress.MakeDialog(true); }
		for (int32 Index = 0; Index < Count; ++Index) {
			Progress.EnterProgressFrame();
			if (Progress.ShouldCancel()) { Error = TEXT("已取消回读，原代理未修改。"); Frames.Reset(); return false; }
			FFrame& Frame = Frames.AddDefaulted_GetRef();
			if (!Decode(Edited, *Mesh, float(Index) / Result.SampleRate, Frame, Error)) { Frames.Reset(); return false; }
			for (int32 Bone = 0; Bone < MeshIndices.Num(); ++Bone) {
				if (MeshIndices[Bone] >= Mesh->GetRefSkeleton().GetRawBoneNum()) { Error = TEXT("代理端点不能绑定虚拟骨骼。"); return false; }
				FTransform Local(Frame.Pose[MeshIndices[Bone]]);
				const int32 Parent = Result.ParentIndices[Bone];
				if (Parent >= 0) { Local = Local.GetRelativeTransform(FTransform(Frame.Pose[MeshIndices[Parent]])); }
				Local.NormalizeRotation();
				Result.LocalKeys.Add(Local);
			}
		}
		return Result.Validate(Error);
	}

	static bool Save(UObject& Asset) {
		const FString Filename = FPackageName::LongPackageNameToFilename(Asset.GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Asset.GetPackage(), &Asset, *Filename, Args);
	}

	static void CopyPayload(const UKawaiiClothingProxyMotion& From, UKawaiiClothingProxyMotion& To) {
		for (TFieldIterator<FProperty> Property(UKawaiiClothingProxyMotion::StaticClass(), EFieldIteratorFlags::ExcludeSuper); Property; ++Property) {
			Property->CopyCompleteValue_InContainer(&To, &From);
		}
	}

	bool Publish(UKawaiiClothingProxyMotion& Original, const UKawaiiClothingProxyMotion& Candidate, FString& BackupPath, FString& Error) {
		Error.Reset(); BackupPath.Reset();
		if (&Original == &Candidate || Candidate.PartName != Original.PartName || Candidate.SourceAnimation != Original.SourceAnimation
			|| Candidate.BodyBakedAnimation != Original.BodyBakedAnimation || Candidate.SourceMesh != Original.SourceMesh
			|| Candidate.Definition != Original.Definition || !Candidate.RelativeTransform.Equals(Original.RelativeTransform)
			|| Candidate.BakeGroup != Original.BakeGroup || !Candidate.Validate(Error)) {
			if (Error.IsEmpty()) { Error = TEXT("回读结果与原衣物动作关联不一致，请重新读取。"); } return false;
		}
		const UAnimSequence* Edited = Candidate.BakedAnimation.Get();
		if (!Edited || Edited->GetPackage()->IsDirty()) { Error = TEXT("请先应用并保存修整后的衣物动画，再重新读取并预览。"); return false; }
		FString Filename;
		if (!FPackageName::DoesPackageExist(Original.GetPackage()->GetName(), &Filename) || IFileManager::Get().IsReadOnly(*Filename)) {
			Error = TEXT("原代理必须是已保存且可写的资产。"); return false;
		}
		const FString BackupPackage = Original.GetPackage()->GetName() + TEXT("_BeforeReadback_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		TStrongObjectPtr<UKawaiiClothingProxyMotion> Backup(DuplicateObject<UKawaiiClothingProxyMotion>(&Original, CreatePackage(*BackupPackage), *FPackageName::GetLongPackageAssetName(BackupPackage)));
		Backup->ClearFlags(RF_Transient);
		Backup->SetFlags(RF_Public | RF_Standalone);
		if (!Save(*Backup.Get())) { Error = TEXT("无法保存回读前的代理备份，原代理未更新。"); return false; }
		FAssetRegistryModule::AssetCreated(Backup.Get());
		BackupPath = Backup->GetPathName();
		const FScopedTransaction Transaction(FText::FromString(TEXT("从修整衣物动画更新代理轨迹")));
		Original.Modify();
		CopyPayload(Candidate, Original);
		Original.MarkPackageDirty();
		Original.PostEditChange();
		if (!Save(Original)) {
			CopyPayload(*Backup.Get(), Original);
			Original.MarkPackageDirty();
			Error = TEXT("代理保存失败，内存已恢复原轨迹；备份：") + BackupPath; return false;
		}
		return true;
	}
}

FKawaiiClothingReadbackPreview::FKawaiiClothingReadbackPreview()
	: Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false).ShouldSimulatePhysics(false)) {
	Scene.SetFloorVisibility(false, true);
	BodyComponent = NewObject<UKawaiiPhysicsBakePreviewComponent>();
	ClothingComponent = NewObject<UKawaiiPhysicsBakePreviewComponent>();
	for (auto* Component : {BodyComponent.Get(), ClothingComponent.Get()}) {
		Component->SetDisablePostProcessBlueprint(true);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Component->SetForcedLOD(1);
		Scene.AddComponent(Component, FTransform::Identity);
		Component->SetComponentTickEnabled(false);
	}
}

FKawaiiClothingReadbackPreview::~FKawaiiClothingReadbackPreview() {
	for (auto* Component : {BodyComponent.Get(), ClothingComponent.Get()}) { Scene.RemoveComponent(Component); Component->DestroyComponent(); }
}

void FKawaiiClothingReadbackPreview::Clear() {
	Original = Candidate = nullptr;
	BodyAnimation = nullptr; Profile = nullptr;
	BodyFrames.Reset(); ClothingFrames.Reset(); DisplayedCapsules.Reset(); DisplayedFrame = 0;
	BodyComponent->SetSkeletalMesh(nullptr); ClothingComponent->SetSkeletalMesh(nullptr);
}

bool FKawaiiClothingReadbackPreview::Read(const UKawaiiClothingReadbackSettings& Settings, FString& Error) {
	Clear(); Error.Reset();
	if (!Settings.Motion || !Settings.EditedAnimation) { Error = TEXT("请选择原衣物代理运动及修整后的衣物动画。"); return false; }
	Original = Settings.Motion;
	BodyAnimation = Original->BodyBakedAnimation.LoadSynchronous();
	USkeletalMesh* BodyMesh = Settings.BodyMesh ? Settings.BodyMesh.Get() : (BodyAnimation ? BodyAnimation->GetPreviewMesh() : nullptr);
	if (!BodyAnimation || !BodyMesh || BodyAnimation->GetSkeleton() != BodyMesh->GetSkeleton() || BodyAnimation->IsValidAdditive()) {
		Error = TEXT("请选择与原身体烘焙动画骨架一致的身体网格体，以预览完整角色。"); Clear(); return false;
	}
	Candidate = NewObject<UKawaiiClothingProxyMotion>(GetTransientPackage());
	if (!KawaiiClothingReadback::Build(*Original, *Settings.EditedAnimation, *Candidate, ClothingFrames, Error)) { Clear(); return false; }
	USkeletalMesh* ClothMesh = Candidate->SourceMesh.Get();
	if (int64(BodyMesh->GetRefSkeleton().GetNum() + ClothMesh->GetRefSkeleton().GetNum() + Candidate->BoneNames.Num()) * Candidate->NumFrames > KawaiiClothingProxy::MaxKeys) {
		Error = TEXT("身体与衣物预览合计超过 400 万个骨骼样本。"); Clear(); return false;
	}
	OriginalRevision = Original->MotionRevision;
	BodyRevision = KawaiiClothingReadback::AnimationRevision(BodyAnimation);
	ClothingSkeletonSignature = Candidate->SourceSkeletonSignature;
	IgnoredTargetRoots = Candidate->Definition->IgnoredTargetRootBones;
	BodySkeletonSignature = KawaiiClothingProxy::ComputeSkeletonSignature(BodyMesh->GetRefSkeleton());
	BodyComponent->SetSkeletalMesh(BodyMesh);
	ClothingComponent->SetSkeletalMesh(ClothMesh);
	ClothingComponent->SetWorldTransform(Candidate->RelativeTransform);
	BodyComponent->SetAnimation(BodyAnimation);
	ClothingComponent->SetAnimation(Settings.EditedAnimation);
	BodyComponent->RefreshBoneTransforms(); ClothingComponent->RefreshBoneTransforms();
	if (!FMath::IsFinite(Settings.CorrectionAlpha) || Settings.CorrectionAlpha < 0.f || Settings.CorrectionAlpha > 1.f) {
		Error = TEXT("头发修正权重必须位于 0 到 1 之间。"); Clear(); return false;
	}
	Profile = Settings.CorrectionAlpha > 0.f ? Settings.CorrectionProfile.Get() : nullptr;
	FKawaiiPhysicsBakePreviewCorrection Correction;
	if (Profile && !Correction.Initialize(*Candidate->Definition, *ClothingComponent, Profile, Candidate->SampleRate, Error)) { Clear(); return false; }
	FScopedSlowTask Progress(Candidate->NumFrames, FText::FromString(TEXT("生成身体、修整衣物及代理预览")));
	if (!IsRunningCommandlet()) { Progress.MakeDialog(true); }
	for (int32 Index = 0; Index < Candidate->NumFrames; ++Index) {
		Progress.EnterProgressFrame();
		if (Progress.ShouldCancel()) { Error = TEXT("已取消预览读取，原代理未修改。"); Clear(); return false; }
		auto& Body = BodyFrames.AddDefaulted_GetRef();
		if (!KawaiiClothingReadback::Decode(*BodyAnimation, *BodyMesh, float(Index) / Candidate->SampleRate, Body, Error)) { Clear(); return false; }
		if (Profile) {
			const auto& Clothing = ClothingFrames[Index];
			BodyComponent->DisplayPose(Body.Pose, Body.MaterialCurves, Body.MorphCurves);
			ClothingComponent->DisplayPose(Clothing.Pose, Clothing.MaterialCurves, Clothing.MorphCurves);
			TArray<FTransform3f> Corrected;
			if (!Correction.Evaluate(*BodyComponent, *ClothingComponent, 1.f / Candidate->SampleRate, Settings.CorrectionAlpha, Corrected, Error)) { Clear(); return false; }
			Body.Pose = MoveTemp(Corrected);
		}
	}
	ShowFrame(0);
	return true;
}

bool FKawaiiClothingReadbackPreview::IsCurrent(FString& Error) const {
	if (!Candidate || !Original || ClothingFrames.IsEmpty()) { Error = TEXT("请先读取并预览修整动画。"); return false; }
	if (Original->MotionRevision != OriginalRevision || Original->Definition != Candidate->Definition
		|| Original->BodyBakedAnimation.Get() != BodyAnimation || !Candidate->Definition
		|| Candidate->DefinitionSignature != Candidate->Definition->ComputeSignature()
		|| Candidate->Definition->IgnoredTargetRootBones != IgnoredTargetRoots
		|| KawaiiClothingReadback::AnimationRevision(BodyAnimation) != BodyRevision
		|| KawaiiClothingReadback::AnimationRevision(Candidate->BakedAnimation.Get()) != Candidate->BakedAnimationDataGuid
		|| KawaiiClothingProxy::ComputeSkeletonSignature(ClothingComponent->GetSkeletalMeshAsset()->GetRefSkeleton()) != ClothingSkeletonSignature
		|| KawaiiClothingProxy::ComputeSkeletonSignature(BodyComponent->GetSkeletalMeshAsset()->GetRefSkeleton()) != BodySkeletonSignature) {
		Error = TEXT("动画、代理或骨架已改变，请重新读取并预览后更新。"); return false;
	}
	return Candidate->ValidateAnimationRevisions(Error);
}

void FKawaiiClothingReadbackPreview::ShowFrame(int32 Frame) {
	if (ClothingFrames.IsEmpty()) { return; }
	DisplayedFrame = FMath::Clamp(Frame, 0, ClothingFrames.Num() - 1);
	const auto& Clothing = ClothingFrames[DisplayedFrame]; const auto& Body = BodyFrames[DisplayedFrame];
	BodyComponent->DisplayPose(Body.Pose, Body.MaterialCurves, Body.MorphCurves);
	ClothingComponent->DisplayPose(Clothing.Pose, Clothing.MaterialCurves, Clothing.MorphCurves);
	FString Error;
	KawaiiClothingProxy::SampleBlended({{Candidate, float(DisplayedFrame) / Candidate->SampleRate, 1.f}}, Candidate->RelativeTransform, DisplayedCapsules, Error);
	Scene.GetWorld()->SendAllEndOfFrameUpdates();
}

void FKawaiiClothingReadbackPreview::Draw(FPrimitiveDrawInterface* PDI) const {
	for (const auto& Capsule : DisplayedCapsules) {
		const FVector Axis = Capsule.End - Capsule.Start;
		if (Axis.IsNearlyZero()) { DrawWireSphere(PDI, Capsule.Start, FLinearColor::Green, Capsule.Radius, 16, SDPG_Foreground); }
		else {
			const FVector Z = Axis.GetSafeNormal(); FVector X, Y; Z.FindBestAxisVectors(X, Y);
			DrawWireCapsule(PDI, (Capsule.Start + Capsule.End) * .5, X, Y, Z, FLinearColor::Green, Capsule.Radius, Axis.Length() * .5 + Capsule.Radius, 16, SDPG_Foreground);
		}
	}
}

bool FKawaiiClothingReadbackPreview::Publish(FString& BackupPath, FString& Error) {
	if (!IsCurrent(Error) || !KawaiiClothingReadback::Publish(*Original, *Candidate, BackupPath, Error)) { return false; }
	OriginalRevision = Original->MotionRevision;
	return true;
}

FBox FKawaiiClothingReadbackPreview::GetBounds() const { return BodyComponent->Bounds.GetBox() + ClothingComponent->Bounds.GetBox(); }
void FKawaiiClothingReadbackPreview::AddReferencedObjects(FReferenceCollector& Collector) {
	Collector.AddReferencedObject(BodyComponent); Collector.AddReferencedObject(ClothingComponent);
	Collector.AddReferencedObject(Original); Collector.AddReferencedObject(Candidate); Collector.AddReferencedObject(BodyAnimation); Collector.AddReferencedObject(Profile);
}
