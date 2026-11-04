#include "KawaiiClothingProxySnapshot.h"

#include "Async/Async.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace KawaiiProxySnapshotTests {

	UKawaiiClothingProxyMotion* MakeMotion() {
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>();
		UKawaiiClothingProxyAsset* Definition = NewObject<UKawaiiClothingProxyAsset>();
		Definition->PreviewMesh = Mesh;
		FKawaiiClothingProxyShape Shape;
		Shape.Id = TEXT("Collar");
		Shape.StartBone = TEXT("root");
		Shape.EndBone = TEXT("collar");
		Shape.StartOffset = FVector(1, 2, 0);
		Shape.EndOffset = FVector(0, 0, 2);
		Shape.Radius = 2.f;
		Definition->Shapes.Add(Shape);
		Definition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
		UKawaiiClothingProxyMotion* Motion = NewObject<UKawaiiClothingProxyMotion>();
		Motion->Definition = Definition;
		Motion->DefinitionSignature = Definition->ComputeSignature();
		Motion->SourceMesh = Mesh;
		Motion->Shapes = Definition->Shapes;
		Motion->SampleRate = 60;
		Motion->NumFrames = 3;
		Motion->SourceDuration = 0.03f;
		Motion->BoneNames = {TEXT("root"), TEXT("collar")};
		Motion->ParentIndices = {INDEX_NONE, 0};
		Motion->LocalKeys = {
			FTransform::Identity, FTransform(FVector(10, 0, 0)),
			FTransform(FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(60.0))), FTransform(FVector(10, 0, 2)),
			FTransform(FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(120.0))), FTransform(FVector(10, 0, 4))
		};
		return Motion;
	}

	FKawaiiClothingProxyMotionSample AssetSample(UKawaiiClothingProxyMotion* Motion, float Time, float Weight) {
		FKawaiiClothingProxyMotionSample Sample;
		Sample.Motion = Motion;
		Sample.Time = Time;
		Sample.Weight = Weight;
		return Sample;
	}

	bool EqualCapsules(const TArray<FKawaiiClothingProxyCapsule>& A, const TArray<FKawaiiClothingProxyCapsule>& B) {
		if (A.Num() != B.Num()) {
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index) {
			if (A[Index].Id != B[Index].Id || !A[Index].Start.Equals(B[Index].Start, 1.e-5)
				|| !A[Index].End.Equals(B[Index].End, 1.e-5) || !FMath::IsNearlyEqual(A[Index].Radius, B[Index].Radius, 1.e-5f)
				|| A[Index].IgnoredTargetRootBones != B[Index].IgnoredTargetRootBones)
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySnapshotParityTest, "KawaiiPhysicsBaker.ClothingProxy.Snapshot.AssetSamplerParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxySnapshotParityTest::RunTest(const FString&) {
	using namespace KawaiiClothingProxy;
	using namespace KawaiiProxySnapshotTests;
	TStrongObjectPtr<UKawaiiClothingProxyMotion> A(MakeMotion());
	TStrongObjectPtr<UKawaiiClothingProxyMotion> B(DuplicateObject<UKawaiiClothingProxyMotion>(A.Get(), GetTransientPackage()));
	for (FTransform& Key : B->LocalKeys) {
		Key.SetRotation(Key.GetRotation() * -1.0);
	}
	B->LocalKeys[1].SetTranslation(FVector(8, 3, 2));
	B->LocalKeys[3].SetTranslation(FVector(7, 5, 1));
	A->RelativeTransform = B->RelativeTransform = FTransform(FVector(100, 0, 0));
	FSnapshot SA, SB;
	FString Error;
	if (!TestTrue(TEXT("First complete asset snapshots"), BuildSnapshot(*A.Get(), SA, Error))
		|| !TestTrue(TEXT("Second complete asset snapshots"), BuildSnapshot(*B.Get(), SB, Error)))
	{
		AddError(Error);
		return false;
	}
	const FTransform ToTarget(FQuat(FVector::YAxisVector, 0.3), FVector(5, 2, 0), FVector(2, 1, 3));
	for (float Time : {-1.f, 0.f, 0.007f, 0.025f, 0.03f, 2.f}) {
		TArray<FKawaiiClothingProxyCapsule> Expected, Actual;
		const float OtherTime = Time + 0.002f;
		TestTrue(TEXT("Original game-thread sampler succeeds"),
			SampleBlended({AssetSample(A.Get(), Time, 2.f), AssetSample(B.Get(), OtherTime, 3.f)}, ToTarget, Expected, Error));
		TestTrue(TEXT("Snapshot sampler succeeds"),
			SampleSnapshots({{&SA, Time, 2.f}, {&SB, OtherTime, 3.f}}, ToTarget, Actual, Error));
		TestTrue(FString::Printf(TEXT("Snapshot agrees with original sampler at %.4f seconds"), Time), EqualCapsules(Expected, Actual));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySnapshotWorkerTest, "KawaiiPhysicsBaker.ClothingProxy.Snapshot.WorkerIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxySnapshotWorkerTest::RunTest(const FString&) {
	using namespace KawaiiClothingProxy;
	using namespace KawaiiProxySnapshotTests;
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Motion(MakeMotion());
	FSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("Build the immutable worker payload on the game thread"), BuildSnapshot(*Motion.Get(), Snapshot, Error))) {
		AddError(Error);
		return false;
	}
	TArray<FKawaiiClothingProxyCapsule> Expected;
	TestTrue(TEXT("Capture expected result before asset changes"),
		SampleBlended({AssetSample(Motion.Get(), 0.012f, 1.f)}, FTransform::Identity, Expected, Error));
	Motion->LocalKeys.Reset();
	Motion->Definition->Shapes[0].Radius = 500.f;
	Motion->Definition->IgnoredTargetRootBones = {TEXT("ChangedAfterSnapshot")};
	struct FWorkerResult {
		bool bWorker = false;
		bool bSampled = false;
		TArray<FKawaiiClothingProxyCapsule> Capsules;
		FString Error;
	};
	FWorkerResult Result = Async(EAsyncExecution::ThreadPool, [Data = MoveTemp(Snapshot)]() {
		FWorkerResult Worker;
		Worker.bWorker = !IsInGameThread();
		Worker.bSampled = SampleSnapshots({{&Data, 0.012f, 1.f}}, FTransform::Identity, Worker.Capsules, Worker.Error);
		return Worker;
	}).Get();
	TestTrue(TEXT("Sampling actually ran outside the game thread"), Result.bWorker);
	TestTrue(TEXT("Workers do not call the game-thread-only asset sampler"), Result.bSampled);
	TestTrue(TEXT("Changes to live tracks, geometry and rules cannot mutate an existing snapshot"), EqualCapsules(Expected, Result.Capsules));
	FSnapshot Rejected;
	Rejected.LocalKeys.Add(FTransform::Identity);
	TestFalse(TEXT("Rebuilding detects invalidated source assets"), BuildSnapshot(*Motion.Get(), Rejected, Error));
	TestTrue(TEXT("Failed rebuild clears its prior payload"), Rejected.LocalKeys.IsEmpty() && Rejected.Shapes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySnapshotRulesTest, "KawaiiPhysicsBaker.ClothingProxy.Snapshot.RulesAndIdentities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxySnapshotRulesTest::RunTest(const FString&) {
	using namespace KawaiiClothingProxy;
	using namespace KawaiiProxySnapshotTests;
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Motion(MakeMotion());
	Motion->SourceAnimation = FSoftObjectPath(TEXT("/Game/Actions/Walk.Walk"));
	Motion->BakedAnimation = FSoftObjectPath(TEXT("/Game/Bakes/Walk_Coat.Walk_Coat"));
	Motion->BodyBakedAnimation = FSoftObjectPath(TEXT("/Game/Bakes/Walk_Main.Walk_Main"));
	FSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("Snapshot soft identities without loading sequences"), BuildSnapshot(*Motion.Get(), Snapshot, Error))) {
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Original action matches by recorded identity"), Snapshot.MatchesAnimation(Motion->SourceAnimation.ToSoftObjectPath()));
	TestTrue(TEXT("Garment baked action matches by recorded identity"), Snapshot.MatchesAnimation(Motion->BakedAnimation.ToSoftObjectPath()));
	TestTrue(TEXT("Explicitly recorded Main baked action matches"), Snapshot.MatchesAnimation(Motion->BodyBakedAnimation.ToSoftObjectPath()));
	TestFalse(TEXT("Other similarly named baked actions never match"), Snapshot.MatchesAnimation(FSoftObjectPath(TEXT("/Game/Other/Walk_Main.Walk_Main"))));
	TestFalse(TEXT("Null action never matches absent aliases"), Snapshot.MatchesAnimation(FSoftObjectPath()));
	const FTransform* const TrackAddress = Snapshot.LocalKeys.GetData();
	TArray<FName> CurrentRules = {TEXT("Hair_back")};
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	TestTrue(TEXT("Captured rules can update without copying the immutable tracks"),
		SampleSnapshots({{&Snapshot, 0.f, 1.f, &CurrentRules}}, FTransform::Identity, Capsules, Error));
	TestTrue(TEXT("Sampling keeps track storage intact"), TrackAddress == Snapshot.LocalKeys.GetData());
	if (TestEqual(TEXT("One proxy emitted"), Capsules.Num(), 1)) {
		TestTrue(TEXT("Rule override reaches collision consumers"), Capsules[0].IgnoredTargetRootBones == CurrentRules);
		const TArray<FName> EmittedRules = Capsules[0].IgnoredTargetRootBones;
		CurrentRules = {TEXT("NewRule")};
		TestTrue(TEXT("Emitted rules are owned values"), Capsules[0].IgnoredTargetRootBones == EmittedRules);
	}
	CurrentRules = {NAME_None};
	TestFalse(TEXT("Invalid live rules reject the frame"),
		SampleSnapshots({{&Snapshot, 0.f, 1.f, &CurrentRules}}, FTransform::Identity, Capsules, Error));
	TestTrue(TEXT("Invalid rules cannot reuse prior collision"), Capsules.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySnapshotFailureTest, "KawaiiPhysicsBaker.ClothingProxy.Snapshot.FailureIsAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxySnapshotFailureTest::RunTest(const FString&) {
	using namespace KawaiiClothingProxy;
	using namespace KawaiiProxySnapshotTests;
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Motion(MakeMotion());
	FSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("Valid fixture snapshots"), BuildSnapshot(*Motion.Get(), Snapshot, Error))) {
		AddError(Error);
		return false;
	}
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	TestTrue(TEXT("Zero-weight missing snapshots are ignored"),
		SampleSnapshots({{nullptr, 0.f, 0.f}, {&Snapshot, 0.f, 1.f}}, FTransform::Identity, Capsules, Error));
	FSnapshot Other = Snapshot;
	Other.DefinitionIdentity = FSoftObjectPath(TEXT("/Game/Other/Proxy.Proxy"));
	TestFalse(TEXT("Different garments cannot be mixed as one pose"),
		SampleSnapshots({{&Snapshot, 0.f, 1.f}, {&Other, 0.f, 1.f}}, FTransform::Identity, Capsules, Error));
	TestTrue(TEXT("Incompatible second entry clears all output"), Capsules.IsEmpty());
	Capsules.AddDefaulted();
	TestFalse(TEXT("All-zero weights reject the frame"), SampleSnapshots({{&Snapshot, 0.f, 0.f}}, FTransform::Identity, Capsules, Error));
	TestTrue(TEXT("All-zero frame clears stale collision"), Capsules.IsEmpty());
	Capsules.AddDefaulted();
	TestFalse(TEXT("NaN time is rejected"),
		SampleSnapshots({{&Snapshot, std::numeric_limits<float>::quiet_NaN(), 1.f}}, FTransform::Identity, Capsules, Error));
	TestTrue(TEXT("NaN frame clears stale collision"), Capsules.IsEmpty());
	Other = Snapshot;
	Other.LocalKeys.Pop();
	TestFalse(TEXT("Truncated snapshot fails before indexing its tracks"),
		SampleSnapshots({{&Other, 0.f, 1.f}}, FTransform::Identity, Capsules, Error));
	Other = Snapshot;
	Other.ParentIndices[1] = 1;
	TestFalse(TEXT("Self-parent fails before reconstructing hierarchy"),
		SampleSnapshots({{&Other, 0.f, 1.f}}, FTransform::Identity, Capsules, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySnapshotReanchorTest, "KawaiiPhysicsBaker.ClothingProxy.Snapshot.ReanchorToLiveRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxySnapshotReanchorTest::RunTest(const FString&) {
	using namespace KawaiiClothingProxy;
	using namespace KawaiiProxySnapshotTests;
	// Baked tracks keep the action's root motion. The locked copy is what the body shows when root motion is extracted.
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Moving(MakeMotion());
	for (int32 Frame = 0; Frame < Moving->NumFrames; ++Frame) {
		Moving->LocalKeys[Frame * 2].SetTranslation(FVector(40.0 * Frame, 15.0 * Frame, 3.0 * Frame));
	}
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Locked(DuplicateObject<UKawaiiClothingProxyMotion>(Moving.Get(), GetTransientPackage()));
	const FTransform LockedRoot(FQuat(FVector::ZAxisVector, 0.4), FVector(2, -3, 0));
	for (int32 Frame = 0; Frame < Locked->NumFrames; ++Frame) {
		Locked->LocalKeys[Frame * 2] = LockedRoot;
	}
	FSnapshot MovingSnapshot, LockedSnapshot;
	FString Error;
	if (!TestTrue(TEXT("Moving fixture snapshots"), BuildSnapshot(*Moving.Get(), MovingSnapshot, Error))
		|| !TestTrue(TEXT("Locked fixture snapshots"), BuildSnapshot(*Locked.Get(), LockedSnapshot, Error)))
	{
		AddError(Error);
		return false;
	}
	// A non-identity garment placement must survive: the node maps both roots through the same space.
	const FTransform Space(FQuat(FVector::YAxisVector, 0.2), FVector(5, 0, 7));
	for (float Time : {0.f, 0.012f, 0.03f}) {
		TArray<FKawaiiClothingProxyCapsule> Recorded, Expected;
		FTransform RecordedRoot;
		TestTrue(TEXT("Recorded proxies sample"), SampleSnapshots({{&MovingSnapshot, Time, 1.f}}, Space, Recorded, Error, &RecordedRoot));
		TestTrue(TEXT("Locked proxies sample"), SampleSnapshots({{&LockedSnapshot, Time, 1.f}}, Space, Expected, Error));
		const TArray<FKawaiiClothingProxyCapsule> Unchanged = Recorded;
		ReanchorCapsules(Recorded, RecordedRoot, RecordedRoot);
		TestTrue(TEXT("Reanchoring to the recorded root changes nothing"), EqualCapsules(Unchanged, Recorded));
		ReanchorCapsules(Recorded, RecordedRoot, LockedRoot * Space);
		TestTrue(FString::Printf(TEXT("Reanchored proxies follow the locked root at %.3f seconds"), Time), EqualCapsules(Expected, Recorded));
	}
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	FTransform RecordedRoot;
	TestTrue(TEXT("Last frame samples"), SampleSnapshots({{&MovingSnapshot, 1.f, 1.f}}, FTransform::Identity, Capsules, Error, &RecordedRoot));
	TestTrue(TEXT("The recorded root carries the baked root motion"), RecordedRoot.GetTranslation().Equals(FVector(80, 30, 6), 1.e-5));
	TestFalse(TEXT("A failed sample reports no recorded root"), SampleSnapshots({{&MovingSnapshot, 0.f, 0.f}}, FTransform::Identity, Capsules, Error, &RecordedRoot));
	TestTrue(TEXT("The failed sample resets the recorded root"), RecordedRoot.Equals(FTransform::Identity));
	return true;
}

#endif
