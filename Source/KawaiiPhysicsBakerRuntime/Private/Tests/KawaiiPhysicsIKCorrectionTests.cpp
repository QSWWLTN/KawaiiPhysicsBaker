#include "Misc/AutomationTest.h"

#include "KawaiiPhysicsIKCorrectionMath.h"
#include "KawaiiClothingProxyMotion.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace KawaiiPhysicsIKCorrection;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKProxyContactTest,
	"KawaiiPhysicsBaker.IKCorrection.ExternalProxyContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiIKProxyContactTest::RunTest(const FString&) {
	FKawaiiClothingProxyCapsule Proxy;
	Proxy.Id = TEXT("Collar");
	Proxy.Start = Proxy.End = FVector::ZeroVector;
	Proxy.Radius = 1.f;
	TArray<FCapsule> Capsules;
	TestTrue(TEXT("Current-frame custom proxy values are accepted"), AppendProxyCapsules({Proxy}, Capsules));
	TArray<FVector> Points{FVector(0, 0, 3), FVector(0.2, 0, 0.2)};
	const TArray<FVector> Reference = Points;
	const TArray<double> Lengths{FVector::Distance(Points[0], Points[1])};
	const FDiagnostics Result = SolveChain(Points, Lengths, Reference, Capsules, 64, 0.1);
	TestTrue(TEXT("Baked hair segment clears custom garment proxy"), Result.bFinite && Result.MaxPenetration < 0.001);
	TestTrue(TEXT("Contact does not move the hair root"), Points[0].Equals(Reference[0], 0.0));
	FKawaiiClothingProxyCapsule Invalid = Proxy;
	Invalid.Radius = -1.f;
	TestFalse(TEXT("Invalid batches are rejected"), AppendProxyCapsules({Proxy, Invalid}, Capsules));
	TestEqual(TEXT("A rejected batch cannot partially append colliders"), Capsules.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKProxyRootFilterTest,
	"KawaiiPhysicsBaker.IKCorrection.ProxyRootFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiIKProxyRootFilterTest::RunTest(const FString&) {
	FKawaiiClothingProxyCapsule Coat;
	Coat.Id = TEXT("CoatCollar");
	Coat.Start = Coat.End = FVector::ZeroVector;
	Coat.Radius = 1.f;
	Coat.IgnoredTargetRootBones = {TEXT("Tail_Root"), TEXT("Tail_Root")};
	FKawaiiClothingProxyCapsule Cape = Coat;
	Cape.Id = TEXT("Cape");
	Cape.Start = Cape.End = FVector(5, 0, 0);
	Cape.IgnoredTargetRootBones = {TEXT("Hair_back")};
	const FCapsule Body{FVector(100, 0, 0), FVector(100, 0, 5), 2.0};
	TArray<FCapsule> TailCapsules{Body};
	TArray<FCapsule> HairCapsules{Body};
	TestTrue(TEXT("Each garment's rules are evaluated for the requested tail root"), AppendProxyCapsules({Coat, Cape}, TailCapsules, TEXT("Tail_Root")));
	TestTrue(TEXT("The same proxies can be evaluated independently for hair"), AppendProxyCapsules({Coat, Cape}, HairCapsules, TEXT("Hair_back")));
	if (TestEqual(TEXT("The tail keeps body collision and the other garment's proxy"), TailCapsules.Num(), 2)) {
		TestTrue(
			TEXT("Ignoring a garment never removes body collision"),
			TailCapsules[0].Start.Equals(Body.Start) && TailCapsules[0].End.Equals(Body.End) && TailCapsules[0].Radius == Body.Radius
		);
		TestTrue(TEXT("A different garment can still collide with the tail"), TailCapsules[1].Start.Equals(Cape.Start));
	}
	if (TestEqual(TEXT("Hair retains the coat proxy but ignores the cape proxy"), HairCapsules.Num(), 2)) {
		TestTrue(TEXT("Only the proxy carrying the matching ignore rule is filtered"), HairCapsules[1].Start.Equals(Coat.Start));
	}
	TArray<FCapsule> Unfiltered;
	TestTrue(TEXT("Omitting the root preserves the unfiltered helper contract"), AppendProxyCapsules({Coat, Cape}, Unfiltered));
	TestEqual(TEXT("None selects every valid proxy"), Unfiltered.Num(), 2);

	TailCapsules.Reset();
	HairCapsules.Reset();
	AppendProxyCapsules({Coat}, TailCapsules, TEXT("Tail_Root"));
	AppendProxyCapsules({Coat}, HairCapsules, TEXT("Hair_back"));
	const TArray<FVector> Reference{FVector(0, 0, 3), FVector(0.2, 0, 0.2)};
	const TArray<double> Lengths{FVector::Distance(Reference[0], Reference[1])};
	TArray<FVector> TailPoints = Reference;
	TArray<FVector> HairPoints = Reference;
	const FDiagnostics TailResult = SolveChain(TailPoints, Lengths, Reference, TailCapsules, 64, 0.1);
	const FDiagnostics HairResult = SolveChain(HairPoints, Lengths, Reference, HairCapsules, 64, 0.1);
	TestTrue(
		TEXT("The ignored garment cannot displace the tail or report residual contact"),
		TailResult.bFinite && TailPoints[1].Equals(Reference[1], 1.e-8) && TailResult.MaxPenetration == 0.0
	);
	TestTrue(
		TEXT("Hair still corrects against the same garment"),
		HairResult.bFinite && !HairPoints[1].Equals(Reference[1], 0.001) && HairResult.MaxPenetration < 0.001
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKProxyFilterValidationTest,
	"KawaiiPhysicsBaker.IKCorrection.ProxyFilterAtomicValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiIKProxyFilterValidationTest::RunTest(const FString&) {
	FKawaiiClothingProxyCapsule Valid;
	Valid.Id = TEXT("Collar");
	Valid.Radius = 1.f;
	const FCapsule Body{FVector(10, 0, 0), FVector(10, 0, 3), 2.0};
	TArray<FCapsule> Capsules{Body};
	const auto IsBodyUnchanged = [&Capsules, &Body] {
		return Capsules.Num() == 1 && Capsules[0].Start.Equals(Body.Start) && Capsules[0].End.Equals(Body.End) && Capsules[0].Radius == Body.Radius;
	};
	FKawaiiClothingProxyCapsule Invalid = Valid;
	Invalid.Radius = -1.f;
	Invalid.IgnoredTargetRootBones = {TEXT("Tail_Root")};
	TestFalse(TEXT("Ignoring a proxy cannot hide its invalid geometry"), AppendProxyCapsules({Valid, Invalid}, Capsules, TEXT("Tail_Root")));
	TestTrue(TEXT("A bad filtered proxy prevents partial addition of earlier valid proxies"), IsBodyUnchanged());
	Invalid = Valid;
	Invalid.IgnoredTargetRootBones = {NAME_None};
	TestFalse(TEXT("None is invalid inside a proxy ignore list"), AppendProxyCapsules({Valid, Invalid}, Capsules, TEXT("Hair_back")));
	TestTrue(TEXT("Malformed ignore metadata leaves existing body collision untouched"), IsBodyUnchanged());
	Invalid.IgnoredTargetRootBones.Init(TEXT("Tail_Root"), 65);
	TestFalse(TEXT("Oversized ignore metadata is rejected even when the root is ignored"), AppendProxyCapsules({Valid, Invalid}, Capsules, TEXT("Tail_Root")));
	TestTrue(TEXT("Oversized ignore metadata cannot partially append colliders"), IsBodyUnchanged());
	Valid.IgnoredTargetRootBones.Init(TEXT("Tail_Root"), 64);
	TestTrue(TEXT("The 64-name bound accepts duplicate nonempty root names"), AppendProxyCapsules({Valid}, Capsules, TEXT("Tail_Root")));
	TestTrue(TEXT("A fully ignored valid batch leaves body collision in place"), IsBodyUnchanged());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKWarpTest,
	"KawaiiPhysicsBaker.IKCorrection.PoseMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

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
	TestTrue(
		TEXT("Unweighted leg motion leaves the anchor mapping alone"),
		WarpTransform(Bone, Identity, Identity, Identity, LiftedDriver, 0.f).Equals(Bone, 1.e-5)
	);
	TestTrue(
		TEXT("Half influence transfers half the driver displacement"),
		WarpTransform(Bone, Identity, Identity, Identity, LiftedDriver, 0.5f).GetTranslation().Equals(Bone.GetTranslation() + FVector(0, 0, 10), 1.e-5)
	);
	TestTrue(
		TEXT("Full influence transfers the complete driver displacement"),
		WarpTransform(Bone, Identity, Identity, Identity, LiftedDriver, 1.f).GetTranslation().Equals(Bone.GetTranslation() + FVector(0, 0, 20), 1.e-5)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKPointContactTest,
	"KawaiiPhysicsBaker.IKCorrection.PointContactAndLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKSegmentContactTest,
	"KawaiiPhysicsBaker.IKCorrection.SegmentContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKConflictTest,
	"KawaiiPhysicsBaker.IKCorrection.PinnedRootConflictAndDegeneracy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKEmbeddedAttachmentTest,
	"KawaiiPhysicsBaker.IKCorrection.EmbeddedAttachment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiIKEmbeddedAttachmentTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsIKCorrection;
	const TArray<FVector> Input{{0, 0, 1}, {0, 0, -10}, {0, 0, -20}};
	const TArray<double> Lengths{11., 10.};
	const TArray<FCapsule> Capsules{{FVector::ZeroVector, FVector::ZeroVector, 2.}};
	TArray<FVector> Points = Input;
	const auto Result = SolveChain(Points, Lengths, Input, Capsules, 32, 0.);
	TestTrue(TEXT("Embedded root is still pinned"), Points[0].Equals(Input[0]));
	TestTrue(TEXT("An impossible first-edge contact cannot rotate a clear strand"), Points[1].Equals(Input[1], 1.e-6) && Points[2].Equals(Input[2], 1.e-6));
	TestTrue(TEXT("Unavoidable attachment overlap remains visible in diagnostics"), Result.MaxPenetration >= 1.);
	// The exemption is only for the first edge. A later edge returning through the
	// capsule must still be moved, even though both of its endpoints start outside.
	const TArray<FVector> Returning{{0, 0, 1}, {0, 0, -10}, {0, 0, 10}};
	Points = Returning;
	SolveChain(Points, {11., 20.}, Returning, Capsules, 32, 0.);
	TestFalse(TEXT("Later crossing segment still collides"), Points[2].Equals(Returning[2], 1.e-4));
	return true;
}

#endif
