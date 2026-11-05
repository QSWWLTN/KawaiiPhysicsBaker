#include "KawaiiPhysicsCalibrationSession.h"

#include "Curves/CurveFloat.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EdGraph/EdGraphPin.h"
#include "GameFramework/Actor.h"
#include "PreviewScene.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "KawaiiPhysicsCalibrationProfile.h"
#include "KawaiiPhysicsLimitsDataAsset.h"
#include "KawaiiPhysicsBoneConstraintsDataAsset.h"
#include "AnimGraphNode_KawaiiPhysics.h"

#include "KawaiiPhysicsCalibrationSettings.h"
#include "KawaiiCalibrationAnimInstance.h"

namespace {
	FGuid CachedAnimationGuid(const UAnimSequence* Animation) {
		// UE 5.5 updates this cache on model edits; hashing all tracks every Tick is unnecessarily expensive.
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Animation->GetRawDataGuid();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}

	void ComponentPose(const FReferenceSkeleton& Ref, const TArray<FTransform>& Local, TArray<FTransform>& CS) {
		CS.SetNum(Local.Num());
		for (int32 I = 0; I < Local.Num(); ++I) {
			int32 P = Ref.GetParentIndex(I);
			CS[I] = P < 0 ? Local[I] : Local[I] * CS[P];
		}
	}

	UAnimGraphNode_KawaiiPhysics* FindNode(UAnimBlueprint* Blueprint, FName Root) {
		if (!Blueprint) {
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		UAnimGraphNode_KawaiiPhysics* Found = nullptr;
		for (auto* Graph : Graphs) {
			for (auto Node : Graph->Nodes) {
				if (auto* K = Cast<UAnimGraphNode_KawaiiPhysics>(Node)) {
					if (Root.IsNone() || K->Node.RootBone.BoneName == Root) {
						if (Found) {
							return nullptr;
						}
						Found = K;
					}
				}
			}
		}
		return Found;
	}

	bool InSubtree(const FReferenceSkeleton& Ref, int32 Bone, int32 Root) {
		for (int32 I = Bone; I >= 0; I = Ref.GetParentIndex(I)) {
			if (I == Root) {
				return true;
			}
		}
		return false;
	}

	bool FinitePose(const TArray<FTransform>& Pose) {
		for (const auto& T : Pose) {
			if (T.ContainsNaN() || !T.IsRotationNormalized()) {
				return false;
			}
		}
		return true;
	}

	uint32 NodeHash(UAnimBlueprint* Blueprint, FName Root) {
		auto* Node = FindNode(Blueprint, Root);
		if (!Node) {
			return 0;
		}
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		FObjectAndNameAsStringProxyArchive Archive(Writer, false);
		FAnimNode_KawaiiPhysics::StaticStruct()->SerializeItem(Archive, &Node->Node, nullptr);
		const auto SerializeDependency = [&](UObject* Object) {
			if (Object) {
				Object->Serialize(Archive);
			}
		};
		SerializeDependency(Node->Node.LimitsDataAsset);
		SerializeDependency(Node->Node.BoneConstraintsDataAsset);
		SerializeDependency(Node->Node.PhysicsAssetForLimits);
		SerializeDependency(Node->Node.CalibrationProfile);
		if (Node->Node.PhysicsAssetForLimits) {
			for (auto BodySetup : Node->Node.PhysicsAssetForLimits->SkeletalBodySetups) {
				SerializeDependency(BodySetup);
			}
		}
		for (const auto* Curve :
			 {&Node->Node.DampingCurveData,
			  &Node->Node.StiffnessCurveData,
			  &Node->Node.RadiusCurveData,
			  &Node->Node.WorldDampingLocationCurveData,
			  &Node->Node.WorldDampingRotationCurveData,
			  &Node->Node.LimitAngleCurveData})
		{
			SerializeDependency(Curve->ExternalCurve);
		}
		return FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
	}
} // namespace

FKawaiiPhysicsCalibrationSession::FKawaiiPhysicsCalibrationSession() = default;

FKawaiiPhysicsCalibrationSession::~FKawaiiPhysicsCalibrationSession() {
	Cancel();
}

bool FKawaiiPhysicsCalibrationSession::Start(UKawaiiPhysicsCalibrationSettings* InSettings, FString& OutError) {
	if (IsRunning()) {
		OutError = TEXT("请先取消当前校准。");
		return false;
	}
	Cancel();
	Result = nullptr;
	bResultAccepted = false;
	OriginalProfile = nullptr;
	Candidates.Reset();
	ReportPath.Reset();
	WorkingProfile = nullptr;
	Samples.Reset();
	GPUInput = FKawaiiCalibrationGPUInput();
	SkinPoints.Reset();
	SurfacePointIndices.Reset();
	SurfacePointDepths.Reset();
	bSurfacePointsSelected = false;
	Verified.Reset();
	VerifiedProfiles.Reset();
	GPUScores.Reset();
	Finalists.Reset();
	Settings = InSettings ? DuplicateObject<UKawaiiPhysicsCalibrationSettings>(InSettings, GetTransientPackage()) : nullptr;
	if (!Prepare(OutError)) {
		Fail(OutError);
		return false;
	}
	bSurfacePointsSelected = SkinPoints.IsEmpty();
	SurfacePointDepths.Init(0., SkinPoints.Num());
	MotionIndex = 0;
	StartTime = FPlatformTime::Seconds();
	Stage = EStage::Sampling;
	Status = TEXT("正在采样人物动作及动画切换，源资产保持不变。");
	return true;
}

bool FKawaiiPhysicsCalibrationSession::CreateManualProfile(UKawaiiPhysicsCalibrationSettings* InSettings, FString& OutError) {
	if (IsRunning() || !InSettings) {
		OutError = TEXT("请先结束当前校准，再保存手动配置。");
		return false;
	}
	auto* Copy = DuplicateObject<UKawaiiPhysicsCalibrationSettings>(InSettings, GetTransientPackage());
	FString Source;
	if (!Copy->ResolveConstraints(Source, OutError)) {
		return false;
	}
	Cancel();
	Settings = Copy;
	Settings->PhysicsRoot = Settings->FindPhysicsNode()->Node.RootBone.BoneName;
	SourceNodeHash = NodeHash(Settings->ClothingBlueprint, Settings->PhysicsRoot);
	ReportPath.Reset();
	Result = NewObject<UKawaiiPhysicsCalibrationProfile>();
	bResultAccepted = false;
	Result->TargetRoot = Settings->PhysicsRoot;
	Settings->CopyConstraintsToProfile(*Result);
	Result->ValidationSummary = TEXT("手动配置，未经离线校准或穿模验收；保留原节点阻尼、刚度和骨骼半径，只应用编辑后的碰撞体与骨链范围。未生成蒙皮接触点。");
	Status = Result->ValidationSummary;
	Stage = EStage::Complete;
	return true;
}

void FKawaiiPhysicsCalibrationSession::Tick() {
	if (!IsRunning()) {
		return;
	}
	FString Error;
	if (FPlatformTime::Seconds() - StartTime > 1800) {
		Fail(TEXT("校准超过 30 分钟，已超时。请减少测试动作或候选数量。"));
		return;
	}
	if (NodeHash(Settings->ClothingBlueprint, Settings->PhysicsRoot) != SourceNodeHash) {
		Fail(TEXT("源物理节点已更改，请重新开始校准。"));
		return;
	}
	for (int32 I = 0; I < Settings->Motions.Num(); ++I) {
		if (CachedAnimationGuid(Settings->Motions[I].Animation) != SourceAnimationGuids[I]) {
			Fail(TEXT("源动画已更改，请重新开始校准。"));
			return;
		}
	}
	if (Stage == EStage::Sampling) {
		if (!SampleMotion(MotionIndex, Error)) {
			Fail(Error);
			return;
		}
		if (++MotionIndex >= Settings->Motions.Num()) {
			if (Settings->bSearchNodeSettings) {
				Finalists.Reset();
				for (int32 I = 0; I < Candidates.Num(); ++I) {
					Finalists.Add(I);
				}
				FinalistIndex = 0;
				Stage = EStage::CPU;
				if (!BeginCPU(Finalists[0], Error)) {
					Fail(Error);
				}
				return;
			}
			GPU = FKawaiiCalibrationGPU::Start(GPUInput, Error);
			if (!GPU) {
				Fail(Error);
				return;
			}
			Stage = EStage::GPU;
			Status = TEXT("GPU 正在使用筛选模型模拟候选参数，随后将通过原始 CPU 模拟复验入选配置。");
		}
		return;
	}
	if (Stage == EStage::GPU) {
		GPU->Tick();
		if (!GPU->IsDone()) {
			return;
		}
		if (!GPU->GetError().IsEmpty()) {
			Fail(GPU->GetError());
			return;
		}
		GPUScores = GPU->GetScores();
		GPU.Reset();
		TArray<int32> Ranked;
		for (int32 I = 1; I < GPUScores.Num(); ++I) {
			const auto& S = GPUScores[I];
			if (FMath::IsFinite(S.X) && FMath::IsFinite(S.Y) && FMath::IsFinite(S.Z) && FMath::IsFinite(S.W)) {
				Ranked.Add(I);
			}
		}
		int32 TrainingFrames = 0, FreeBones = 0;
		for (const auto& S : Samples) {
			TrainingFrames += S.bScore && !S.bValidation;
		}
		for (int32 P : ParticleParents) {
			FreeBones += P >= 0;
		}
		const double Count = FMath::Max(1, TrainingFrames * FreeBones);
		const auto Value = [&](int32 I) {
			const auto& S = GPUScores[I];
			return Settings->PenetrationWeight * (S.X / Count + S.Y * S.Y) + Settings->DeviationWeight * S.Z / Count + Settings->JitterWeight * S.W / Count
				+ Settings->IterationCostWeight * GPUInput.Candidates[I].W;
		};
		Ranked.Sort([&](int32 A, int32 B) { return Value(A) < Value(B); });
		Finalists = {0};
		for (int32 I = 0; I < FMath::Min(Settings->CPUFinalists, Ranked.Num()); ++I) {
			Finalists.Add(Ranked[I]);
		}
		FinalistIndex = 0;
		Stage = EStage::CPU;
		if (!BeginCPU(Finalists[0], Error)) {
			Fail(Error);
		}
		return;
	}
	if (Stage == EStage::CPU) {
		const double Deadline = FPlatformTime::Seconds() + .008;
		do {
			if (!StepCPU(Error)) {
				Fail(Error);
				return;
			}
		} while (SampleIndex < Samples.Num() && FPlatformTime::Seconds() < Deadline);
		if (bSurfacePointsSelected) {
			Status = FString::Printf(
				TEXT("CPU 复验：配置 %d/%d，帧 %d/%d（包含动画切换和独立验证片段）。"),
				FinalistIndex + 1,
				Finalists.Num(),
				SampleIndex,
				Samples.Num()
			);
		} else {
			Status = TEXT("CPU 正在定位训练动作中的蒙皮接触区域，随后统一复验所有候选。");
		}
		if (SampleIndex >= Samples.Num()) {
			if (!bSurfacePointsSelected) {
				SelectSurfacePoints();
				if (!BeginCPU(Finalists[0], Error)) {
					Fail(Error);
				}
				return;
			}
			Current.Rank = Rank(Current.Training, Current.Candidate);
			Verified.Add(Current);
			VerifiedProfiles.Add(WorkingProfile);
			if (++FinalistIndex >= Finalists.Num()) {
				Finish();
			} else if (!BeginCPU(Finalists[FinalistIndex], Error)) {
				Fail(Error);
			}
		}
	}
}

void FKawaiiPhysicsCalibrationSession::Cancel() {
	if (IsRunning()) {
		Stage = EStage::Cancelled;
		Status = TEXT("已取消，源资产未更改。");
	}
	GPU.Reset();
	ClearScene();
}

bool FKawaiiPhysicsCalibrationSession::IsRunning() const {
	return Stage == EStage::Sampling || Stage == EStage::GPU || Stage == EStage::CPU;
}

float FKawaiiPhysicsCalibrationSession::GetProgress() const {
	if (Stage == EStage::Sampling) {
		return .15f * MotionIndex / FMath::Max(1, Settings->Motions.Num());
	}
	if (Stage == EStage::GPU) {
		return .15f + .45f * GPU->GetProgress();
	}
	if (Stage == EStage::CPU) {
		const float Fraction = float(SampleIndex) / FMath::Max(1, Samples.Num());
		if (Settings->bSearchNodeSettings) {
			if (!bSurfacePointsSelected) {
				return .15f + .05f * Fraction;
			}
			const float Begin = SkinPoints.IsEmpty() ? .15f : .2f;
			return Begin + (1.f - Begin) * (FinalistIndex + Fraction) / FMath::Max(1, Finalists.Num());
		}
		return bSurfacePointsSelected ? .65f + .35f * (FinalistIndex + Fraction) / FMath::Max(1, Finalists.Num()) : .6f + .05f * Fraction;
	}
	return Stage == EStage::Complete ? 1.f : 0.f;
}

bool FKawaiiPhysicsCalibrationSession::SaveResult(FString& OutError, bool bAllowUnaccepted) {
	if (Stage != EStage::Complete || !Result) {
		OutError = TEXT("请等待 CPU 复验完成后再保存。");
		return false;
	}
	if (!bResultAccepted && !bAllowUnaccepted) {
		OutError = TEXT("当前最佳候选尚未达到验收阈值。请使用“保存最佳候选（未达标）”明确保存供试用的参数。");
		return false;
	}
	if (Result->GetOutermost() != GetTransientPackage()) {
		return true;
	}
	FString Directory = Settings->OutputDirectory.Path;
	Directory.RemoveFromEnd(TEXT("/"));
	if (!Directory.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Directory + TEXT("/Probe"))) {
		OutError = TEXT("输出目录必须是有效的 /Game 子目录。");
		return false;
	}
	const FString PackageName = Directory / TEXT("DA_KawaiiCalibration_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	UPackage* Package = CreatePackage(*PackageName);
	auto* Asset = DuplicateObject<UKawaiiPhysicsCalibrationProfile>(Result, Package, *FPackageName::GetLongPackageAssetName(PackageName));
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *Filename, Args)) {
		OutError = TEXT("无法保存新的校准配置。");
		return false;
	}
	FAssetRegistryModule::AssetCreated(Asset);
	Result = Asset;
	Status = TEXT("已保存 CPU 运行时配置：") + Result->GetPathName();
	return true;
}

