#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace KawaiiIKRuntimeTests
{
	FString ObjectPath(FString Path) {
		if (!Path.Contains(TEXT("."))) {
			Path += TEXT(".") + FPaths::GetCleanFilename(Path);
		}
		return Path;
	}

	class FRunAttachedClothing : public IAutomationLatentCommand
	{
	public:
		FRunAttachedClothing(FAutomationTestBase* InTest, USkeletalMesh* InBodyMesh, USkeletalMesh* InClothingMesh, UAnimSequence* InAnimation, UAnimBlueprint* InGraph)
			: Test(InTest), BodyMesh(InBodyMesh), ClothingMesh(InClothingMesh), Animation(InAnimation), Graph(InGraph),
			  Scene(MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues().SetEditor(true).SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(false))) {
			StartSeconds = FPlatformTime::Seconds();
		}

		virtual ~FRunAttachedClothing() override {
			// PreviewScene references registered components; remove consumers before their source.
			if (Clothing.IsValid()) {
				Scene->RemoveComponent(Clothing.Get());
				Clothing->DestroyComponent();
				Clothing.Reset();
			}
			if (Body.IsValid()) {
				Scene->RemoveComponent(Body.Get());
				Body->DestroyComponent();
				Body.Reset();
			}
			if (Actor.IsValid()) {
				Actor->Destroy();
				Actor.Reset();
			}
		}

		virtual bool Update() override {
			if (FPlatformTime::Seconds() - StartSeconds > 60.0) {
				Test->AddError(TEXT("Attached clothing animation smoke test timed out."));
				return true;
			}
			if (!bInitialized) {
				if (!Initialize()) {
					return true;
				}
				bInitialized = true;
				LastFrame = GFrameCounter;
				return false;
			}
			if (LastFrame == GFrameCounter) {
				return false;
			}
			LastFrame = GFrameCounter;
			constexpr float Delta = 1.f / 60.f;
			Scene->GetWorld()->Tick(LEVELTICK_All, Delta);
			UAnimSingleNodeInstance* Source = Body->GetSingleNodeInstance();
			if (!Source) {
				Test->AddError(TEXT("Body single-node animation instance was replaced."));
				return true;
			}
			Source->SetPosition(FMath::Min(Step * Delta, Animation->GetPlayLength()), false);
			Body->TickAnimation(Delta, false);
			Body->RefreshBoneTransforms(nullptr);
			// This ordered synchronous evaluation mirrors the component prerequisite in gameplay.
			Clothing->TickAnimation(Delta, false);
			Clothing->RefreshBoneTransforms(nullptr);

			UAnimInstance* Instance = Clothing->GetAnimInstance();
			FAnimNode_KawaiiPhysicsIKCorrection* Node = FindCorrectionNode(Instance);
			if (!Node) {
				Test->AddError(TEXT("The real clothing Anim Instance has no unique correction node."));
				return true;
			}
			const TArray<FTransform>& Pose = Clothing->GetComponentSpaceTransforms();
			if (Pose.Num() < ClothingMesh->GetRefSkeleton().GetRawBoneNum()) {
				Test->AddError(TEXT("Real clothing evaluation returned an incomplete component pose."));
				return true;
			}
			for (const FTransform& Transform : Pose) {
				if (Transform.ContainsNaN() || !Transform.IsRotationNormalized()) {
					Test->AddError(TEXT("Real clothing evaluation produced an invalid transform."));
					return true;
				}
			}
			if (!FMath::IsFinite(Node->GetMaxPenetration()) || !FMath::IsFinite(Node->GetMaxLengthError())) {
				Test->AddError(TEXT("Correction diagnostics must remain finite."));
				return true;
			}
			if (++Step < 24) {
				return false;
			}

			Test->TestTrue(TEXT("Attached-parent correction ran through the real cached-pose graph"), Node->GetCorrectionDebugReason().StartsWith(TEXT("corrected")));
			Test->TestTrue(TEXT("Test graph enables the real attached-parent capsule path"), Node->Profile && Node->Profile->bUseAttachedParentForCollision);
			Test->TestTrue(TEXT("Real clothing retains its nonidentity attachment transform"),
						   Clothing->GetRelativeTransform().Equals(FTransform(FQuat::Identity, FVector(0, 0, -1), FVector(1.01)), 0.0001));
			Test->AddInfo(FString::Printf(TEXT("24 real component frames: %s; remaining penetration %.6f cm; length error %.6f cm. Constraint residuals are recorded, not required to be zero."),
										  *Node->GetCorrectionDebugReason(),
										  Node->GetMaxPenetration(),
										  Node->GetMaxLengthError()));
			return true;
		}

	private:
		static FAnimNode_KawaiiPhysicsIKCorrection* FindCorrectionNode(UAnimInstance* Instance) {
			if (!Instance) {
				return nullptr;
			}
			const IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(Instance->GetClass());
			if (!Interface) {
				return nullptr;
			}
			FAnimNode_KawaiiPhysicsIKCorrection* Result = nullptr;
			for (const FStructProperty* Property : Interface->GetAnimNodeProperties()) {
				if (Property->Struct->IsChildOf(FAnimNode_KawaiiPhysicsIKCorrection::StaticStruct())) {
					if (Result) {
						return nullptr;
					}
					Result = Property->ContainerPtrToValuePtr<FAnimNode_KawaiiPhysicsIKCorrection>(Instance);
				}
			}
			return Result;
		}

		bool Initialize() {
			FActorSpawnParameters Spawn;
			Spawn.ObjectFlags = RF_Transient;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor.Reset(Scene->GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn));
			if (!Actor.IsValid()) {
				Test->AddError(TEXT("Could not create the transient preview actor."));
				return false;
			}
			Actor->SetActorTickEnabled(false);
			Body.Reset(NewObject<USkeletalMeshComponent>(Actor.Get(), TEXT("IKSmokeBody"), RF_Transient));
			Clothing.Reset(NewObject<USkeletalMeshComponent>(Actor.Get(), TEXT("IKSmokeClothing"), RF_Transient));
			Actor->AddInstanceComponent(Body.Get());
			Actor->AddInstanceComponent(Clothing.Get());
			Actor->SetRootComponent(Body.Get());
			const auto Configure = [](USkeletalMeshComponent* Component, USkeletalMesh* Mesh) {
				Component->SetComponentTickEnabled(false);
				Component->bEnableUpdateRateOptimizations = false;
				Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
				Component->SetDisablePostProcessBlueprint(true);
				Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Component->SetSkeletalMesh(Mesh);
				Component->SetForcedLOD(1);
			};
			Configure(Body.Get(), BodyMesh.Get());
			Configure(Clothing.Get(), ClothingMesh.Get());
			Clothing->SetupAttachment(Body.Get());
			Clothing->SetRelativeTransform(FTransform(FQuat::Identity, FVector(0, 0, -1), FVector(1.01)));
			Clothing->AddTickPrerequisiteComponent(Body.Get());
			Scene->AddComponent(Body.Get(), FTransform::Identity);
			Scene->AddComponent(Clothing.Get(), Clothing->GetRelativeTransform());
			Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			UAnimSingleNodeInstance* Source = Body->GetSingleNodeInstance();
			if (!Source) {
				Test->AddError(TEXT("Could not initialize the body animation source."));
				return false;
			}
			Source->SetAnimationAsset(Animation.Get(), false, 1.f);
			Source->SetPlaying(false);
			Source->SetRootMotionMode(ERootMotionMode::NoRootMotionExtraction);
			Source->bUseMultiThreadedAnimationUpdate = false;
			Clothing->SetAnimInstanceClass(Graph->GeneratedClass);
			if (!Clothing->GetAnimInstance()) {
				Test->AddError(TEXT("Could not initialize the clothing blueprint."));
				return false;
			}
			Clothing->GetAnimInstance()->bUseMultiThreadedAnimationUpdate = false;
			Body->SetComponentTickEnabled(false);
			Clothing->SetComponentTickEnabled(false);
			return true;
		}

		FAutomationTestBase* Test;
		TStrongObjectPtr<USkeletalMesh> BodyMesh, ClothingMesh;
		TStrongObjectPtr<UAnimSequence> Animation;
		TStrongObjectPtr<UAnimBlueprint> Graph;
		TStrongObjectPtr<AActor> Actor;
		TStrongObjectPtr<USkeletalMeshComponent> Body, Clothing;
		TUniquePtr<FPreviewScene> Scene;
		double StartSeconds = 0.0;
		uint64 LastFrame = MAX_uint64;
		int32 Step = 0;
		bool bInitialized = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKRuntimeAttachedClothing, "KawaiiPhysicsBaker.IKCorrection.RuntimeAttachedClothing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiIKRuntimeAttachedClothing::RunTest(const FString&) {
	FString BodyPath, ClothingPath, AnimationPath, GraphPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("KawaiiBakeTestMesh="), BodyPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("KawaiiBakeTestClothing="), ClothingPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("KawaiiBakeTestAnimation="), AnimationPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("KawaiiIKTestBlueprint="), GraphPath))
	{
		AddInfo(TEXT("Real component smoke test requires KawaiiBakeTestMesh, KawaiiBakeTestClothing, KawaiiBakeTestAnimation and KawaiiIKTestBlueprint; asset work skipped."));
		return true;
	}
	using namespace KawaiiIKRuntimeTests;
	USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(nullptr, *ObjectPath(BodyPath));
	USkeletalMesh* ClothingMesh = LoadObject<USkeletalMesh>(nullptr, *ObjectPath(ClothingPath));
	UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *ObjectPath(AnimationPath));
	UAnimBlueprint* Graph = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath(GraphPath));
	if (!TestNotNull(TEXT("Body mesh"), BodyMesh)
		|| !TestNotNull(TEXT("Clothing mesh"), ClothingMesh)
		|| !TestNotNull(TEXT("Body animation"), Animation)
		|| !TestNotNull(TEXT("Corrected clothing blueprint"), Graph))
	{
		return false;
	}
	if (!TestTrue(TEXT("Body animation matches the body Skeleton"), Animation->GetSkeleton() == BodyMesh->GetSkeleton())
		|| !TestTrue(TEXT("Clothing graph matches its independent Skeleton"), Graph->TargetSkeleton == ClothingMesh->GetSkeleton())
		|| !TestTrue(TEXT("Clothing blueprint has a valid compiled Anim Instance class"), Graph->GeneratedClass && Graph->GeneratedClass->IsChildOf(UAnimInstance::StaticClass())))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FRunAttachedClothing(this, BodyMesh, ClothingMesh, Animation, Graph));
	return true;
}

#endif
