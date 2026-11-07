#include "Misc/AutomationTest.h"
#include "KawaiiPhysicsIKCorrectionMath.h"

#if WITH_DEV_AUTOMATION_TESTS
using namespace KawaiiPhysicsIKCorrection;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFastContactProjectionTest,
	"KawaiiPhysicsBaker.IKCorrection.FastContact.DirectedProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiFastContactProjectionTest::RunTest(const FString&) {
	for (const double Scale : {0.1, 1., 10.}) {
		const TArray<FVector> Input{Scale * FVector(0, 0, 10), Scale * FVector(-0.99, 0, 0)};
		const TArray<FVector> Previous{Input[0], Scale * FVector(0.5, 0, 0)};
		const TArray<double> Lengths{FVector::Distance(Input[0], Input[1])};
		const TArray<FCapsule> Capsules{{Scale * FVector(0, -2, 0), Scale * FVector(0, 2, 0), Scale}};
		auto Points = Input;
		const auto Result = SolveChain(Points, Lengths, Input, Capsules, 1, 0., &Previous);
		AddInfo(FString::Printf(TEXT("Directed contact scale %.1f: x %.6f, penetration %.6f"), Scale, Points[1].X, Result.MaxPenetration));
		TestTrue(TEXT("A history-directed contact crosses the whole interior to its original side"),
			Result.bFinite && Points[1].X > 0. && Result.MaxPenetration < .02 * Scale);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFastContactSweepTest,
	"KawaiiPhysicsBaker.IKCorrection.FastContact.SweepPastCapsule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiFastContactSweepTest::RunTest(const FString&) {
	for (const FQuat Rotation : {FQuat::Identity, FQuat(FVector::YAxisVector, .8)}) {
		const auto V = [&](FVector P) { return Rotation.RotateVector(P); };
		const TArray<FVector> Input{V(FVector(0, 0, 10)), V(FVector(-4, 0, 0))};
		const TArray<FVector> Previous{Input[0], V(FVector(4, 0, 0))};
		const TArray<double> Lengths{FVector::Distance(Input[0], Input[1])};
		const TArray<FCapsule> Capsules{{V(FVector(0, -2, 0)), V(FVector(0, 2, 0)), 1.}};
		auto Points = Input;
		const auto Result = SolveChain(Points, Lengths, Input, Capsules, 16, 0., &Previous);
		TestTrue(TEXT("Both endpoints being outside must not hide a crossing between frames"),
			Result.bFinite && FVector::DotProduct(Points[1], V(FVector::XAxisVector)) > .9);
		TestTrue(TEXT("The pinned attachment remains fixed during sweep correction"), Points[0].Equals(Input[0]));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiMovingCapsuleSweepTest,
	"KawaiiPhysicsBaker.IKCorrection.FastContact.MovingCapsule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiMovingCapsuleSweepTest::RunTest(const FString&) {
	const TArray<FVector> Input{FVector(0, 0, 10), FVector::ZeroVector};
	const TArray<double> Lengths{10.};
	const TArray<FCapsule> Before{{FVector(-4, -2, 0), FVector(-4, 2, 0), 1.}};
	const TArray<FCapsule> Now{{FVector(4, -2, 0), FVector(4, 2, 0), 1.}};
	auto Points = Input;
	const auto Result = SolveChain(Points, Lengths, Input, Now, 32, 0., &Input, &Before);
	TestTrue(TEXT("A moving capsule cannot pass through a stationary free joint between samples"), Result.bFinite && Points[1].X > 4.);
	TestTrue(TEXT("Moving proxy correction retains the pinned attachment"), Points[0].Equals(Input[0]));
	TestTrue(TEXT("A feasible moving-proxy contact clears without stretching the bone"), Result.MaxPenetration < .02 && Result.MaxLengthError < .05);

	// A large displacement alone is not a collision: the trajectory passes above the proxy.
	const TArray<FVector> NearMiss{FVector(0, 0, 10), FVector(-4, 0, 3)};
	const TArray<FVector> Previous{NearMiss[0], FVector(4, 0, 3)};
	const TArray<double> NearMissLengths{FVector::Distance(NearMiss[0], NearMiss[1])};
	const TArray<FCapsule> Stationary{{FVector(0, -2, 0), FVector(0, 2, 0), 1.}};
	Points = NearMiss;
	SolveChain(Points, NearMissLengths, NearMiss, Stationary, 16, 0., &Previous);
	TestTrue(TEXT("A non-intersecting fast trajectory is preserved"), Points[1].Equals(NearMiss[1], 1.e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiBoxContactTest,
	"KawaiiPhysicsBaker.IKCorrection.FastContact.BoxInteriorAndSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiBoxContactTest::RunTest(const FString&) {
	for (const FQuat Q : {FQuat::Identity, FRotator(20, 35, 15).Quaternion()}) {
		FCapsule Box; Box.Start = Box.End = FVector(4, 7, 2); Box.Radius = 1.; Box.BoxExtent = FVector(1, 3, 2); Box.BoxRotation = Q;
		const auto V = [&](FVector P) { return Box.Start + Q.RotateVector(P); };
		for (const FVector Local : {FVector::ZeroVector, FVector(.5, 0, 0), FVector(1.1, 3.1, 0)}) {
			const FVector P = V(Local), Push = BoxPush(P, Box, .5);
			TestTrue(TEXT("An embedded sphere exits the complete box, including its particle radius"), SurfaceDistance(P + Push, Box) >= .5 - 1.e-6);
		}
		const TArray<FVector> Input{V(FVector(0, 0, 10)), V(FVector(-4, 0, 0))};
		const TArray<FVector> Previous{Input[0], V(FVector(4, 0, 0))};
		TArray<FVector> Points = Input;
		const auto R = SolveChain(Points, {FVector::Distance(Input[0], Input[1])}, Input, {Box}, 16, .25, &Previous);
		TestTrue(TEXT("Box sweep preserves the entry side across a full crossing"), R.bFinite && Q.UnrotateVector(Points[1] - Box.Start).X >= 1.24);
		TestTrue(TEXT("Box sweep pins the root"), Points[0].Equals(Input[0]));
	}
	const TArray<FVector> Input{FVector(0,0,10), FVector(2,0,0)};
	const TArray<FCapsule> Sphere{{FVector::ZeroVector, FVector::ZeroVector, 1.}};
	const TArray<double> Radii{0., .1};
	auto Points = Input;
	SolveChain(Points, {FVector::Distance(Input[0],Input[1])}, Input, Sphere, 16, 3., nullptr, nullptr, &Radii);
	TestTrue(TEXT("Local radii keep a valid narrow strand instead of expanding it to the chain maximum"), Points[1].Equals(Input[1], 1.e-6));
	return true;
}
#endif
