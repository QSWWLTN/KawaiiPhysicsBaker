#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "AnimNode_KawaiiAutoClothingProxy.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

// Inspect the real node's cache and seed a previous successful output. The public
// PreUpdate/Evaluate paths below still decide when that data must be discarded.
struct FKawaiiAutoClothingProxyTestAccess
{
	static int32 TrackCount(const FAnimNode_KawaiiAutoClothingProxy& Node) { return Node.Tracks.Num(); }

	static bool HasAction(const FAnimNode_KawaiiAutoClothingProxy& Node, const FSoftObjectPath& Path) { return Node.ActionMap.Contains(Path); }

	static const FString& SnapshotError(const FAnimNode_KawaiiAutoClothingProxy& Node) { return Node.SnapshotError; }
	static bool BuildActionSamples(FAnimNode_KawaiiAutoClothingProxy& Node,
		const TArray<KawaiiClothingProxy::FTrackedAction>& Actions, TArray<KawaiiClothingProxy::FWeightedSnapshot>& Samples) {
		return Node.BuildActionSamples(Actions, Samples);
	}
	static float CorrectionAlpha(const FAnimNode_KawaiiAutoClothingProxy& Node) { return Node.Correction.Alpha; }

	static void SeedPreviousCapsules(FAnimNode_KawaiiAutoClothingProxy& Node) {
		FKawaiiClothingProxyCapsule Capsule;
		Capsule.Id = TEXT("PreviousGarment");
		Capsule.Radius = 3.f;
		Node.Correction.ExternalCapsules = {Capsule};
		Node.CapsuleCount = 1;
	}

	static bool HasNoPreviousCapsules(const FAnimNode_KawaiiAutoClothingProxy& Node) {
		return Node.Correction.ExternalCapsules.IsEmpty() && Node.CapsuleCount == 0;
	}

	static bool SampleFirstTrack(const FAnimNode_KawaiiAutoClothingProxy& Node, TArray<FKawaiiClothingProxyCapsule>& Out, FString& Error) {
		if (Node.Tracks.IsEmpty()) {
			return false;
		}
		KawaiiClothingProxy::FWeightedSnapshot Sample;
		Sample.Motion = Node.Tracks[0].Get();
		Sample.IgnoredTargetRootBones = &Node.IgnoredRoots[0];
		return KawaiiClothingProxy::SampleSnapshots({Sample}, FTransform::Identity, Out, Error);
	}
};

namespace KawaiiAutoProxyTests {
	struct FConstantPose : FAnimNode_Base
	{
		virtual void Evaluate_AnyThread(FPoseContext& Output) override {
			Output.ResetToRefPose();
			Output.Pose[FCompactPoseBoneIndex(0)].SetTranslation(FVector(11, 4, 3));
		}
	};