bool FKawaiiPhysicsCalibrationSession::ApplyToBlueprint(FString& OutError) {
	if (!Result || Result->GetOutermost() == GetTransientPackage()) {
		OutError = TEXT("请先保存新配置，再应用到蓝图。");
		return false;
	}
	auto* Node = FindNode(Settings->ClothingBlueprint, Settings->PhysicsRoot);
	if (!Node) {
		OutError = TEXT("目标节点已不存在，或无法唯一确定。");
		return false;
	}
	if (NodeHash(Settings->ClothingBlueprint, Settings->PhysicsRoot) != SourceNodeHash) {
		OutError = TEXT("源物理节点已更改，请重新校准后再应用。");
		return false;
	}
	FScopedTransaction Transaction(NSLOCTEXT("KawaiiCalibration", "Apply", "应用 Kawaii 校准配置"));
	Settings->ClothingBlueprint->Modify();
	Node->Modify();
	if (Result->bHasOptimizedNodeSettings) {
		KawaiiCalibrationSearch::Apply(Result->OptimizedNodeSettings, Node->Node);
	}
	Node->Node.CalibrationProfile = Result;
	FBlueprintEditorUtils::MarkBlueprintAsModified(Settings->ClothingBlueprint);
	FKismetEditorUtilities::CompileBlueprint(Settings->ClothingBlueprint);
	if (Settings->ClothingBlueprint->Status == BS_Error) {
		OutError = TEXT("蓝图编译失败。本次配置赋值可以撤销，蓝图尚未保存。");
		return false;
	}
	Status = (bResultAccepted ? FString() : FString(TEXT("此候选未达到验收阈值，当前仅供试用。"))) + TEXT("已将配置和搜索得到的节点参数赋给所选 Kawaii 节点。请检查并保存衣物动画蓝图。");
	return true;
}

void FKawaiiPhysicsCalibrationSession::AddReferencedObjects(FReferenceCollector& C) {
	C.AddReferencedObject(Settings);
	C.AddReferencedObject(WorkingProfile);
	C.AddReferencedObject(Result);
	C.AddReferencedObject(OriginalProfile);
	C.AddReferencedObjects(VerifiedProfiles);
	C.AddReferencedObject(Body);
	C.AddReferencedObject(Clothing);
	C.AddReferencedObject(BodyInstance);
	C.AddReferencedObject(ClothingInstance);
	C.AddReferencedObject(Actor);
}

