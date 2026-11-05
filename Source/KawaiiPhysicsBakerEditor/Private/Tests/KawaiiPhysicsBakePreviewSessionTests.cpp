#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "KawaiiPhysicsClothingCollisionComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsBakeSession.h"
#include "KawaiiClothingProxyAsset.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "KawaiiClothingProxySnapshot.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace {
	class FPreviewProxySessionCommand : public IAutomationLatentCommand
	{
	public:
		FPreviewProxySessionCommand(FAutomationTestBase* InTest, UAnimBlueprint* InBlueprint)
			: Test(InTest), Blueprint(InBlueprint), Session(MakeShared<FKawaiiPhysicsBakeSession>()), StartedAt(FPlatformTime::Seconds()) {}

		virtual bool Update() override {
			if (FPlatformTime::Seconds() - StartedAt > 30.0) {
				Test->AddError(TEXT("Preview collision session did not advance within 30 seconds."));
				Session->Cancel();
				return true;
			}
			UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Blueprint.Get());
			FString Error;
			if (Stage == 0) {
				if (!Session->Initialize(Blueprint.Get(), Error) || !Session->StartPreview(Error)) {
					Test->AddError(Error);
					return true;
				}
				Test->TestEqual(TEXT("Preview does not modify the saved warm-up setting"), Settings->WarmUpFrames, 0);
				Stage = 1;
				return false;
			}
			Session->Tick(1.f / 60.f);
			if (!Session->IsRunning()) {
				Test->AddError(Session->GetStatus().ToString());
				return true;
			}
			USkeletalMeshComponent* Main = Session->GetMainComponent();
			if (Stage == 1) {
				if (Session->GetCachedFrameCount() > NativeFrames.Num()) {
					if (!CheckRuntimePose(*Main, *Settings)) {
						return true;
					}
					NativeFrames.Add(Main->GetBoneSpaceTransforms());
				}
				if (Session->GetCachedFrameCount() <= Session->GetLastFrame()) {
					return false;
				}
			} else if (Stage == 4) {
				const int32 Frame = Session->GetCachedFrameCount() - 1;
				if (NativeFrames.IsValidIndex(Frame)) {
					const TArray<FTransform> NativePose = Main->GetBoneSpaceTransforms();
					for (int32 Bone = 0; Bone < NativePose.Num(); ++Bone) {
						if (!NativePose[Bone].Equals(NativeFrames[Frame][Bone], 0.001f)) {
							Test->AddError(FString::Printf(TEXT("Preview correction fed back into the bake baseline at frame %d bone %d."), Frame, Bone));
							return true;
						}
					}
				}
				if (Session->GetCachedFrameCount() <= Session->GetLastFrame()) {
					return false;
				}
				Test->AddInfo(FString::Printf(
					TEXT("Preview/runtime parity: %d frames, maximum bone position error %.6f cm. Native bake poses unchanged."),
					NativeFrames.Num(),
					MaxPositionError
				));
				Session->Cancel();
				return true;
			}
			if (Stage != 1 && ++Steps < 8) {
				return false;
			}
			Steps = 0;
			if (Stage == 1) {
				UKawaiiPhysicsClothingCollisionComponent* Bridge = Main->GetOwner()->FindComponentByClass<UKawaiiPhysicsClothingCollisionComponent>();
				Test->TestNull(TEXT("Runtime preview does not inject previous-frame collision into native Kawaii simulation"), Bridge);
				Test->TestTrue(
					TEXT("Preview identifies the runtime correction profile"),
					Session->GetCollisionStatus().ToString().Contains(Settings->PreviewCorrectionProfile->GetName())
				);
				Session->SeekPreview(0);
				Test->TestEqual(TEXT("Collision preview can rewind its cached poses"), Session->GetPreviewFrame(), 0);
				Session->Cancel();
				// Keep both bake runs in warm-up so the test never creates output assets.
				Settings->WarmUpFrames = 3600;
				if (!Session->StartBake(Error)) {
					Test->AddError(Error);
					return true;
				}
				Test->TestNull(
					TEXT("Starting a bake destroys the preview-only bridge"),
					Session->GetMainComponent()->GetOwner()->FindComponentByClass<UKawaiiPhysicsClothingCollisionComponent>()
				);
				Stage = 2;
				return false;
			}
			if (Stage == 2) {
				Baseline = Main->GetComponentSpaceTransforms();
				Session->Cancel();
				Settings->bPreviewClothingProxyCollision = false;
				if (!Session->StartBake(Error)) {
					Test->AddError(Error);
					return true;
				}
				Stage = 3;
				return false;
			}
			const TArray<FTransform>& WithoutPreview = Main->GetComponentSpaceTransforms();
			if (Test->TestEqual(TEXT("Bake pose bone count does not depend on the preview switch"), WithoutPreview.Num(), Baseline.Num())) {
				for (int32 Bone = 0; Bone < Baseline.Num(); ++Bone) {
					if (!WithoutPreview[Bone].Equals(Baseline[Bone], 1.e-5)) {
						Test->AddError(FString::Printf(TEXT("Preview switch changed bake pose at bone %d."), Bone));
						break;
					}
				}
			}
			Test->TestTrue(TEXT("Isolation test does not create exported assets"), Session->GetOutputAssets().IsEmpty());
			Session->Cancel();
			Settings->WarmUpFrames = 0;
			if (!Session->StartPreview(Error)) {
				Test->AddError(Error);
				return true;
			}
			Stage = 4;
			return false;
		}

	private:
		bool CheckRuntimePose(USkeletalMeshComponent& Main, UKawaiiPhysicsBakeSettings& Settings) {
			USkeletalMeshComponent* Garment = nullptr;
			TInlineComponentArray<USkeletalMeshComponent*> Components(Main.GetOwner());
			for (USkeletalMeshComponent* Component : Components) {
				if (Component->GetFName() == Settings.CollisionSourcePart) {
					Garment = Component;
					break;
				}
			}
			if (!Test->TestNotNull(TEXT("Current-frame garment component"), Garment)) {
				return false;
			}
			FString Error;
			const UKawaiiClothingProxyAsset& Definition = *Settings.Parts[0].ProxyDefinition;
			if (RuntimeSnapshot.BoneNames.IsEmpty()) {
				if (!KawaiiClothingProxy::BuildMinimalHierarchy(
						Garment->GetSkeletalMeshAsset()->GetRefSkeleton(),
						Definition.Shapes,
						RuntimeSnapshot.BoneNames,
						RuntimeSnapshot.ParentIndices,
						SourceBones,
						Error
					))
				{
					Test->AddError(Error);
					return false;
				}
				RuntimeSnapshot.DefinitionIdentity = FSoftObjectPath(&Definition);
				RuntimeSnapshot.DefinitionSignature = Definition.ComputeSignature();
				RuntimeSnapshot.SourceMesh = FSoftObjectPath(Garment->GetSkeletalMeshAsset());
				RuntimeSnapshot.Shapes = Definition.Shapes;
				RuntimeSnapshot.IgnoredTargetRootBones = Definition.IgnoredTargetRootBones;
				RuntimeSnapshot.SampleRate = Settings.SampleRate;
				RuntimeSnapshot.SourceDuration = 1.f / Settings.SampleRate;
				RuntimeSnapshot.NumFrames = 2;
				RuntimeSnapshot.LocalKeys.SetNum(SourceBones.Num() * 2);
				RuntimeCorrection.Profile = Settings.PreviewCorrectionProfile;
			}
			// The export records local animation tracks. Compare against those, not the preview adapter's CS conversion.
			const TArray<FTransform> GarmentLocal = Garment->GetBoneSpaceTransforms();
			for (int32 Bone = 0; Bone < SourceBones.Num(); ++Bone) {
				RuntimeSnapshot.LocalKeys[Bone] = RuntimeSnapshot.LocalKeys[Bone + SourceBones.Num()] = GarmentLocal[SourceBones[Bone]];
			}
			RuntimeSnapshot.RelativeTransform = Garment->GetComponentTransform().GetRelativeTransform(Main.GetComponentTransform());
			KawaiiClothingProxy::FWeightedSnapshot Sample;
			Sample.Motion = &RuntimeSnapshot;
			FMemMark Mark(FMemStack::Get());
			UAnimInstance* Instance = Main.GetAnimInstance();
			FAnimInstanceProxy Proxy(Instance);
			Proxy.GetRequiredBones() = Instance->GetRequiredBones();
			FPoseContext Output(&Proxy);
			const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
			const TArray<FTransform> Local = Main.GetBoneSpaceTransforms();
			for (int32 Bone = 0; Bone < Output.Pose.GetNumBones(); ++Bone) {
				const FCompactPoseBoneIndex Compact(Bone);
				Output.Pose[Compact] = Local[Bones.MakeMeshPoseIndex(Compact).GetInt()];
			}
			TArray<FName> ExcludedRoots;
			for (const auto& Chain : Settings.PreviewCorrectionProfile->Chains) {
				ExcludedRoots.Add(Chain.RootBone.BoneName);
			}
			KawaiiClothingProxy::FLiveBodyPose LiveBody;
			if (!KawaiiClothingProxy::BuildLiveBodyPose(Output.Pose, ExcludedRoots, LiveBody, Error)
				|| !KawaiiClothingProxy::SampleSnapshots(
					{Sample},
					RuntimeSnapshot.RelativeTransform,
					RuntimeCorrection.ExternalCapsules,
					Error,
					nullptr,
					&LiveBody
				))
			{
				Test->AddError(Error);
				return false;
			}
			FPoseContext BaselinePose(Output);
			BaselinePose.Pose.CopyBonesFrom(Output.Pose);
			RuntimeCorrection.PreUpdate(Instance);
			RuntimeCorrection.Alpha = Settings.PreviewCorrectionAlpha;
			RuntimeCorrection.AccumulateDeltaTime(1.f / Settings.SampleRate);
			RuntimeCorrection.CorrectPose(Output, BaselinePose);
			TArray<FTransform> Expected;
			Expected.SetNum(Output.Pose.GetNumBones());
			const TArray<FTransform>& PreviewPose = Main.GetComponentSpaceTransforms();
			for (int32 Bone = 0; Bone < Expected.Num(); ++Bone) {
				const FCompactPoseBoneIndex Compact(Bone);
				const int32 Parent = Bones.GetParentBoneIndex(Compact).GetInt();
				Expected[Bone] = Parent == INDEX_NONE ? Output.Pose[Compact] : Output.Pose[Compact] * Expected[Parent];
				Expected[Bone].NormalizeRotation();
				const FTransform& Actual = PreviewPose[Bones.MakeMeshPoseIndex(Compact).GetInt()];
				MaxPositionError = FMath::Max(MaxPositionError, FVector::Distance(Expected[Bone].GetTranslation(), Actual.GetTranslation()));
				if (!Actual.Equals(Expected[Bone], 0.002f)) {
					Test->AddError(FString::Printf(TEXT("Preview/runtime mismatch at frame %d bone %d (%.6f cm)."), NativeFrames.Num(), Bone, MaxPositionError)
					);
					return false;
				}
			}
			return true;
		}

		FAutomationTestBase* Test;
		TStrongObjectPtr<UAnimBlueprint> Blueprint;
		TSharedRef<FKawaiiPhysicsBakeSession> Session;
		TArray<FTransform> Baseline;
		TArray<TArray<FTransform>> NativeFrames;
		KawaiiClothingProxy::FSnapshot RuntimeSnapshot;
		TArray<int32> SourceBones;
		FAnimNode_KawaiiPhysicsIKCorrection RuntimeCorrection;
		double MaxPositionError = 0.0;
		double StartedAt;
		int32 Stage = 0;
		int32 Steps = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiPreviewProxySessionTest,
	"KawaiiPhysicsBaker.PreviewProxy.ProjectSessionIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiPreviewProxySessionTest::RunTest(const FString&) {
	// Optional project fixture: the duplicate may compile itself; the user's asset is never edited or saved.
	if (!FPackageName::DoesPackageExist(TEXT("/Game/KawaiiBakes/ABP_KawaiiBake"))) {
		AddWarning(TEXT("Project preview fixture is unavailable; session integration check skipped."));
		return true;
	}
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/KawaiiBakes/ABP_KawaiiBake.ABP_KawaiiBake"));
	if (!Source) {
		AddWarning(TEXT("Project preview fixture is unavailable; session integration check skipped."));
		return true;
	}
	// Blueprint duplication reinstantiates generated classes; give it a private package so
	// its CDO cannot collide with an earlier transient REINST class of the source blueprint.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/KawaiiProxyPreviewTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	UAnimBlueprint* Copy = DuplicateObject<UAnimBlueprint>(Source, Package, TEXT("ABP_ProxyPreviewTest"));
	UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Copy);
	if (!Settings || Settings->Parts.IsEmpty() || !Settings->Parts[0].ProxyDefinition) {
		AddWarning(TEXT("Project fixture has no clothing proxy definition; session integration check skipped."));
		return true;
	}
	Settings->bEnableClothingCollision = false;
	Settings->ClothingCollisionProfile = nullptr;
	Settings->bPreviewClothingProxyCollision = true;
	Settings->PreviewCorrectionProfile =
		LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr, TEXT("/Game/BP/AnimeBP/Lime/DA_AnimBP_Lime_IKCorrection.DA_AnimBP_Lime_IKCorrection"));
	if (!TestNotNull(TEXT("Project runtime correction profile"), Settings->PreviewCorrectionProfile.Get())) {
		return false;
	}
	Settings->CollisionSourcePart = Settings->Parts[0].Name;
	Settings->PreviewCollisionRootBones.Reset();
	Settings->Parts[0].ProxyDefinition =
		DuplicateObject<UKawaiiClothingProxyAsset>(Settings->Parts[0].ProxyDefinition, Package, TEXT("PreviewProxyDefinition"));
	Settings->Parts[0].ProxyDefinition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	Settings->Parts[0].ProxyMotionSet = nullptr;
	Settings->WarmUpFrames = 0;
	Settings->OutputDirectory.Path = TEXT("/Game/KawaiiBakes");
	Settings->OutputPrefix = TEXT("PreviewIsolationUnused");
	ADD_LATENT_AUTOMATION_COMMAND(FPreviewProxySessionCommand(this, Copy));
	return true;
}

#endif
