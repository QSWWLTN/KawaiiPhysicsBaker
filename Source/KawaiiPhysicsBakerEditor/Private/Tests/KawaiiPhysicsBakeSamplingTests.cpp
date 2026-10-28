#include "KawaiiPhysicsBakeSampling.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiBakeSamplingEndpointTest, "KawaiiPhysicsBaker.Sampling.EndpointsAndQuantization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiBakeSamplingEndpointTest::RunTest(const FString& Parameters) {
	using namespace KawaiiPhysicsBakeSampling;
	TestEqual(TEXT("Integral duration uses exactly sixty intervals"), NumIntervals(1.0, 60), 60);
	TestEqual(TEXT("Float-sized roundoff does not introduce a whole extra frame"), NumIntervals(1.0 + 1.e-8, 60), 60);
	const double Length = 1.013;
	const int32 Intervals = NumIntervals(Length, 60);
	TestEqual(TEXT("Fractional duration rounds outward"), Intervals, 61);
	TestEqual(TEXT("First sample at zero"), SourceTime(0, Intervals, Length, 60), 0.f);
	TestEqual(TEXT("Last sample includes the exact sequence endpoint"), SourceTime(Intervals, Intervals, Length, 60), float(Length));
	TestTrue(TEXT("End padding is less than one sample"), double(Intervals) / 60.0 - Length < 1.0 / 60.0);
	float Previous = -1.f;
	for (int32 Index = 0; Index <= Intervals; ++Index) {
		const float Time = SourceTime(Index, Intervals, Length, 60);
		TestTrue(TEXT("Sample times increase without looping"), Time > Previous);
		TestTrue(TEXT("Sample never exceeds source duration"), Time <= float(Length));
		Previous = Time;
	}
	TestEqual(TEXT("Sub-frame source still has two keys"), NumIntervals(0.001, 60), 1);
	TestEqual(TEXT("Sub-frame last key samples actual endpoint"), SourceTime(1, 1, 0.001, 60), 0.001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiBakeTransformValidationTest, "KawaiiPhysicsBaker.Sampling.TransformValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiBakeTransformValidationTest::RunTest(const FString& Parameters) {
	using namespace KawaiiPhysicsBakeSampling;
	TestTrue(TEXT("Identity is safe"), IsValidTransform(FTransform::Identity));
	TestTrue(TEXT("Finite nonuniform scale is accepted"), IsValidTransform(FTransform(FQuat::Identity, FVector(30, 40, 50), FVector(2, 1, 3))));
	TestFalse(TEXT("Zero scale cannot be used for parent-space reconstruction"), IsValidTransform(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(1, 0, 1))));
	TestFalse(TEXT("Mirrored scale is rejected explicitly"), IsValidTransform(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(-1, 1, 1))));
	return true;
}

#endif