bool FKawaiiPhysicsCalibrationSession::Prepare(FString& OutError) {
	if (!Settings || !Settings->BodyMesh || !Settings->ClothingMesh || !Settings->ClothingBlueprint || Settings->Motions.IsEmpty()) {
		OutError = TEXT("请选择人物与衣物网格体、衣物动画蓝图和测试动作。");
		return false;
	}
	if (Settings->SampleRate < 30 || Settings->SampleRate > 120 || Settings->WarmUpFrames < 0 || Settings->WarmUpFrames > 240
		|| (!Settings->bSearchNodeSettings
			&& (Settings->CandidateCount < 8 || Settings->CandidateCount > 1024 || Settings->CPUFinalists < 1 || Settings->CPUFinalists > 16))
		|| Settings->MaxContactIterations < 1 || Settings->MaxContactIterations > 8 || Settings->SkinSampleCount < 0 || Settings->SkinSampleCount > 512)
	{
		OutError = TEXT("校准采样或候选数量等设置超出支持范围。");
		return false;
	}
	if (Settings->ClothingTransform.ContainsNaN() || !Settings->ClothingTransform.IsRotationNormalized()
		|| Settings->ClothingTransform.GetScale3D().GetMin() <= 0)
	{
		OutError = TEXT("衣物相对变换必须使用有限正数缩放和归一化旋转。");
		return false;
	}
	if (Settings->ClothingBlueprint->TargetSkeleton != Settings->ClothingMesh->GetSkeleton()) {
		OutError = TEXT("衣物动画蓝图与衣物网格体的骨架不匹配。");
		return false;
	}
	auto* Node = FindNode(Settings->ClothingBlueprint, Settings->PhysicsRoot);
	if (!Node) {
		OutError = TEXT("请通过物理根骨骼唯一确定衣物蓝图中的 Kawaii 节点。若节点位于链接或嵌套蓝图中，请直接选择该蓝图。");
		return false;
	}
	Template = Node->Node;
	OriginalTemplate = Node->Node;
	OriginalProfile = Node->Node.CalibrationProfile;
	if (Settings->bSearchNodeSettings) {
		for (const UEdGraphPin* Pin : Node->Pins) {
			if (!Pin || Pin->Direction != EGPD_Input || Pin->LinkedTo.IsEmpty()) {
				continue;
			}
			const UEdGraphPin* RootPin = Pin;
			while (RootPin->ParentPin) {
				RootPin = RootPin->ParentPin;
			}
			const FString Name = RootPin->PinName.ToString();
			const bool bSearched = Name.StartsWith(TEXT("PhysicsSettings")) || Name == TEXT("CalibrationProfile") || Name.Contains(TEXT("CurveData"))
				|| Name.Contains(TEXT("BoneConstraint")) || Name.Contains(TEXT("BoneSubdivision")) || Name == TEXT("bAutoAddChildDummyBoneConstraint");
			if (bSearched) {
				OutError = FString::Printf(
					TEXT("输入引脚 %s 已连接，会覆盖节点默认参数；当前自动搜索无法可靠应用此节点。请先将该参数作为节点常量配置。"),
					*Pin->PinName.ToString()
				);
				return false;
			}
		}
	}
	Settings->PhysicsRoot = Template.RootBone.BoneName;
	SourceNodeHash = NodeHash(Settings->ClothingBlueprint, Settings->PhysicsRoot);
	if (Template.bAllowWorldCollision || Template.bEnableWind || !Template.CustomExternalForces.IsEmpty() || !Template.ExternalForces.IsEmpty()
		|| Template.bUseSharedCollision || Template.bSharedCollisionSource || !Template.SyncBones.IsEmpty())
	{
		OutError = TEXT("离线校准要求节点关闭世界碰撞、风力、外力数组、共享碰撞和同步骨骼。请使用仅含确定性局部约束的蓝图副本进行校准，原蓝图不会被修改。");
		return false;
	}
	if (Template.SimulationSpace != EKawaiiPhysicsSimulationSpace::ComponentSpace || !Template.SimpleExternalForce.IsNearlyZero() || Template.bUseLegacyGravity
		|| Template.bUseDefaultGravityZProjectSetting)
	{
		OutError = TEXT("GPU 筛选目前要求使用组件空间模拟、显式重力，并关闭旧版重力、项目默认重力和简单外力。");
		return false;
	}
	// 只清除临时节点副本的旧配置，按原始参数重新搜索，不叠加旧倍率或修改源蓝图。
	Template.CalibrationProfile = nullptr;
	// Keep the authored solver rate: CPU verification must match the runtime node's substeps/stiffness response.
	Template.bNeedWarmUp = false;
	Template.bUseWarmUpWhenResetDynamics = false;
	Template.bUpdatePhysicsSettingsInGame = true;
	const auto& Ref = Settings->ClothingMesh->GetRefSkeleton();
	TArray<FTransform> RefCS;
	ComponentPose(Ref, Ref.GetRefBonePose(), RefCS);
	InverseRefPose = RefCS;
	for (auto& T : InverseRefPose) {
		T = T.Inverse();
	}
	TArray<FName> Roots;
	Roots.Add(Template.RootBone.BoneName);
	for (const auto& R : Template.AdditionalRootBones) {
		Roots.Add(R.RootBone.BoneName);
	}
	ParticleMeshBones.Reset();
	ParticleParents.Reset();
	TMap<int32, int32> MeshToParticle;
	for (int32 I = 0; I < Ref.GetNum(); ++I) {
		bool bIncluded = false;
		int32 MatchingRoot = -1;
		for (int32 R = 0; R < Roots.Num(); ++R) {
			const int32 Root = Ref.FindBoneIndex(Roots[R]);
			if (Root < 0) {
				OutError = TEXT("网格体中缺少指定的物理根骨骼。");
				return false;
			}
			if (!InSubtree(Ref, I, Root)) {
				continue;
			}
			const auto& Excludes = R > 0 && Template.AdditionalRootBones[R - 1].bUseOverrideExcludeBones ? Template.AdditionalRootBones[R - 1].OverrideExcludeBones : Template.ExcludeBones;
			bool bExcluded = false;
			for (const auto& Ex : Excludes) {
				if (InSubtree(Ref, I, Ref.FindBoneIndex(Ex.BoneName))) {
					bExcluded = true;
				}
			}
			if (!bExcluded) {
				if (bIncluded) {
					OutError = TEXT("不支持模拟范围相互重叠的物理根骨骼。");
					return false;
				}
				bIncluded = true;
				MatchingRoot = Root;
			}
		}
		if (!bIncluded) {
			continue;
		}
		const int32* Parent = MeshToParticle.Find(Ref.GetParentIndex(I));
		const int32 Particle = ParticleMeshBones.Add(I);
		ParticleParents.Add(I == MatchingRoot || !Parent ? -1 : *Parent);
		MeshToParticle.Add(I, Particle);
	}
	if (ParticleMeshBones.Num() < 2 || ParticleMeshBones.Num() > 512) {
		OutError = TEXT("请选择包含 2 至 512 根实际骨骼的物理模拟子树。");
		return false;
	}
	TArray<double> Lengths;
	Lengths.SetNumZeroed(ParticleMeshBones.Num());
	TArray<int32> RootIds;
	RootIds.SetNum(Lengths.Num());
	TMap<int32, double> MaxLengths;
	for (int32 I = 0; I < Lengths.Num(); ++I) {
		int32 P = ParticleParents[I];
		RootIds[I] = P < 0 ? I : RootIds[P];
		if (P >= 0) {
			Lengths[I] = Lengths[P] + FVector::Distance(RefCS[ParticleMeshBones[I]].GetTranslation(), RefCS[ParticleMeshBones[P]].GetTranslation());
		}
		MaxLengths.FindOrAdd(RootIds[I]) = FMath::Max(MaxLengths.FindRef(RootIds[I]), Lengths[I]);
	}
	ParticleRanges.Reset();
	for (int32 I = 0; I < Lengths.Num(); ++I) {
		float L = Lengths[I] / FMath::Max(.001, MaxLengths.FindRef(RootIds[I]));
		const FName Root = Ref.GetBoneName(ParticleMeshBones[RootIds[I]]);
		const auto* Limit = Settings->ChainLimits.FindByPredicate([&](const auto& Entry) { return Entry.RootBone == Root; });
		FVector2f Range(-1, 0);
		if (Limit && (Limit->bOverrideAngle || Limit->bOverridePlane)) {
			const float Angle = Limit->bOverrideAngle ? Limit->LimitAngle : Template.PhysicsSettings.LimitAngle;
			Range.X = FMath::Clamp(Angle * Template.LimitAngleCurveData.GetRichCurveConst()->Eval(L, 1.f), 0.f, 180.f);
			Range.Y = static_cast<uint8>(Limit->bOverridePlane ? Limit->PlanarConstraint : Template.PlanarConstraint);
		}
		ParticleRanges.Add(Range);
		GPUInput.Bones.Add(FVector4f(
			ParticleParents[I],
			Template.PhysicsSettings.Damping * Template.DampingCurveData.GetRichCurveConst()->Eval(L, 1),
			Template.PhysicsSettings.Stiffness * Template.StiffnessCurveData.GetRichCurveConst()->Eval(L, 1),
			Template.PhysicsSettings.Radius * Template.RadiusCurveData.GetRichCurveConst()->Eval(L, 1)
		));
	}
	if (!Settings->ResolveConstraints(CollisionSource, OutError)) {
		return false;
	}
	bool bHasTraining = false;
	int64 EstimatedFrames = 0;
	for (const auto& M : Settings->Motions) {
		if (!M.Animation || M.Animation->GetSkeleton() != Settings->BodyMesh->GetSkeleton() || M.Animation->IsValidAdditive() || !FMath::IsFinite(M.PlayRate)
			|| M.PlayRate < .25f || M.PlayRate > 3 || !FMath::IsFinite(M.Animation->GetPlayLength()) || M.Animation->GetPlayLength() <= 0
			|| !FMath::IsFinite(M.BlendInSeconds) || M.BlendInSeconds < 0 || M.BlendInSeconds > 2 || M.LocalOffset.ContainsNaN()
			|| M.LocalRotation.ContainsNaN())
		{
			OutError = TEXT("每个测试动作必须是匹配人物骨架的非叠加动画，且时长、播放倍率、过渡时长和 IK 偏移必须有效。");
			return false;
		}
		if (!M.OffsetBone.IsNone() && Settings->BodyMesh->GetRefSkeleton().FindBoneIndex(M.OffsetBone) < 0) {
			OutError = TEXT("人物骨架中缺少指定的 IK 压力测试骨骼。");
			return false;
		}
		bHasTraining |= !M.bValidationOnly;
		EstimatedFrames += FMath::CeilToInt(M.Animation->GetPlayLength() * Settings->SampleRate / M.PlayRate) + 1 + Settings->WarmUpFrames;
	}
	const int64 CachedBytes = EstimatedFrames * (Settings->BodyMesh->GetRefSkeleton().GetNum() + Ref.GetNum()) * sizeof(FTransform);
	if (!bHasTraining || EstimatedFrames > 20000 || EstimatedFrames * ParticleMeshBones.Num() > 8000000 || CachedBytes > 512ll * 1024 * 1024) {
		OutError = TEXT("至少需要一个参与搜索的动作片段；总量不得超过 20,000 帧、800 万个粒子样本和 512 MiB 姿势缓存。");
		return false;
	}
	if (!FMath::IsFinite(Settings->MaxAcceptedPenetration) || Settings->MaxAcceptedPenetration < 0.f || !FMath::IsFinite(Settings->MaxAcceptedLengthError)
		|| Settings->MaxAcceptedLengthError < 0.f)
	{
		OutError = TEXT("验收阈值必须是有限非负数。");
		return false;
	}
	const auto ValidRange = [](const FVector2D& R, double Min) { return FMath::IsFinite(R.X) && FMath::IsFinite(R.Y) && R.X >= Min && R.Y <= 4 && R.X <= R.Y; };
	if (!Settings->bSearchNodeSettings
		&& (!ValidRange(Settings->DampingScaleRange, .1) || !ValidRange(Settings->StiffnessScaleRange, .1) || !ValidRange(Settings->RadiusScaleRange, .25)))
	{
		OutError = TEXT("搜索范围无效：阻尼和刚度倍率必须在 0.1 至 4 之间，半径倍率必须在 0.25 至 4 之间。");
		return false;
	}
	for (float Value :
		 {Settings->PenetrationWeight,
		  Settings->DeviationWeight,
		  Settings->JitterWeight,
		  Settings->IterationCostWeight,
		  Settings->SoftMargin,
		  Settings->SoftResponse})
	{
		if (!FMath::IsFinite(Value) || Value < 0) {
			OutError = TEXT("评分和碰撞参数必须是有限非负数。");
			return false;
		}
	}
	if (Settings->SoftMargin > 5 || Settings->SoftResponse > 100 || Template.Gravity.ContainsNaN()) {
		OutError = TEXT("碰撞参数必须符合运行时限制：柔性缓冲距离不超过 5 厘米，柔性响应速度不超过 100；重力必须是有限数值。");
		return false;
	}
	SourceAnimationGuids.Reset();
	for (const auto& Motion : Settings->Motions) {
		SourceAnimationGuids.Add(CachedAnimationGuid(Motion.Animation));
	}
	GPUInput.CapsuleCount = Settings->BodyCapsules.Num();
	GPUInput.DeltaTime = 1.f / Settings->SampleRate;
	GPUInput.Gravity = FVector3f(Template.Gravity);
	GPUInput.SoftMargin = Settings->SoftMargin;
	GPUInput.SoftResponse = Settings->SoftResponse;
	if (Settings->bSearchNodeSettings) {
		if (!KawaiiCalibrationSearch::Generate(*Settings, OriginalTemplate, Candidates, OutError)) {
			return false;
		}
		if (Candidates.IsEmpty() || !Candidates[0].bOriginalBaseline) {
			OutError = TEXT("原生搜索必须包含原始节点基准候选。");
			return false;
		}
	} else {
		FRandomStream Random(Settings->Seed);
		for (int32 I = 0; I < Settings->CandidateCount; ++I) {
			auto& Candidate = Candidates.AddDefaulted_GetRef();
			KawaiiCalibrationSearch::Capture(Template, Candidate.NodeSettings);
			Candidate.Label = FString::Printf(TEXT("GPU_%d"), I);
			if (I > 0) {
				const auto Pick = [&](const FVector2D& R) { return Random.FRandRange(R.X, R.Y); };
				Candidate.ProfileParameters = FVector4f(
					Pick(Settings->DampingScaleRange),
					Pick(Settings->StiffnessScaleRange),
					Pick(Settings->RadiusScaleRange),
					Random.RandRange(1, Settings->MaxContactIterations)
				);
			}
		}
	}
	for (const auto& Candidate : Candidates) {
		GPUInput.Candidates.Add(Candidate.ProfileParameters);
	}
	// Stable LOD0 skin samples, only vertices influenced by the selected physics subtrees.
	if (Settings->SkinSampleCount > 0) {
		if (auto* Render = Settings->ClothingMesh->GetResourceForRendering()) {
			if (Render->LODRenderData.Num()) {
				const auto& LOD = Render->LODRenderData[0];
				const auto* Weights = LOD.GetSkinWeightVertexBuffer();
				TArray<FSkinPoint> Eligible;
				if (Weights && Weights->GetNeedsCPUAccess()) {
					for (const auto& Section : LOD.RenderSections) {
						for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices; ++V) {
							FSkinPoint Point;
							Point.Position = FVector(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(V));
							bool bRelevant = false;
							for (uint32 W = 0; W < Weights->GetMaxBoneInfluences(); ++W) {
								float Weight = Weights->GetBoneWeight(V, W) / 65535.f;
								if (Weight <= 0) {
									continue;
								}
								int32 LocalBone = Weights->GetBoneIndex(V, W);
								if (!Section.BoneMap.IsValidIndex(LocalBone)) {
									continue;
								}
								int32 Bone = Section.BoneMap[LocalBone];
								Point.Bones.Add(Bone);
								Point.Weights.Add(Weight);
								bRelevant |= MeshToParticle.Contains(Bone);
							}
							if (bRelevant) {
								Eligible.Add(MoveTemp(Point));
							}
						}
					}
				}
				const int32 Count = FMath::Min(Settings->SkinSampleCount, Eligible.Num());
				for (int32 I = 0; I < Count; ++I) {
					SkinPoints.Add(MoveTemp(Eligible[int64(I) * Eligible.Num() / Count]));
				}
			}
		}
	}
	if (Settings->SkinSampleCount > 0 && SkinPoints.IsEmpty()) {
		if (const auto* Model = Settings->ClothingMesh->GetImportedModel()) {
			if (Model->LODModels.Num()) {
				TArray<FSkinPoint> Eligible;
				for (const auto& Section : Model->LODModels[0].Sections) {
					for (const auto& V : Section.SoftVertices) {
						FSkinPoint Point;
						Point.Position = FVector(V.Position);
						bool bRelevant = false;
						for (int32 I = 0; I < UE_ARRAY_COUNT(V.InfluenceBones); ++I) {
							const float Weight = V.InfluenceWeights[I] / 65535.f;
							const int32 LocalBone = V.InfluenceBones[I];
							if (Weight <= 0 || !Section.BoneMap.IsValidIndex(LocalBone)) {
								continue;
							}
							const int32 Bone = Section.BoneMap[LocalBone];
							Point.Bones.Add(Bone);
							Point.Weights.Add(Weight);
							bRelevant |= MeshToParticle.Contains(Bone);
						}
						if (bRelevant) {
							Eligible.Add(MoveTemp(Point));
						}
					}
				}
				const int32 Count = FMath::Min(Settings->SkinSampleCount, Eligible.Num());
				for (int32 I = 0; I < Count; ++I) {
					SkinPoints.Add(MoveTemp(Eligible[int64(I) * Eligible.Num() / Count]));
				}
			}
		}
	}
	if (Settings->SkinSampleCount > 0 && SkinPoints.IsEmpty()) {
		OutError = TEXT("未找到受此根骨骼影响的可读取蒙皮顶点。请选择正确的物理根骨骼，或将蒙皮顶点采样数设为 0，仅验证骨骼碰撞代理。");
		return false;
	}
	return true;
}