	UKawaiiClothingProxyMotion* MakeMotion(USkeletalMesh* Mesh, const TCHAR* SourcePath, double CenterX) {
		UKawaiiClothingProxyAsset* Definition = NewObject<UKawaiiClothingProxyAsset>();
		Definition->PreviewMesh = Mesh;
		FKawaiiClothingProxyShape Shape;
		Shape.Id = TEXT("Collar");
		Shape.StartBone = TEXT("root");
		Shape.StartOffset = Shape.EndOffset = FVector(CenterX, 0, 0);
		Shape.Radius = 2.f;
		Definition->Shapes = {Shape};
		UKawaiiClothingProxyMotion* Motion = NewObject<UKawaiiClothingProxyMotion>();
		Motion->Definition = Definition;
		Motion->DefinitionSignature = Definition->ComputeSignature();
		Motion->SourceMesh = Mesh;
		Motion->SourceAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(SourcePath));
		Motion->SampleRate = 1;
		Motion->NumFrames = 2;
		Motion->SourceDuration = 1.f;
		Motion->BoneNames = {TEXT("root")};
		Motion->ParentIndices = {INDEX_NONE};
		Motion->Shapes = Definition->Shapes;
		Motion->LocalKeys = {FTransform::Identity, FTransform::Identity};
		return Motion;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiAutoProxyDynamicEquipmentTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutoNode.DynamicEquipment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiAutoProxyDynamicEquipmentTest::RunTest(const FString&) {
	using Access = FKawaiiAutoClothingProxyTestAccess;
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>());
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
	Mesh->SetSkeleton(Skeleton.Get());
	Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh.Get());
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	const TArray<FBoneIndexType> BoneIndices{0};
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	FMemMark Mark(FMemStack::Get());
	KawaiiAutoProxyTests::FConstantPose Input;
	FAnimNode_KawaiiAutoClothingProxy Node;
	Node.SourcePose.SetLinkNode(&Input);
	Node.Initialize_AnyThread(FAnimationInitializeContext(&Proxy));
	Node.CacheBones_AnyThread(FAnimationCacheBonesContext(&Proxy));
	TStrongObjectPtr<UKawaiiClothingProxyMotion> A(KawaiiAutoProxyTests::MakeMotion(Mesh.Get(), TEXT("/Game/AutoProxyTest/Walk.Walk"), 10));
	TStrongObjectPtr<UKawaiiClothingProxyMotion> B(KawaiiAutoProxyTests::MakeMotion(Mesh.Get(), TEXT("/Game/AutoProxyTest/Run.Run"), 30));
	TStrongObjectPtr<UKawaiiClothingProxyMotionSet> SetA(NewObject<UKawaiiClothingProxyMotionSet>());
	TStrongObjectPtr<UKawaiiClothingProxyMotionSet> SetB(NewObject<UKawaiiClothingProxyMotionSet>());
	SetA->Motions = {A.Get()};
	SetB->Motions = {B.Get()};
	const auto Evaluate = [&]() {
		FAnimationUpdateSharedContext SharedContext;
		Node.Update_AnyThread(FAnimationUpdateContext(&Proxy, 1.f / 60.f, &SharedContext));
		FPoseContext Output(&Proxy);
		Node.Evaluate_AnyThread(Output);
		TestTrue(TEXT("Fallback preserves the current input pose"), Output.Pose[FCompactPoseBoneIndex(0)].GetTranslation().Equals(FVector(11, 4, 3)));
	};
	const auto ExpectTransition = [&]() {
		Access::SeedPreviousCapsules(Node);
		Evaluate();
		TestEqual(
			TEXT("New pin value waits for its own game-thread snapshot"),
			Node.GetProxyDebugReason(),
			FString(TEXT("clothing_list_changed_waiting_for_game_thread_snapshot"))
		);
		TestTrue(TEXT("The previous garment is removed during the transition frame"), Access::HasNoPreviousCapsules(Node));
	};
	Node.ProxyMotionSet = SetA.Get();
	Node.PreUpdate(Instance.Get());
	TestTrue(TEXT("Initial asset list builds successfully"), Access::SnapshotError(Node).IsEmpty());
	TestEqual(TEXT("One motion is snapshotted"), Access::TrackCount(Node), 1);
	TestTrue(TEXT("Initial action map is installed"), Access::HasAction(Node, A->SourceAnimation.ToSoftObjectPath()));

	// Simulate exposed inputs changing after PreUpdate: the same ordering used by
	// threaded animation updates during a wardrobe change.
	Node.ProxyMotionSet = SetB.Get();
	ExpectTransition();
	Node.PreUpdate(Instance.Get());
	TestTrue(TEXT("The new outfit is available after the next game-thread snapshot"), Access::SnapshotError(Node).IsEmpty());
	TestFalse(TEXT("The previous outfit's lookup is removed"), Access::HasAction(Node, A->SourceAnimation.ToSoftObjectPath()));
	TestTrue(TEXT("The new outfit's lookup replaces it"), Access::HasAction(Node, B->SourceAnimation.ToSoftObjectPath()));
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	FString Error;
	TestTrue(TEXT("The new immutable tracks can be sampled"), Access::SampleFirstTrack(Node, Capsules, Error));
	if (TestEqual(TEXT("One new collar is sampled"), Capsules.Num(), 1)) {
		TestTrue(TEXT("Sampling uses the new garment geometry"), Capsules[0].Start.Equals(FVector(30, 0, 0)));
	}

	Node.ProxyMotionSet = nullptr;
	// Manual readback keeps the same asset, frame count and mappings while replacing its keys.
	B->LocalKeys[0].AddToTranslation(FVector(0, 7, 0));
	B->LocalKeys[1].AddToTranslation(FVector(0, 7, 0));
	B->MotionRevision = FGuid::NewGuid();
	Node.ProxyMotionSet = SetB.Get();
	Node.PreUpdate(Instance.Get());
	TestTrue(TEXT("Same-asset readback revision refreshes immutable runtime tracks"), Access::SampleFirstTrack(Node, Capsules, Error) && Capsules.Num() == 1 && FMath::IsNearlyEqual(Capsules[0].Start.Y, 7., .0001));
	Node.ProxyMotionSet = nullptr;
	ExpectTransition();
	Node.PreUpdate(Instance.Get());
	Access::SeedPreviousCapsules(Node);
	Evaluate();
	TestEqual(TEXT("Unequipping becomes a stable empty-list fallback"), Node.GetProxyDebugReason(), FString(TEXT("no_clothing_proxy_assets")));
	TestEqual(TEXT("Unequipping releases old snapshots"), Access::TrackCount(Node), 0);
	TestTrue(TEXT("Unequipping cannot reuse old collision output"), Access::HasNoPreviousCapsules(Node));

	Node.ProxyMotions = {A.Get()};
	Node.PreUpdate(Instance.Get());
	Node.ProxyMotions = {B.Get()};
	ExpectTransition();
	Node.PreUpdate(Instance.Get());
	TestTrue(TEXT("Explicit dynamic asset arrays support the same replacement"), Access::HasAction(Node, B->SourceAnimation.ToSoftObjectPath()));
	Node.ProxyMotions.Reset();
	ExpectTransition();
	Node.PreUpdate(Instance.Get());
	TestEqual(TEXT("Clearing the explicit list releases its tracks"), Access::TrackCount(Node), 0);

	Node.ProxyMotionSet = SetB.Get();
	Node.PreUpdate(Instance.Get());
	SetB->Motions = {A.Get()};
	Node.PreUpdate(Instance.Get());
	TestTrue(TEXT("Set content edits are detected at the next game-thread snapshot"), Access::HasAction(Node, A->SourceAnimation.ToSoftObjectPath()));
	TestFalse(TEXT("Set content edits remove replaced entries"), Access::HasAction(Node, B->SourceAnimation.ToSoftObjectPath()));
	A->Definition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	Node.PreUpdate(Instance.Get());
	TestTrue(TEXT("Filter changes update the light snapshot without rebaking"), Access::SampleFirstTrack(Node, Capsules, Error));
	if (Capsules.Num() == 1) {
		TestTrue(TEXT("The current ignore rules reach sampled capsules"), Capsules[0].IgnoredTargetRootBones.Contains(TEXT("Tail_Root")));
	}

	TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> NewProfile(NewObject<UKawaiiPhysicsIKCorrectionProfile>());
	Node.Profile = NewProfile.Get();
	ExpectTransition();
	Node.PreUpdate(Instance.Get());
	SetB->Motions = {nullptr};
	Node.PreUpdate(Instance.Get());
	TestFalse(TEXT("An invalid replacement has an actionable error"), Access::SnapshotError(Node).IsEmpty());
	TestEqual(TEXT("A failed replacement removes old snapshots"), Access::TrackCount(Node), 0);
	Access::SeedPreviousCapsules(Node);
	Evaluate();
	TestTrue(TEXT("Invalid equipment cannot keep stale capsules"), Access::HasNoPreviousCapsules(Node));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiAutoProxyBlendOutTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutoNode.BlendToUnmappedAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiAutoProxyBlendOutTest::RunTest(const FString&) {
	using Access = FKawaiiAutoClothingProxyTestAccess;
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>());
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
	Mesh->SetSkeleton(Skeleton.Get());
	Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Climb(KawaiiAutoProxyTests::MakeMotion(
		Mesh.Get(), TEXT("/Game/AutoProxyTest/Climb.Climb"), 10));
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Land(KawaiiAutoProxyTests::MakeMotion(
		Mesh.Get(), TEXT("/Game/AutoProxyTest/Land.Land"), 10));
	// Mapped actions share one garment definition, as required by the production snapshot cache.
	Land->Definition = Climb->Definition;
	Land->DefinitionSignature = Climb->DefinitionSignature;
	FAnimNode_KawaiiAutoClothingProxy Node;
	Node.ProxyMotions = {Climb.Get(), Land.Get()};
	Node.Alpha = 0.8f;
	Node.PreUpdate(nullptr);
	if (!TestTrue(TEXT("Compatible garment snapshots are ready"), Access::SnapshotError(Node).IsEmpty())) {
		return false;
	}
	KawaiiClothingProxy::FTrackedAction ClimbAction, LandAction, IdleAction;
	ClimbAction.Animation = Climb->SourceAnimation.ToSoftObjectPath();
	ClimbAction.Time = 0.7f;
	LandAction.Animation = Land->SourceAnimation.ToSoftObjectPath();
	IdleAction.Animation = FSoftObjectPath(TEXT("/Game/AutoProxyTest/Idle.Idle"));
	TArray<KawaiiClothingProxy::FWeightedSnapshot> Samples;
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	FString Error;
	for (const float Weight : {1.f, 0.999f, 0.75f, 0.5f, 0.1f, 0.001f}) {
		ClimbAction.Weight = Weight;
		IdleAction.Weight = 1.f - Weight;
		TestTrue(TEXT("Unbaked idle does not cancel the still-playing climb correction"),
			Access::BuildActionSamples(Node, {ClimbAction, IdleAction}, Samples));
		TestTrue(TEXT("Correction strength follows the original full-body blend weight"),
			FMath::IsNearlyEqual(Access::CorrectionAlpha(Node), Node.Alpha * Weight));
		TestTrue(TEXT("Mapped subset remains sampleable throughout the fade"),
			KawaiiClothingProxy::SampleSnapshots(Samples, FTransform::Identity, Capsules, Error));
		if (Samples.Num() == 1 && Capsules.Num() == 1) {
			TestEqual(TEXT("Climb time is unchanged during the blend"), Samples[0].Time, 0.7f);
			TestTrue(TEXT("Fade scales influence, not collider size or location"),
				Capsules[0].Start.Equals(FVector(10, 0, 0)) && Capsules[0].Radius == 2.f);
		}
	}
	ClimbAction.Weight = 0.f;
	IdleAction.Weight = 1.f;
	TestFalse(TEXT("Completed fade returns to the input pose"), Access::BuildActionSamples(Node, {ClimbAction, IdleAction}, Samples));
	TestTrue(TEXT("Completed fade cannot reuse an old sample"), Samples.IsEmpty());
	TestEqual(TEXT("Completed fade has zero correction influence"), Access::CorrectionAlpha(Node), 0.f);
	ClimbAction.Weight = 0.3f;
	LandAction.Weight = 0.7f;
	TestTrue(TEXT("Two baked actions still blend normally"), Access::BuildActionSamples(Node, {ClimbAction, LandAction}, Samples));
	TestEqual(TEXT("Fully mapped blends retain the configured alpha"), Access::CorrectionAlpha(Node), Node.Alpha);
	TestEqual(TEXT("Both mapped motions participate"), Samples.Num(), 2);
	return true;
}
#endif
