#include "KawaiiPhysicsIKCorrectionMath.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace KawaiiPhysicsIKCorrection;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKWarpTest, "KawaiiPhysicsBaker.IKCorrection.PoseMapping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKWarpTest::RunTest(const FString&) {
	const FTransform Bone(FQuat(FVector::YAxisVector, 0.2), FVector(12, 4, -20));
	const FTransform Anchor(FQuat::Identity, FVector(2, 0, 10));
	const FTransform Driver(FQuat(FVector::XAxisVector, 0.1), FVector(5, 1, -3));
	for (const float Weight : {0.f, 0.25f, 1.f}) {
		TestTrue(TEXT("No IK change preserves the complete baked transform"), WarpTransform(Bone, Anchor, Anchor, Driver, Driver, Weight).Equals(Bone, 1.e-5));
	}

	const FVector PelvisOffset(0, 0, -8);
	FTransform MovedAnchor = Anchor;
	FTransform MovedDriver = Driver;
	MovedAnchor.AddToTranslation(PelvisOffset);
	MovedDriver.AddToTranslation(PelvisOffset);
	const FTransform Moved = WarpTransform(Bone, Anchor, MovedAnchor, Driver, MovedDriver, 0.7f);
	TestTrue(TEXT("A common pelvis offset is applied exactly once"), Moved.GetTranslation().Equals(Bone.GetTranslation() + PelvisOffset, 1.e-5));
	TestTrue(TEXT("Translation-only compensation retains baked twist"), Moved.GetRotation().Equals(Bone.GetRotation(), 1.e-5));

	const FTransform Rotation(FQuat(FVector::ZAxisVector, UE_PI / 2.0), FVector::ZeroVector);
	const FTransform Rotated = WarpTransform(Bone, Anchor, Anchor * Rotation, Driver, Driver * Rotation, 0.7f);
	TestTrue(TEXT("A common pelvis rotation is applied exactly once"), Rotated.Equals(Bone * Rotation, 1.e-5));

	const FTransform LiftedDriver(FQuat::Identity, FVector(0, 0, 20));
	const FTransform Identity = FTransform::Identity;
	TestTrue(TEXT("Unweighted leg motion leaves the anchor mapping alone"), WarpTransform(Bone, Identity, Identity, Identity, LiftedDriver, 0.f).Equals(Bone, 1.e-5));
	TestTrue(TEXT("Half influence transfers half the driver displacement"),
			 WarpTransform(Bone, Identity, Identity, Identity, LiftedDriver, 0.5f).GetTranslation().Equals(Bone.GetTranslation() + FVector(0, 0, 10), 1.e-5));
	TestTrue(TEXT("Full influence transfers the complete driver displacement"),
			 WarpTransform(Bone, Identity, Identity, Identity, LiftedDriver, 1.f).GetTranslation().Equals(Bone.GetTranslation() + FVector(0, 0, 20), 1.e-5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKPointContactTest, "KawaiiPhysicsBaker.IKCorrection.PointContactAndLength", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKPointContactTest::RunTest(const FString&) {
	TArray<FVector> Points{FVector(0, 0, 3), FVector(0.2, 0, 0.2)};
	const TArray<FVector> Reference = Points;
	const TArray<double> Lengths{FVector::Distance(Points[0], Points[1])};
	const TArray<FCapsule> Capsules{{FVector::ZeroVector, FVector::ZeroVector, 1.0}};
	const FDiagnostics Result = SolveChain(Points, Lengths, Reference, Capsules, 64, 0.1);
	TestTrue(TEXT("The solve is finite"), Result.bFinite);
	TestTrue(TEXT("The root stays at its animated attachment"), Points[0].Equals(Reference[0], 0.0));
	TestTrue(TEXT("The particle is outside the capsule including cloth thickness"), Points[1].Size() >= 1.1 - 0.001);
	TestTrue(TEXT("No residual segment or particle overlap in a feasible case"), Result.MaxPenetration < 0.001);
	TestTrue(TEXT("The feasible chain retains its baked length"), Result.MaxLengthError < 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKSegmentContactTest, "KawaiiPhysicsBaker.IKCorrection.SegmentContact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKSegmentContactTest::RunTest(const FString&) {
	// Both joints start outside the leg. A particle-only solver would miss this intersection.
	TArray<FVector> Points{FVector(-3, 0, 0), FVector(3, 0, 0)};
	const TArray<FVector> Reference = Points;
	const TArray<double> Lengths{6.0};
	const TArray<FCapsule> Capsules{{FVector(0, -2, 0), FVector(0, 2, 0), 1.0}};
	const FDiagnostics Result = SolveChain(Points, Lengths, Reference, Capsules, 64, 0.0);
	TestTrue(TEXT("Crossing segment is moved even though its endpoints were clear"), !Points[1].Equals(Reference[1], 0.001));
	TestTrue(TEXT("The anchor is not moved to resolve contact"), Points[0].Equals(Reference[0], 0.0));
	TestTrue(TEXT("The segment clears the leg capsule"), Result.MaxPenetration < 0.001);
	TestTrue(TEXT("Length projection converges without stretching the garment"), Result.MaxLengthError < 0.02);
	TestTrue(TEXT("Exact centerline crossing produces a finite deterministic result"), Result.bFinite);
	TArray<FVector> Repeated = Reference;
	SolveChain(Repeated, Lengths, Reference, Capsules, 64, 0.0);
	TestTrue(TEXT("Repeating the same pose gives the same correction"), Points[1].Equals(Repeated[1], 1.e-8));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKConflictTest, "KawaiiPhysicsBaker.IKCorrection.PinnedRootConflictAndDegeneracy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKConflictTest::RunTest(const FString&) {
	TArray<FVector> Points{FVector::ZeroVector, FVector(2, 0, 0)};
	const TArray<FVector> Reference = Points;
	const TArray<double> Lengths{2.0};
	const TArray<FCapsule> Capsules{{FVector::ZeroVector, FVector::ZeroVector, 1.0}};
	const FDiagnostics Result = SolveChain(Points, Lengths, Reference, Capsules, 64, 0.0);
	TestTrue(TEXT("An impossible root contact is reported as residual penetration"), Result.MaxPenetration >= 1.0 - 1.e-6);
	TestTrue(TEXT("The root remains pinned even when contact is impossible"), Points[0].IsZero());
	TestTrue(TEXT("An impossible constraint is not a numerical failure"), Result.bFinite);

	TArray<FVector> Degenerate{FVector::ZeroVector, FVector::ZeroVector};
	const TArray<FVector> DegenerateReference = Degenerate;
	const TArray<double> ZeroLength{0.0};
	const FDiagnostics DegenerateResult = SolveChain(Degenerate, ZeroLength, DegenerateReference, Capsules, 0, 0.0);
	TestTrue(TEXT("Zero-length bones and capsules do not produce NaN"), DegenerateResult.bFinite);
	TestTrue(TEXT("Conflicting zero-length constraint retains visible length error"), DegenerateResult.MaxLengthError > 0.0);
	TestTrue(TEXT("Degenerate root remains fixed"), Degenerate[0].IsZero());

	const TArray<FVector> BeforeInvalid = Degenerate;
	const FDiagnostics Invalid = SolveChain(Degenerate, {}, DegenerateReference, Capsules, 16, 0.0);
	TestFalse(TEXT("Malformed chain is explicitly rejected"), Invalid.bFinite);
	TestTrue(TEXT("Invalid input does not partially modify the chain"), Degenerate == BeforeInvalid);
	return true;
}

#endif