bool FKawaiiPhysicsCalibrationSession::SampleMotion(int32 Index, FString& OutError) {
	const auto& Motion = Settings->Motions[Index];
	const auto& BodyRef = Settings->BodyMesh->GetRefSkeleton();
	const auto& ClothRef = Settings->ClothingMesh->GetRefSkeleton();
	TArray<FBoneIndexType> Indices;
	for (int32 I = 0; I < BodyRef.GetNum(); ++I) {
		Indices.Add(I);
	}
	FBoneContainer Bones(Indices, UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::None), *Settings->BodyMesh);
	Bones.SetUseRAWData(true);
	const int32 Steps = FMath::CeilToInt(Motion.Animation->GetPlayLength() * Settings->SampleRate / Motion.PlayRate);
	// Never leak validation motion state into a training example (or vice versa).
	const bool bReset = Samples.IsEmpty() || Motion.bResetBefore || (Index > 0 && Motion.bValidationOnly != Settings->Motions[Index - 1].bValidationOnly);
	const TArray<FTransform> Previous = Samples.IsEmpty() ? TArray<FTransform>() : Samples.Last().BodyLocal;
	for (int32 Step = 0; Step <= Steps; ++Step) {
		FMemMark Mark(FMemStack::Get());
		FCompactPose Pose;
		Pose.SetBoneContainer(&Bones);
		Pose.ResetToRefPose();
		FBlendedCurve Curve;
		Curve.InitFrom(Bones);
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData Data(Pose, Curve, Attributes);
		FSample Sample;
		Sample.Time = FMath::Min(float(Step) * Motion.PlayRate / Settings->SampleRate, Motion.Animation->GetPlayLength());
		Sample.Motion = Index;
		Sample.bValidation = Motion.bValidationOnly;
		FAnimExtractContext Context(double(Sample.Time), false);
		Context.bIgnoreRootLock = true;
		Context.bExtractWithRootMotionProvider = false;
		Motion.Animation->GetAnimationPose(Data, Context);
		Sample.BodyLocal = BodyRef.GetRefBonePose();
		for (auto I : Pose.ForEachBoneIndex()) {
			Sample.BodyLocal[Bones.MakeMeshPoseIndex(I).GetInt()] = Pose[I];
		}
		if (!Motion.OffsetBone.IsNone()) {
			auto& T = Sample.BodyLocal[BodyRef.FindBoneIndex(Motion.OffsetBone)];
			T.AddToTranslation(Motion.LocalOffset);
			T.SetRotation((Motion.LocalRotation.Quaternion() * T.GetRotation()).GetNormalized());
		}
		if (!bReset && !Previous.IsEmpty() && Motion.BlendInSeconds > 0) {
			const float Alpha = FMath::Clamp(float(Step) / (Settings->SampleRate * Motion.BlendInSeconds), 0.f, 1.f);
			for (int32 I = 0; I < Sample.BodyLocal.Num(); ++I) {
				FTransform Blended;
				Blended.Blend(Previous[I], Sample.BodyLocal[I], Alpha);
				Sample.BodyLocal[I] = Blended;
			}
		}
		TArray<FTransform> BodyCS, ClothCS;
		ComponentPose(BodyRef, Sample.BodyLocal, BodyCS);
		Sample.ClothingLocal = ClothRef.GetRefBonePose();
		ClothCS.SetNum(ClothRef.GetNum());
		for (int32 I = 0; I < ClothRef.GetNum(); ++I) {
			const int32 Source = BodyRef.FindBoneIndex(ClothRef.GetBoneName(I)), Parent = ClothRef.GetParentIndex(I);
			if (Source >= 0) {
				ClothCS[I] = BodyCS[Source].GetRelativeTransform(Settings->ClothingTransform);
				Sample.ClothingLocal[I] = Parent < 0 ? ClothCS[I] : ClothCS[I].GetRelativeTransform(ClothCS[Parent]);
			} else {
				ClothCS[I] = Parent < 0 ? Sample.ClothingLocal[I] : Sample.ClothingLocal[I] * ClothCS[Parent];
			}
		}
		if (!FinitePose(Sample.BodyLocal) || !FinitePose(Sample.ClothingLocal)) {
			OutError = TEXT("采样产生了包含非有限数值的姿势。");
			return false;
		}
		for (const auto& D : Settings->BodyCapsules) {
			const int32 A = BodyRef.FindBoneIndex(D.StartBone), B = BodyRef.FindBoneIndex(D.EndBone.IsNone() ? D.StartBone : D.EndBone);
			FKawaiiClothingCapsule C;
			if (!KawaiiClothingCollision::BuildCapsuleFromComponentSpace(D, BodyCS[A], BodyCS[B], FTransform::Identity, Settings->ClothingTransform, C)) {
				OutError = TEXT("无法在衣物空间中构建身体碰撞胶囊。");
				return false;
			}
			Sample.Capsules.Add({C.Start, C.End, C.Radius});
		}
		const auto Append = [&](const FSample& Frame) {
			Samples.Add(Frame);
			GPUInput.Frames.Add(FVector4f(Frame.bReset ? 1 : 0, Frame.bScore && !Frame.bValidation ? 1 : 0, 0, 0));
			for (int32 I = 0; I < ParticleMeshBones.Num(); ++I) {
				const int32 Bone = ParticleMeshBones[I];
				GPUInput.Poses.Add(FVector4f(FVector3f(ClothCS[Bone].GetTranslation()), 0));
				FVector Normal = FVector::ZeroVector;
				if (ParticleParents[I] >= 0 && ParticleRanges[I].Y > 0) {
					Normal = ClothCS[ParticleMeshBones[ParticleParents[I]]].GetUnitAxis(static_cast<EAxis::Type>(static_cast<uint8>(ParticleRanges[I].Y)));
				}
				GPUInput.Ranges.Add(FVector4f(FVector3f(Normal), ParticleRanges[I].X));
			}
			for (const auto& C : Frame.Capsules) {
				GPUInput.Capsules.Add(FVector4f(FVector3f(C.A), C.Radius));
				GPUInput.Capsules.Add(FVector4f(FVector3f(C.B), 0));
			}
		};
		if (Step == 0 && bReset) {
			for (int32 Warm = 0; Warm < Settings->WarmUpFrames; ++Warm) {
				FSample WarmSample = Sample;
				WarmSample.bReset = Warm == 0;
				WarmSample.bScore = false;
				Append(WarmSample);
			}
		}
		Sample.bReset = Step == 0 && bReset && Settings->WarmUpFrames == 0;
		Append(Sample);
	}
	return true;
}

