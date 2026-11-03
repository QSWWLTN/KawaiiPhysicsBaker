#include "KawaiiPhysicsBakeSession.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "KawaiiPhysicsClothingCollisionComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiClothingProxyAsset.h"
#include "KawaiiPhysicsBakeBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace {
	class FPreviewProxySessionCommand : public IAutomationLatentCommand
	{
	public:
		FPreviewProxySessionCommand(FAutomationTestBase* InTest, UAnimBlueprint* InBlueprint)
			: Test(InTest), Blueprint(InBlueprint), Session(MakeShared<FKawaiiPhysicsBakeSession>()), StartedAt(FPlatformTime::Seconds()) {
		}

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
			if (++Steps < 8) {
				return false;
			}
			Steps = 0;
			USkeletalMeshComponent* Main = Session->GetMainComponent();
			if (Stage == 1) {
				UKawaiiPhysicsClothingCollisionComponent* Bridge = Main->GetOwner()->FindComponentByClass<UKawaiiPhysicsClothingCollisionComponent>();
				if (!Test->TestNotNull(TEXT("New proxy preview creates its own bridge without a legacy asset"), Bridge)) {
					return true;
				}
				const auto Frame = Bridge->GetFrameForTarget(Main, TEXT("Hair_back"));
				if (Test->TestTrue(TEXT("Back hair receives a finalized clothing proxy frame"), Frame.IsValid())) {
					Test->TestTrue(TEXT("Clothing geometry is published, not just an empty bridge"), Frame->bDynamicValid);
					Test->TestEqual(TEXT("All shapes from the new definition arrive at the consumer"), Frame->DynamicCapsules.Num(), Settings->Parts[0].ProxyDefinition->Shapes.Num());
					Test->TestTrue(TEXT("Legacy geometry is absent"), Frame->StaticCapsules.IsEmpty() && Frame->ComponentCapsules.IsEmpty());
				}
				Test->TestFalse(TEXT("The ignored tail does not receive the shared clothing proxy frame"), Bridge->GetFrameForTarget(Main, TEXT("Tail_Root")).IsValid());
				Session->SeekPreview(0);
				Test->TestEqual(TEXT("Collision preview can rewind its cached poses"), Session->GetPreviewFrame(), 0);
				Session->Cancel();
				// Keep both bake runs in warm-up so the test never creates output assets.
				Settings->WarmUpFrames = 3600;
				if (!Session->StartBake(Error)) {
					Test->AddError(Error);
					return true;
				}
				Test->TestNull(TEXT("Starting a bake destroys the preview-only bridge"), Session->GetMainComponent()->GetOwner()->FindComponentByClass<UKawaiiPhysicsClothingCollisionComponent>());
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
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TStrongObjectPtr<UAnimBlueprint> Blueprint;
		TSharedRef<FKawaiiPhysicsBakeSession> Session;
		TArray<FTransform> Baseline;
		double StartedAt;
		int32 Stage = 0;
		int32 Steps = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiPreviewProxySessionTest, "KawaiiPhysicsBaker.PreviewProxy.ProjectSessionIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
	Settings->CollisionSourcePart = Settings->Parts[0].Name;
	Settings->PreviewCollisionRootBones.Reset();
	Settings->Parts[0].ProxyDefinition = DuplicateObject<UKawaiiClothingProxyAsset>(Settings->Parts[0].ProxyDefinition, Package, TEXT("PreviewProxyDefinition"));
	Settings->Parts[0].ProxyDefinition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	Settings->Parts[0].ProxyMotionSet = nullptr;
	Settings->WarmUpFrames = 0;
	Settings->OutputDirectory.Path = TEXT("/Game/KawaiiBakes");
	Settings->OutputPrefix = TEXT("PreviewIsolationUnused");
	ADD_LATENT_AUTOMATION_COMMAND(FPreviewProxySessionCommand(this, Copy));
	return true;
}

#endif
