#if WITH_DEV_AUTOMATION_TESTS

#include "KawaiiClothingMotionReadback.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "KawaiiClothingProxyPlayback.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"

namespace {
	void PopulateReadbackClip(UAnimSequence& Animation, USkeleton& Skeleton, bool bEdited, int32 Frames = 2) {
		Animation.SetSkeleton(&Skeleton);
		auto& Controller = Animation.GetController();
		Controller.OpenBracket(FText::FromString(TEXT("Readback fixture")), false);
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(2, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
		for (const FName Bone : {FName(TEXT("root")), FName(TEXT("cloth")), FName(TEXT("tip"))}) {
			TArray<FVector3f> Positions, Scales;
			TArray<FQuat4f> Rotations;
			for (int32 Frame = 0; Frame <= Frames; ++Frame) {
				Positions.Add(Bone == TEXT("root") ? FVector3f(Frame * 5, 0, 0) : FVector3f(0, 0, Bone == TEXT("cloth") ? 10 : -10));
				Scales.Add(FVector3f::OneVector);
				Rotations.Add(bEdited && Frame == 1 && Bone == TEXT("cloth") ? FQuat4f(FVector3f::YAxisVector, PI / 2) : FQuat4f::Identity);
			}
			Controller.AddBoneCurve(Bone, false);
			Controller.SetBoneTrackKeys(Bone, Positions, Rotations, Scales, false);
		}
		Controller.NotifyPopulated(); Controller.CloseBracket(false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiClothingReadbackKeysTest, "KawaiiPhysicsBaker.ClothingReadback.ManualKeysAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiClothingReadbackKeysTest::RunTest(const FString&) {
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>());
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("cloth"), TEXT("cloth"), 0), FTransform(FVector(0, 0, 10)));
		Modifier.Add(FMeshBoneInfo(TEXT("tip"), TEXT("tip"), 1), FTransform(FVector(0, 0, -10)));
	}
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
	Mesh->SetSkeleton(Skeleton.Get()); Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	TStrongObjectPtr<UAnimSequence> Body(NewObject<UAnimSequence>()), OldClothing(NewObject<UAnimSequence>()), Edited(NewObject<UAnimSequence>());
	PopulateReadbackClip(*Body.Get(), *Skeleton.Get(), false);
	PopulateReadbackClip(*OldClothing.Get(), *Skeleton.Get(), false);
	PopulateReadbackClip(*Edited.Get(), *Skeleton.Get(), true);
	Edited->bEnableRootMotion = Edited->bForceRootLock = true;
	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>());
	Definition->PreviewMesh = Mesh.Get();
	FKawaiiClothingProxyShape Shape;
	Shape.Id = TEXT("Skirt"); Shape.StartBone = TEXT("cloth"); Shape.EndBone = TEXT("tip"); Shape.StartOffset = FVector(2, 0, 0); Shape.Radius = 1;
	Definition->Shapes.Add(Shape);
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Original(NewObject<UKawaiiClothingProxyMotion>()), Result(NewObject<UKawaiiClothingProxyMotion>());
	Original->Definition = Definition.Get(); Original->SourceMesh = Mesh.Get();
	Original->SourceAnimation = Body.Get(); Original->BodyBakedAnimation = Body.Get(); Original->BakedAnimation = OldClothing.Get();
	Original->SourceDataGuid = KawaiiClothingReadback::AnimationRevision(Body.Get());
	Original->PartName = TEXT("Clothing"); Original->SampleRate = 2; Original->SourceDuration = .9f; Original->BakeGroup = FGuid::NewGuid();
	Original->RelativeTransform = FTransform(FRotator(0, 90, 0), FVector(20, 30, 40), FVector(1.5));
	TArray<KawaiiClothingReadback::FFrame> Frames;
	FString Error;
	if (!TestTrue(TEXT("Edited animation can be read without a physics graph"), KawaiiClothingReadback::Build(*Original.Get(), *Edited.Get(), *Result.Get(), Frames, Error))) { AddError(Error); return false; }
	TestEqual(TEXT("Padded endpoint is retained"), Result->NumFrames, 3);
	TestTrue(TEXT("Body action identity and take pairing are retained"), Result->SourceAnimation == Original->SourceAnimation && Result->BodyBakedAnimation == Original->BodyBakedAnimation && Result->BakeGroup == Original->BakeGroup);
	TestTrue(TEXT("Playback now points at the edited clothing animation"), Result->BakedAnimation.Get() == Edited.Get());
	TestTrue(TEXT("Readback does not update the original before review"), Original->LocalKeys.IsEmpty() && Original->BakedAnimation.Get() == OldClothing.Get());
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	TestTrue(TEXT("Readback is consumable by the runtime sampler"), KawaiiClothingProxy::SampleBlended({{Result.Get(), .5f, 1.f}}, Result->RelativeTransform, Capsules, Error));
	if (TestEqual(TEXT("One edited capsule"), Capsules.Num(), 1)) {
		TestTrue(TEXT("Manual rotation, source root motion and relative placement are applied exactly once"), Capsules[0].Start.Equals(Result->RelativeTransform.TransformPosition(FVector(5, 0, 8)), .001));
		TestTrue(TEXT("Edited parent rotation carries its child endpoint"), Capsules[0].End.Equals(Result->RelativeTransform.TransformPosition(FVector(-5, 0, 10)), .001));
	}
	TestTrue(TEXT("Preview poses preserve the authored root even with root lock enabled"), FVector(Frames.Last().Pose[0].GetTranslation()).Equals(FVector(10, 0, 0), .001));
	const auto Revision = Result->MotionRevision;
	PopulateReadbackClip(*Edited.Get(), *Skeleton.Get(), false);
	TestFalse(TEXT("Editing again invalidates an older readback"), Result->Validate(Error));
	TestTrue(TEXT("Repeated readback succeeds"), KawaiiClothingReadback::Build(*Original.Get(), *Edited.Get(), *Result.Get(), Frames, Error));
	TestTrue(TEXT("Same-path updates carry a new runtime cache revision"), Result->MotionRevision != Revision);
	PopulateReadbackClip(*Edited.Get(), *Skeleton.Get(), true, 3);
	TestFalse(TEXT("Timeline changes cannot silently drift from the body"), KawaiiClothingReadback::Build(*Original.Get(), *Edited.Get(), *Result.Get(), Frames, Error));
	Original->SampleRate = 0;
	TestFalse(TEXT("Invalid sampling fails before division"), KawaiiClothingReadback::Build(*Original.Get(), *OldClothing.Get(), *Result.Get(), Frames, Error));
	return true;
}