bool FKawaiiPhysicsCalibrationSession::BeginCPU(int32 Candidate, FString& OutError) {
	ClearScene();
	Current = FVerified();
	Current.Candidate = Candidate;
	SampleIndex = 0;
	PreviousOffsets.Reset();
	PreviousDisplacements.Reset();
	const auto& SearchCandidate = Candidates[Candidate];
	WorkingProfile = SearchCandidate.bOriginalBaseline ? OriginalProfile.Get() : NewObject<UKawaiiPhysicsCalibrationProfile>();
	if (!SearchCandidate.bOriginalBaseline) {
		const FVector4f C = SearchCandidate.ProfileParameters;
		WorkingProfile->TargetRoot = Template.RootBone.BoneName;
		WorkingProfile->DampingScale = C.X;
		WorkingProfile->StiffnessScale = C.Y;
		WorkingProfile->RadiusScale = C.Z;
		WorkingProfile->ContactIterations = C.W;
		Settings->CopyConstraintsToProfile(*WorkingProfile);
		WorkingProfile->bUseSceneContacts = false;
		WorkingProfile->bUseAttachedParent = !Settings->bSearchNodeSettings || Settings->bUseAttachedParentForContacts;
		WorkingProfile->SurfaceMesh = Settings->ClothingMesh;
		for (int32 Index : SurfacePointIndices) {
			const auto& Skin = SkinPoints[Index];
			FKawaiiCalibrationSurfacePoint Point;
			for (int32 Influence = 0; Influence < Skin.Bones.Num(); ++Influence) {
				const int32 Bone = Skin.Bones[Influence];
				Point.Bones.Add(Settings->ClothingMesh->GetRefSkeleton().GetBoneName(Bone));
				Point.LocalPositions.Add(InverseRefPose[Bone].TransformPosition(Skin.Position));
			}
			Point.Weights = Skin.Weights;
			WorkingProfile->SurfacePoints.Add(MoveTemp(Point));
		}
		if (Settings->bSearchNodeSettings) {
			WorkingProfile->bHasOptimizedNodeSettings = true;
			WorkingProfile->OptimizedNodeSettings = SearchCandidate.NodeSettings;
		}
	}
	Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetEditor(true).SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(false)
	);
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	Actor = Scene->GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!Actor) {
		OutError = TEXT("无法创建 CPU 验证预览场景。");
		return false;
	}
	const auto Create = [&](const TCHAR* Name, USkeletalMesh* Mesh) {
		auto* Component = NewObject<USkeletalMeshComponent>(Actor, Name, RF_Transient);
		Actor->AddInstanceComponent(Component);
		Component->SetSkeletalMesh(Mesh);
		Component->SetComponentTickEnabled(false);
		Component->SetDisablePostProcessBlueprint(true);
		Component->bEnableUpdateRateOptimizations = false;
		Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Component->SetForcedLOD(1);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return Component;
	};
	Body = Create(TEXT("CalibrationBody"), Settings->BodyMesh);
	Actor->SetRootComponent(Body);
	Clothing = Create(TEXT("CalibrationClothing"), Settings->ClothingMesh);
	Clothing->SetupAttachment(Body);
	Clothing->SetRelativeTransform(Settings->ClothingTransform);
	Clothing->AddTickPrerequisiteComponent(Body);
	Scene->AddComponent(Body, FTransform::Identity);
	Scene->AddComponent(Clothing, Settings->ClothingTransform);
	Body->SetAnimInstanceClass(UKawaiiCalibrationAnimInstance::StaticClass());
	Clothing->SetAnimInstanceClass(UKawaiiCalibrationAnimInstance::StaticClass());
	Body->SetComponentTickEnabled(false);
	Clothing->SetComponentTickEnabled(false);
	BodyInstance = Cast<UKawaiiCalibrationAnimInstance>(Body->GetAnimInstance());
	ClothingInstance = Cast<UKawaiiCalibrationAnimInstance>(Clothing->GetAnimInstance());
	if (!BodyInstance || !ClothingInstance) {
		OutError = TEXT("无法初始化原生 CPU 校准动画图。");
		return false;
	}
	ClothingInstance->PhysicsTemplate = Settings->bSearchNodeSettings ? OriginalTemplate : Template;
	if (Settings->bSearchNodeSettings && !SearchCandidate.bOriginalBaseline) {
		KawaiiCalibrationSearch::Apply(SearchCandidate.NodeSettings, ClothingInstance->PhysicsTemplate);
	}
	ClothingInstance->PhysicsTemplate.CalibrationProfile = WorkingProfile;
	ClothingInstance->bEnablePhysics = true;
	ClothingInstance->InitializeAnimation();
	return true;
}

