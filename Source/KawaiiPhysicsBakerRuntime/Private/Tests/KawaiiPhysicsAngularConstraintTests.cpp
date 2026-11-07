#include "KawaiiPhysicsIKCorrectionMath.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiAngularConstraintTest, "KawaiiPhysicsBaker.IKCorrection.AngularConstraints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiAngularConstraintTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsIKCorrection;
	FAngularConstraints C;
	C.Pose = {{0,0,0},{5,0,0}}; C.LimitRadians = {0., FMath::DegreesToRadians(30.)};
	TArray<FVector> P{{0,0,0},{-5,0,0}};
	auto D = SolveChain(P, {5.}, C.Pose, {}, 64, 0., nullptr, nullptr, nullptr, &C);
	TestTrue(TEXT("Antiparallel input has a finite deterministic cone projection"), D.bFinite && D.MaxAngleViolationDegrees < .01);
	TestTrue(TEXT("The attachment and bone length are preserved"), P[0].IsZero() && FMath::IsNearlyEqual(P[1].Size(), 5., .001));
	C.Pose = {{0,0,0},{5,0,0},{10,0,0},{15,0,0}};
	C.LimitRadians.Init(0.,4); C.MaxBendChangeRadians = FMath::DegreesToRadians(20.);
	P = {{0,0,0},{5,0,0},{5,5,0},{5,10,0}};
	D = SolveChain(P, {5.,5.,5.}, C.Pose, {}, 64, 0., nullptr, nullptr, nullptr, &C);
	AddInfo(FString::Printf(TEXT("Bend residual %.6f deg; length residual %.6f cm"),D.MaxBendViolationDegrees,D.MaxLengthError));
	TestTrue(TEXT("A sharp joint bend is distributed along the chain"), D.bFinite && D.MaxBendViolationDegrees < .2 && D.MaxLengthError < .02);
	C.Pose = {{0,0,0},{5,0,0},{8,4,0},{8,9,0}}; P = C.Pose;
	D = SolveChain(P, {5.,5.,5.}, C.Pose, {}, 64, 0., nullptr, nullptr, nullptr, &C);
	for (int32 I=0; I<P.Num(); ++I) { TestTrue(TEXT("Existing animation curvature is not straightened"), P[I].Equals(C.Pose[I],1.e-5)); }
	C.MaxBendChangeRadians = 0.;
	P = {{0,0,0},{5,0,0},{5,5,0},{5,10,0}}; auto Legacy = P;
	SolveChain(Legacy,{5.,5.,5.},C.Pose,{},16,0.);
	SolveChain(P,{5.,5.,5.},C.Pose,{},16,0.,nullptr,nullptr,nullptr,&C);
	for (int32 I=0; I<P.Num(); ++I) { TestTrue(TEXT("Zero settings preserve legacy behavior"), P[I].Equals(Legacy[I],1.e-8)); }
	C.LimitRadians.Pop(); const auto Before = P;
	TestFalse(TEXT("Malformed angle data is rejected"),SolveChain(P,{5.,5.,5.},C.Pose,{},16,0.,nullptr,nullptr,nullptr,&C).bFinite);
	TestTrue(TEXT("Rejected data leaves the input intact"),P == Before);
	C.Pose = {{0,0,0},{0,0,0},{5,0,0}}; C.LimitRadians.Init(0.,3); C.MaxBendChangeRadians = FMath::DegreesToRadians(20.);
	P = {{0,0,0},{5,0,0},{5,5,0}}; const auto Degenerate = P;
	SolveChain(P,{5.,5.},C.Pose,{},16,0.,nullptr,nullptr,nullptr,&C);
	for (int32 I=0; I<P.Num(); ++I) { TestTrue(TEXT("A zero-length reference segment does not invent an arbitrary bend"),P[I].Equals(Degenerate[I],1.e-6)); }
	return true;
}
#endif