namespace KawaiiClothingReadbackTests {
	void VerifyExport(FAutomationTestBase& Test, const UKawaiiPhysicsBakeSettings& Settings, const TArray<TWeakObjectPtr<UObject>>& Assets, const TArray<TArray<FTransform>>& Preview) {
		UKawaiiClothingProxyMotion* Motion = nullptr;
		for (const auto& Asset : Assets) { if (auto* Found = Cast<UKawaiiClothingProxyMotion>(Asset.Get()); Found && Found->PartName == Settings.Parts[0].Name) { Motion = Found; break; } }
		if (!Test.TestNotNull(TEXT("Initial bake exports clothing proxy"), Motion)) { return; }
		UAnimSequence* Baked = Motion->BakedAnimation.LoadSynchronous();
		const auto BodyGuid = KawaiiClothingReadback::AnimationRevision(Motion->BodyBakedAnimation.LoadSynchronous());
		const auto BakedGuid = KawaiiClothingReadback::AnimationRevision(Baked);
		FString Error;
		double MaxError = 0.;
		for (int32 Frame = 0; Frame < Preview.Num(); ++Frame) {
			KawaiiClothingReadback::FFrame Read;
			if (!Test.TestTrue(TEXT("Native baked garment decodes"), KawaiiClothingReadback::Decode(*Baked, *Settings.Parts[0].Mesh, float(Frame) / Settings.SampleRate, Read, Error))) { return; }
			for (int32 Bone = 0; Bone < Settings.Parts[0].Mesh->GetRefSkeleton().GetRawBoneNum(); ++Bone) {
				MaxError = FMath::Max(MaxError, FVector::Distance(FVector(Read.Pose[Bone].GetTranslation()), Preview[Frame][Bone].GetLocation()));
			}
		}
		Test.TestTrue(TEXT("Body-contact preview agrees with exported clothing"), MaxError < .01);
		const FString PackageName = FPackageName::GetLongPackagePath(Settings.OutputDirectory.Path) + TEXT("/ManualEditedClothing");
		TStrongObjectPtr<UAnimSequence> Edited(DuplicateObject<UAnimSequence>(Baked, CreatePackage(*PackageName), TEXT("ManualEditedClothing")));
		Edited->SetFlags(RF_Public | RF_Standalone);
		FName EditBone;
		for (const FName Bone : Motion->BoneNames) { if (Bone.ToString().StartsWith(TEXT("Skirt_"))) { EditBone = Bone; break; } }
		if (!Test.TestFalse(TEXT("Project proxy includes a skirt bone"), EditBone.IsNone())) { return; }
		TArray<FTransform> Transforms;
		Edited->GetDataModel()->GetBoneTrackTransforms(EditBone, Transforms);
		if (!Test.TestTrue(TEXT("Editable garment track exists"), Transforms.Num() > 2)) { return; }
		const int32 Mid = Transforms.Num() / 2;
		TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
		for (int32 Frame = 0; Frame < Transforms.Num(); ++Frame) {
			Positions.Add(FVector3f(Transforms[Frame].GetTranslation())); Scales.Add(FVector3f(Transforms[Frame].GetScale3D()));
			Rotations.Add(FQuat4f(Frame == Mid ? FQuat(FVector::XAxisVector, .35) * Transforms[Frame].GetRotation() : Transforms[Frame].GetRotation()));
		}
		Edited->GetController().SetBoneTrackKeys(EditBone, Positions, Rotations, Scales, false);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		if (!Test.TestTrue(TEXT("Manual edit fixture saved"), UPackage::SavePackage(Edited->GetPackage(), Edited.Get(), *FPackageName::LongPackageNameToFilename(PackageName, TEXT(".uasset")), Args))) { return; }
		const auto EditedGuid = KawaiiClothingReadback::AnimationRevision(Edited.Get());
		TStrongObjectPtr<UKawaiiClothingReadbackSettings> Review(NewObject<UKawaiiClothingReadbackSettings>());
		Review->Motion = Motion; Review->EditedAnimation = Edited.Get(); Review->BodyMesh = Settings.MainMesh;
		FKawaiiClothingReadbackPreview View;
		if (!Test.TestTrue(TEXT("Body plus manually edited clothing preview is available"), View.Read(*Review.Get(), Error))) { Test.AddError(Error); return; }
		if (auto* Profile = LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr, TEXT("/Game/BP/AnimeBP/Lime/DA_AnimBP_Lime_IKCorrection.DA_AnimBP_Lime_IKCorrection"))) {
			Review->CorrectionProfile = Profile; Review->CorrectionAlpha = 0.f;
			if (!Test.TestTrue(TEXT("Zero-weight correction keeps the authored preview available"), View.Read(*Review.Get(), Error))) { Test.AddError(Error); return; }
			Review->CorrectionAlpha = 1.f;
			if (!Test.TestTrue(TEXT("Edited clothing can drive the runtime hair correction preview"), View.Read(*Review.Get(), Error))) { Test.AddError(Error); return; }
		}
		Test.TestEqual(TEXT("Review contains the entire bake"), View.NumFrames(), Preview.Num());
		View.ShowFrame(Mid); View.ShowFrame(0); View.ShowFrame(Mid);
		TArray<FKawaiiClothingProxyCapsule> Before, After;
		KawaiiClothingProxy::SampleBlended({{Motion, float(Mid) / Settings.SampleRate, 1.f}}, Motion->RelativeTransform, Before, Error);
		KawaiiClothingProxy::SampleBlended({{View.GetCandidate(), float(Mid) / Settings.SampleRate, 1.f}}, Motion->RelativeTransform, After, Error);
		double Movement = 0.;
		for (int32 I = 0; I < FMath::Min(Before.Num(), After.Num()); ++I) { Movement = FMath::Max(Movement, FVector::Distance(Before[I].Start, After[I].Start)); Movement = FMath::Max(Movement, FVector::Distance(Before[I].End, After[I].End)); }
		Test.TestTrue(TEXT("Manual bone edit changes the reread collision trajectory"), Movement > .01);
		Test.TestTrue(TEXT("Preview leaves the old playback pairing untouched"), Motion->BakedAnimation.Get() == Baked);
		const auto OldRevision = Motion->MotionRevision;
		FString Backup;
		if (!Test.TestTrue(TEXT("Reviewed trajectories update the original proxy"), View.Publish(Backup, Error))) { Test.AddError(Error); return; }
		Test.TestTrue(TEXT("Original asset now pairs edited animation and fresh trajectories"), Motion->BakedAnimation.Get() == Edited.Get() && Motion->MotionRevision != OldRevision);
		Test.TestTrue(TEXT("Proxy backup is saved"), FPackageName::DoesPackageExist(FSoftObjectPath(Backup).GetLongPackageName()));
		Test.TestTrue(TEXT("New proxy is saved"), !Motion->GetPackage()->IsDirty());
		for (const auto& Asset : Assets) {
			if (auto* Set = Cast<UKawaiiClothingProxyMotionSet>(Asset.Get()); Set && Set->Motions.Contains(Motion)) {
				TArray<FKawaiiClothingProxyMotionSample> Resolved;
				Test.TestTrue(TEXT("Existing action mapping resolves without relinking"), UKawaiiClothingProxyPlaybackLibrary::ResolveProxyMotionSamples(Set, {{Settings.SourceAnimation, .5f, 1.f}}, Resolved, Error) && Resolved.Num() == 1 && Resolved[0].Motion == Motion);
			}
		}
		Test.TestTrue(TEXT("Readback never rewrites authored animation keys or body output"), KawaiiClothingReadback::AnimationRevision(Edited.Get()) == EditedGuid && KawaiiClothingReadback::AnimationRevision(Baked) == BakedGuid && KawaiiClothingReadback::AnimationRevision(Motion->BodyBakedAnimation.Get()) == BodyGuid);
		Rotations[Mid] = FQuat4f::Identity;
		Edited->GetController().SetBoneTrackKeys(EditBone, Positions, Rotations, Scales, false);
		Test.TestFalse(TEXT("Further manual changes invalidate the preview"), View.IsCurrent(Error));
		Test.TestFalse(TEXT("Stale preview cannot overwrite published trajectories"), View.Publish(Backup, Error));
		Test.AddInfo(FString::Printf(TEXT("Readback: %d preview/export frames, max position error %.6f cm; manual edit moved proxies %.4f cm; saved fixture %s"), Preview.Num(), MaxError, Movement, *PackageName));
	}
}
#endif