bool FKawaiiPhysicsCalibrationSession::StepCPU(FString& OutError) {
	const FSample& Sample = Samples[SampleIndex];
	if (Sample.bReset) {
		ClothingInstance->InitializeAnimation();
		PreviousOffsets.Reset();
		PreviousDisplacements.Reset();
	}
	BodyInstance->InputPose = Sample.BodyLocal;
	ClothingInstance->InputPose = Sample.ClothingLocal;
	const uint64 BodySerial = BodyInstance->GetSourceEvaluationSerial();
	const uint64 ClothingSerial = ClothingInstance->GetSourceEvaluationSerial();
	const double Start = FPlatformTime::Seconds();
	Body->TickAnimation(GPUInput.DeltaTime, false);
	Body->RefreshBoneTransforms(nullptr);
	Clothing->TickAnimation(GPUInput.DeltaTime, false);
	Clothing->RefreshBoneTransforms(nullptr);
	if (BodyInstance->GetSourceEvaluationSerial() <= BodySerial || ClothingInstance->GetSourceEvaluationSerial() <= ClothingSerial) {
		OutError = TEXT("CPU 采样未实际求值输入姿态，请等待网格体编译完成后重试。");
		return false;
	}
	const double Elapsed = FPlatformTime::Seconds() - Start;
	if (ClothingInstance->GetSimulatedBoneCount() == 0) {
		OutError = TEXT("CPU 验证动画图未执行 KawaiiPhysics 节点。");
		return false;
	}
	const auto& Actual = Clothing->GetComponentSpaceTransforms();
	if (Actual.Num() != Settings->ClothingMesh->GetRefSkeleton().GetNum() || !FinitePose(Actual)) {
		OutError = TEXT("CPU KawaiiPhysics 模拟产生了不完整或包含非有限数值的姿势。");
		return false;
	}
	FMetrics& M = Sample.bValidation ? Current.Validation : Current.Training;
	if (Sample.bScore) {
		M.CPUSeconds += Elapsed;
	}
	TArray<FTransform> TargetCS;
	ComponentPose(Settings->ClothingMesh->GetRefSkeleton(), Sample.ClothingLocal, TargetCS);
	TArray<FVector> Positions, Offsets, Displacements;
	Positions.SetNum(ParticleMeshBones.Num());
	Offsets.SetNum(ParticleMeshBones.Num());
	Displacements.SetNumZeroed(Positions.Num());
	for (int32 I = 0; I < Positions.Num(); ++I) {
		Positions[I] = Actual[ParticleMeshBones[I]].GetTranslation();
		Offsets[I] = Positions[I] - TargetCS[ParticleMeshBones[I]].GetTranslation();
		if (PreviousOffsets.IsValidIndex(I)) {
			Displacements[I] = Offsets[I] - PreviousOffsets[I];
		}
	}
	if (Sample.bScore) {
		for (int32 I = 0; I < Positions.Num(); ++I) {
			const int32 Parent = ParticleParents[I];
			if (Parent < 0) {
				continue;
			}
			double Depth = 0;
			int32 DeepestCapsule = INDEX_NONE;
			for (int32 CapsuleIndex = 0; CapsuleIndex < Sample.Capsules.Num(); ++CapsuleIndex) {
				const auto& C = Sample.Capsules[CapsuleIndex];
				FVector N;
				double Along;
				// 固定验收几何，避免缩小候选碰撞半径就能降低穿透评分。
				const double ContactDepth =
					KawaiiContact::SegmentContact(Positions[Parent], Positions[I], C, GPUInput.Bones[I].W, FVector::ForwardVector, N, Along);
				if (ContactDepth > Depth) {
					Depth = ContactDepth;
					DeepestCapsule = CapsuleIndex;
				}
			}
			M.AddDepth(Depth, Sample.Motion, Sample.Time, ParticleMeshBones[I], DeepestCapsule);
			++M.BoneCount;
			M.Deviation2 += FVector::DistSquared(Positions[I], TargetCS[ParticleMeshBones[I]].GetTranslation());
			if (PreviousDisplacements.IsValidIndex(I)) {
				M.Jitter2 += FVector::DistSquared(Displacements[I], PreviousDisplacements[I]);
			}
			const double Length = FVector::Distance(TargetCS[ParticleMeshBones[I]].GetTranslation(), TargetCS[ParticleMeshBones[Parent]].GetTranslation());
			M.MaxLengthError = FMath::Max(M.MaxLengthError, FMath::Abs(FVector::Distance(Positions[I], Positions[Parent]) - Length));
		}
		for (int32 SkinIndex = 0; SkinIndex < SkinPoints.Num(); ++SkinIndex) {
			const auto& Skin = SkinPoints[SkinIndex];
			FVector P = FVector::ZeroVector;
			for (int32 I = 0; I < Skin.Bones.Num(); ++I) {
				const int32 B = Skin.Bones[I];
				P += Actual[B].TransformPosition(InverseRefPose[B].TransformPosition(Skin.Position)) * Skin.Weights[I];
			}
			double Depth = 0;
			int32 DeepestCapsule = INDEX_NONE;
			for (int32 CapsuleIndex = 0; CapsuleIndex < Sample.Capsules.Num(); ++CapsuleIndex) {
				const auto& C = Sample.Capsules[CapsuleIndex];
				const FVector Closest = FMath::ClosestPointOnSegment(P, C.A, C.B);
				const double ContactDepth = C.Radius - FVector::Distance(P, Closest);
				if (ContactDepth > Depth) {
					Depth = ContactDepth;
					DeepestCapsule = CapsuleIndex;
				}
			}
			if (Depth > M.MaxSkinDepth) {
				M.WorstSkinSample = SkinIndex;
				M.WorstSkinMotion = Sample.Motion;
				M.WorstSkinTime = Sample.Time;
				M.WorstSkinCapsule = DeepestCapsule;
			}
			if (!bSurfacePointsSelected && !Sample.bValidation) {
				SurfacePointDepths[SkinIndex] = FMath::Max(SurfacePointDepths[SkinIndex], Depth);
			}
			M.AddDepth(Depth, Sample.Motion, Sample.Time, INDEX_NONE, DeepestCapsule);
		}
	}
	PreviousOffsets = MoveTemp(Offsets);
	PreviousDisplacements = MoveTemp(Displacements);
	++SampleIndex;
	return true;
}

void FKawaiiPhysicsCalibrationSession::SelectSurfacePoints() {
	TArray<double> Distances;
	Distances.Init(1., SkinPoints.Num());
	TArray<bool> Selected;
	Selected.Init(false, SkinPoints.Num());
	int32 EligibleCount = 0;
	for (int32 I = 0; I < SkinPoints.Num(); ++I) {
		bool bMovable = false;
		for (int32 MeshBone : SkinPoints[I].Bones) {
			const int32 Particle = ParticleMeshBones.IndexOfByKey(MeshBone);
			bMovable |= ParticleParents.IsValidIndex(Particle) && ParticleParents[Particle] != INDEX_NONE;
		}
		// 固定区域仍参与验收，但不占用运行时接触预算。
		Selected[I] = !bMovable;
		EligibleCount += bMovable ? 1 : 0;
	}
	const int32 Count = FMath::Min(128, EligibleCount);
	for (int32 Point = 0; Point < Count; ++Point) {
		int32 Best = INDEX_NONE;
		double BestScore = -1.;
		for (int32 I = 0; I < SkinPoints.Num(); ++I) {
			const double Score = (1. + 8. * SurfacePointDepths[I]) * FMath::Sqrt(Distances[I]);
			if (!Selected[I] && Score > BestScore) {
				Best = I;
				BestScore = Score;
			}
		}
		Selected[Best] = true;
		SurfacePointIndices.Add(Best);
		for (int32 I = 0; I < SkinPoints.Num(); ++I) {
			const double Distance = FVector::DistSquared(SkinPoints[I].Position, SkinPoints[Best].Position);
			Distances[I] = Point == 0 ? Distance : FMath::Min(Distances[I], Distance);
		}
	}
	bSurfacePointsSelected = true;
}

void FKawaiiPhysicsCalibrationSession::Finish() {
	const FVerified& Baseline = Verified[0];
	const auto MeetsLimits = [&](const FVerified& V) {
		return AcceptanceDepth(V.Training) <= Settings->MaxAcceptedPenetration && AcceptanceDepth(V.Validation) <= Settings->MaxAcceptedPenetration
			&& V.Training.MaxLengthError <= Settings->MaxAcceptedLengthError && V.Validation.MaxLengthError <= Settings->MaxAcceptedLengthError;
	};
	int32 Best = 0;
	for (int32 I = 1; I < Verified.Num(); ++I) {
		const auto& V = Verified[I];
		const bool bValidationOK = !V.Validation.Count
			|| (AcceptanceDepth(V.Validation) <= AcceptanceDepth(Baseline.Validation) + .01
				&& Rank(V.Validation, V.Candidate) <= Rank(Baseline.Validation, 0) + .001
				&& V.Validation.MaxLengthError <= FMath::Max(.5, Baseline.Validation.MaxLengthError + .01));
		if (bValidationOK && AcceptanceDepth(V.Training) <= AcceptanceDepth(Baseline.Training) + .01
			&& ((MeetsLimits(V) && !MeetsLimits(Verified[Best])) || (MeetsLimits(V) == MeetsLimits(Verified[Best]) && V.Rank < Verified[Best].Rank))
			&& V.Training.MaxLengthError <= FMath::Max(.5, Baseline.Training.MaxLengthError + .01))
		{
			Best = I;
		}
	}
	const auto& Winner = Verified[Best];
	const bool bAccepted = MeetsLimits(Winner);
	const auto& Selected = Candidates[Winner.Candidate];
	Result = Selected.bOriginalBaseline ? nullptr : DuplicateObject<UKawaiiPhysicsCalibrationProfile>(VerifiedProfiles[Best].Get(), GetTransientPackage());
	bResultAccepted = bAccepted && Result != nullptr;
	FString Summary = FString::Printf(
		TEXT(
			"%s：%d 组候选；原始 CPU 验证：%d 组；选中候选：%d（%s）。训练片段验收穿透：%.4f -> %.4f 厘米；独立验证片段验收穿透：%.4f -> %.4f 厘米；蒙皮采样点：%d。存在蒙皮采样时按布面验收，否则按固定骨骼代理验收；不保证三角网格完全无穿模。"
		),
		Settings->bSearchNodeSettings ? TEXT("节点参数搜索") : TEXT("GPU 筛选"),
		Candidates.Num(),
		Verified.Num(),
		Winner.Candidate,
		*Selected.Label,
		AcceptanceDepth(Baseline.Training),
		AcceptanceDepth(Winner.Training),
		AcceptanceDepth(Baseline.Validation),
		AcceptanceDepth(Winner.Validation),
		SkinPoints.Num()
	);
	if (!Winner.Validation.Count) {
		Summary += TEXT(" 未提供独立验证片段，尚未检验对其他动作的适用性。");
	}
	if (!bAccepted) {
		Summary = (Result ? FString(TEXT("未达到验收阈值；这是有限候选中的最佳结果，可另行保存供试用。")) : FString(TEXT("原配置仍未达到验收阈值。"))) + Summary;
	}
	if (Selected.bOriginalBaseline) {
		Summary = TEXT("原配置表现最好，本次没有找到优于原配置的新参数。") + Summary;
	}
	if (Result) {
		Result->ValidationSummary = Summary;
	}
	const auto Metrics = [&](const FMetrics& M) {
		auto O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("samples"), M.Count);
		O->SetNumberField(TEXT("max_penetration_cm"), M.MaxDepth);
		O->SetNumberField(TEXT("acceptance_penetration_cm"), AcceptanceDepth(M));
		O->SetNumberField(TEXT("max_bone_penetration_cm"), M.MaxBoneDepth);
		O->SetNumberField(TEXT("max_skin_penetration_cm"), M.MaxSkinDepth);
		const bool bSkin = !SkinPoints.IsEmpty();
		const int32 WorstMotion = bSkin ? M.WorstSkinMotion : M.WorstMotion;
		const int32 WorstCapsule = bSkin ? M.WorstSkinCapsule : M.WorstCapsule;
		if (Settings->Motions.IsValidIndex(WorstMotion)) {
			O->SetStringField(TEXT("worst_animation"), Settings->Motions[WorstMotion].Animation->GetPathName());
			O->SetNumberField(TEXT("worst_time_seconds"), bSkin ? M.WorstSkinTime : M.WorstTime);
			O->SetStringField(TEXT("worst_kind"), bSkin ? TEXT("skin") : TEXT("bone"));
			if (!bSkin && M.WorstMeshBone != INDEX_NONE) {
				O->SetStringField(TEXT("worst_bone"), Settings->ClothingMesh->GetRefSkeleton().GetBoneName(M.WorstMeshBone).ToString());
			} else if (SkinPoints.IsValidIndex(M.WorstSkinSample)) {
				const auto& Point = SkinPoints[M.WorstSkinSample];
				O->SetStringField(TEXT("worst_skin_reference_position"), Point.Position.ToString());
				O->SetBoolField(TEXT("worst_skin_is_runtime_sample"), SurfacePointIndices.Contains(M.WorstSkinSample));
				TArray<TSharedPtr<FJsonValue>> Influences;
				for (int32 I = 0; I < Point.Bones.Num(); ++I) {
					auto Influence = MakeShared<FJsonObject>();
					Influence->SetStringField(TEXT("bone"), Settings->ClothingMesh->GetRefSkeleton().GetBoneName(Point.Bones[I]).ToString());
					Influence->SetNumberField(TEXT("weight"), Point.Weights[I]);
					const int32 Particle = ParticleMeshBones.IndexOfByKey(Point.Bones[I]);
					Influence->SetBoolField(TEXT("movable"), ParticleParents.IsValidIndex(Particle) && ParticleParents[Particle] != INDEX_NONE);
					Influences.Add(MakeShared<FJsonValueObject>(Influence));
				}
				O->SetArrayField(TEXT("worst_skin_influences"), Influences);
			}
			if (Settings->BodyCapsules.IsValidIndex(WorstCapsule)) {
				O->SetStringField(TEXT("worst_capsule_bone"), Settings->BodyCapsules[WorstCapsule].StartBone.ToString());
			}
		}
		O->SetNumberField(TEXT("mean_squared_penetration"), MeanSquaredPenetration(M));
		O->SetNumberField(TEXT("mean_squared_skin_penetration"), M.SkinDepth2 / FMath::Max<int64>(1, M.SkinCount));
		O->SetNumberField(TEXT("mean_squared_bone_penetration"), M.BoneDepth2 / FMath::Max<int64>(1, M.BoneCount));
		O->SetNumberField(TEXT("skin_samples"), M.SkinCount);
		O->SetNumberField(TEXT("bone_samples"), M.BoneCount);
		O->SetNumberField(TEXT("mean_squared_pose_deviation"), M.Deviation2 / FMath::Max<int64>(1, M.BoneCount));
		O->SetNumberField(TEXT("mean_squared_displacement_change"), M.Jitter2 / FMath::Max<int64>(1, M.BoneCount));
		O->SetNumberField(TEXT("max_length_error_cm"), M.MaxLengthError);
		O->SetNumberField(TEXT("cpu_replay_seconds"), M.CPUSeconds);
		return O;
	};
	auto Report = MakeShared<FJsonObject>();
	Report->SetBoolField(TEXT("accepted"), bAccepted);
	Report->SetBoolField(TEXT("has_new_candidate"), Result != nullptr);
	Report->SetStringField(TEXT("search_mode"), Settings->bSearchNodeSettings ? TEXT("native_node_settings") : TEXT("gpu_profile_multipliers"));
	Report->SetStringField(
		TEXT("jitter_metric"),
		TEXT("Squared second difference of simulated minus input component-space position, per fixed sample; animation acceleration itself is excluded.")
	);
	if (Settings->bSearchNodeSettings) {
		Report->SetStringField(
			TEXT("body_contact_source"),
			TEXT("New candidates read same-name body bones from the clothing input pose; component attachment is not required.")
		);
		Report->SetStringField(
			TEXT("world_damping_search"),
			TEXT("Preserved: test components have no world trajectory, so world translation/rotation damping cannot be identified.")
		);
	}
	Report->SetNumberField(TEXT("max_accepted_penetration_cm"), Settings->MaxAcceptedPenetration);
	Report->SetNumberField(TEXT("max_accepted_length_error_cm"), Settings->MaxAcceptedLengthError);
	Report->SetStringField(TEXT("collision_source"), CollisionSource);
	Report->SetNumberField(TEXT("body_capsule_count"), Settings->BodyCapsules.Num());
	TArray<TSharedPtr<FJsonValue>> ShapeJSON, RangeJSON, ReplacedJSON;
	for (const auto& Capsule : Settings->BodyCapsules) {
		ShapeJSON.Add(MakeShared<FJsonValueObject>(FJsonObjectConverter::UStructToJsonObject(Capsule).ToSharedRef()));
	}
	for (const auto& Limit : Settings->ChainLimits) {
		RangeJSON.Add(MakeShared<FJsonValueObject>(FJsonObjectConverter::UStructToJsonObject(Limit).ToSharedRef()));
	}
	for (FName Bone : Settings->ReplacedCollisionBones) {
		ReplacedJSON.Add(MakeShared<FJsonValueString>(Bone.ToString()));
	}
	Report->SetArrayField(TEXT("body_capsules"), ShapeJSON);
	Report->SetArrayField(TEXT("chain_limits"), RangeJSON);
	Report->SetArrayField(TEXT("replaced_collision_bones"), ReplacedJSON);
	Report->SetNumberField(TEXT("runtime_surface_sample_count"), SurfacePointIndices.Num());
	Report->SetNumberField(TEXT("validation_skin_sample_count"), SkinPoints.Num());
	Report->SetStringField(TEXT("clothing_transform"), Settings->ClothingTransform.ToString());
	Report->SetStringField(TEXT("summary"), Summary);
	Report->SetStringField(TEXT("body_mesh"), Settings->BodyMesh->GetPathName());
	Report->SetStringField(TEXT("clothing_mesh"), Settings->ClothingMesh->GetPathName());
	Report->SetStringField(TEXT("blueprint"), Settings->ClothingBlueprint->GetPathName());
	Report->SetStringField(TEXT("root"), Settings->PhysicsRoot.ToString());
	Report->SetNumberField(TEXT("seed"), Settings->Seed);
	Report->SetNumberField(TEXT("sample_rate"), Settings->SampleRate);
	Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - StartTime);
	Report->SetNumberField(TEXT("selected_candidate"), Winner.Candidate);
	Report->SetBoolField(TEXT("has_held_out_validation"), Winner.Validation.Count > 0);
	Report->SetStringField( TEXT("baseline"), Settings->bSearchNodeSettings ? TEXT("候选 0：原始节点参数和已绑定的原校准配置；不添加新接触点。") : TEXT("候选 0：原始参数倍率（1,1,1），启用新增 CPU 碰撞修正并迭代 2 次；与未启用该修正的原始节点不同。") );
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const auto& V : Verified) {
		auto Row = MakeShared<FJsonObject>();
		const auto& Candidate = Candidates[V.Candidate];
		const auto& P = Candidate.ProfileParameters;
		Row->SetNumberField(TEXT("candidate"), V.Candidate);
		Row->SetStringField(TEXT("label"), Candidate.Label);
		Row->SetBoolField(TEXT("original_baseline"), Candidate.bOriginalBaseline);
		Row->SetObjectField(TEXT("node_settings"), FJsonObjectConverter::UStructToJsonObject(Candidate.NodeSettings).ToSharedRef());
		Row->SetNumberField(TEXT("damping_scale"), P.X);
		Row->SetNumberField(TEXT("stiffness_scale"), P.Y);
		Row->SetNumberField(TEXT("radius_scale"), P.Z);
		Row->SetNumberField(TEXT("contact_iterations"), P.W);
		Row->SetBoolField(TEXT("has_contact_profile"), VerifiedProfiles[&V - Verified.GetData()] != nullptr);
		if (const auto* Profile = VerifiedProfiles[&V - Verified.GetData()].Get()) {
			Row->SetBoolField(TEXT("contacts_use_attached_parent"), Profile->bUseAttachedParent);
		}
		Row->SetObjectField(TEXT("training"), Metrics(V.Training));
		Row->SetObjectField(TEXT("validation"), Metrics(V.Validation));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("cpu_finalists"), Rows);
	Rows.Reset();
	for (const auto& M : Settings->Motions) {
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("animation"), M.Animation->GetPathName());
		Row->SetNumberField(TEXT("play_rate"), M.PlayRate);
		Row->SetNumberField(TEXT("blend_seconds"), M.BlendInSeconds);
		Row->SetBoolField(TEXT("reset"), M.bResetBefore);
		Row->SetBoolField(TEXT("held_out"), M.bValidationOnly);
		Row->SetStringField(TEXT("ik_bone"), M.OffsetBone.ToString());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("motions"), Rows);
	if (!bAccepted) {
		FString Failure = FString::Printf(
			TEXT(
				"最佳结果未达标：仍有 %.3f 厘米穿透（允许 %.3f），骨长误差 %.3f 厘米（允许 %.3f）。碰撞来源：%s。请根据报告检查最严重穿透的动作、时刻和碰撞骨骼。"
			),
			FMath::Max(AcceptanceDepth(Winner.Training), AcceptanceDepth(Winner.Validation)),
			Settings->MaxAcceptedPenetration,
			FMath::Max(Winner.Training.MaxLengthError, Winner.Validation.MaxLengthError),
			Settings->MaxAcceptedLengthError,
			*CollisionSource
		);
		const FMetrics& Worst = AcceptanceDepth(Winner.Training) >= AcceptanceDepth(Winner.Validation) ? Winner.Training : Winner.Validation;
		if (Settings->Motions.IsValidIndex(Worst.WorstSkinMotion) && Settings->BodyCapsules.IsValidIndex(Worst.WorstSkinCapsule)) {
			Failure += FString::Printf(
				TEXT(" 最严重布面接触：%s 的 %.3f 秒处，与 %s 相交。"),
				*Settings->Motions[Worst.WorstSkinMotion].Animation->GetName(),
				Worst.WorstSkinTime,
				*Settings->BodyCapsules[Worst.WorstSkinCapsule].StartBone.ToString()
			);
			if (SkinPoints.IsValidIndex(Worst.WorstSkinSample)) {
				const auto& Point = SkinPoints[Worst.WorstSkinSample];
				double MovableWeight = 0.;
				for (int32 I = 0; I < Point.Bones.Num(); ++I) {
					const int32 Particle = ParticleMeshBones.IndexOfByKey(Point.Bones[I]);
					if (ParticleParents.IsValidIndex(Particle) && ParticleParents[Particle] != INDEX_NONE) {
						MovableWeight += Point.Weights[I];
					}
				}
				Failure += FString::Printf(TEXT(" 此点可平移修正的骨骼影响权重为 %.1f%%；请检查模拟根骨骼、蒙皮权重与碰撞代理。"), MovableWeight * 100.);
				if (MovableWeight < .1) {
					Failure +=
						TEXT(" 该点大部分权重属于固定骨骼；调整 Physics Settings 或 Bone Constraints 无法移动固定骨骼，增加候选数量也不能解决这一限制。");
				}
			}
		}
		Summary += TEXT(" ") + Failure;
		if (Result) {
			Result->ValidationSummary = Summary;
		}
	}
	Report->SetStringField(TEXT("summary"), Summary);
	ReportPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("KawaiiCalibration") / FGuid::NewGuid().ToString(EGuidFormats::Digits) / TEXT("report.json")
	);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
	FString Json;
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
	if (!FFileHelper::SaveStringToFile(Json, *ReportPath)) {
		Result = nullptr;
		Fail(TEXT("无法保存校准报告。"));
		return;
	}
	Stage = EStage::Complete;
	Status = Summary + (Result ? TEXT(" 可保存当前最佳参数供查看和试用；未达标状态会保留在配置中。") : TEXT(" 保留原节点，无需生成替代资产。"));
	ClearScene();
}

void FKawaiiPhysicsCalibrationSession::ClearScene() {
	BodyInstance = nullptr;
	ClothingInstance = nullptr;
	if (Scene) {
		if (Clothing) {
			Scene->RemoveComponent(Clothing);
			Clothing->DestroyComponent();
		}
		if (Body) {
			Scene->RemoveComponent(Body);
			Body->DestroyComponent();
		}
		if (Actor) {
			Actor->Destroy();
		}
	}
	Clothing = nullptr;
	Body = nullptr;
	Actor = nullptr;
	Scene.Reset();
}

double FKawaiiPhysicsCalibrationSession::Rank(const FMetrics& M, int32 Candidate) const {
	const double BoneCount = FMath::Max<int64>(1, M.BoneCount);
	return Settings->PenetrationWeight * (MeanSquaredPenetration(M) + FMath::Square(AcceptanceDepth(M))) + Settings->DeviationWeight * M.Deviation2 / BoneCount
		+ Settings->JitterWeight * M.Jitter2 / BoneCount
		+ Settings->IterationCostWeight
		* (Settings->bSearchNodeSettings ? KawaiiCalibrationSearch::Cost(Candidates[Candidate]) : Candidates[Candidate].ProfileParameters.W);
}

double FKawaiiPhysicsCalibrationSession::MeanSquaredPenetration(const FMetrics& M) const {
	return SkinPoints.IsEmpty() ? M.BoneDepth2 / FMath::Max<int64>(1, M.BoneCount) : M.SkinDepth2 / FMath::Max<int64>(1, M.SkinCount);
}

double FKawaiiPhysicsCalibrationSession::AcceptanceDepth(const FMetrics& M) const {
	return SkinPoints.IsEmpty() ? M.MaxBoneDepth : M.MaxSkinDepth;
}

void FKawaiiPhysicsCalibrationSession::Fail(const FString& Error) {
	Status = Error;
	Stage = EStage::Failed;
	GPU.Reset();
	ClearScene();
}

void FKawaiiPhysicsCalibrationSession::FMetrics::AddDepth(double Depth, int32 Motion, float Time, int32 MeshBone, int32 Capsule) {
	Depth2 += Depth * Depth;
	if (MeshBone == INDEX_NONE) {
		SkinDepth2 += Depth * Depth;
		++SkinCount;
	} else {
		BoneDepth2 += Depth * Depth;
	}
	double& KindDepth = MeshBone == INDEX_NONE ? MaxSkinDepth : MaxBoneDepth;
	KindDepth = FMath::Max(KindDepth, Depth);
	if (Depth > MaxDepth) {
		MaxDepth = Depth;
		WorstMotion = Motion;
		WorstTime = Time;
		WorstMeshBone = MeshBone;
		WorstCapsule = Capsule;
	}
	++Count;
}
