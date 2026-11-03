#include "KawaiiClothingProxyPlayback.h"

#include "Animation/AnimSequence.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyActionMappingTest, "KawaiiPhysicsBaker.ClothingProxy.ActionMapping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxyActionMappingTest::RunTest(const FString&) {
	TStrongObjectPtr<UAnimSequence> Attack(NewObject<UAnimSequence>());
	TStrongObjectPtr<UAnimSequence> Landing(NewObject<UAnimSequence>());
	TStrongObjectPtr<UKawaiiClothingProxyMotion> AttackMotion(NewObject<UKawaiiClothingProxyMotion>());
	TStrongObjectPtr<UKawaiiClothingProxyMotion> LandingMotion(NewObject<UKawaiiClothingProxyMotion>());
	TStrongObjectPtr<UKawaiiClothingProxyMotionSet> Set(NewObject<UKawaiiClothingProxyMotionSet>());
	AttackMotion->SourceAnimation = Attack.Get();
	LandingMotion->SourceAnimation = Landing.Get();
	Set->Motions = {AttackMotion.Get(), LandingMotion.Get()};
	FKawaiiClothingProxyActionSample A, B;
	A.SourceAnimation = Attack.Get();
	A.Time = 0.45f;
	A.Weight = 0.7f;
	B.SourceAnimation = Landing.Get();
	B.Time = 0.1f;
	B.Weight = 0.3f;
	TArray<FKawaiiClothingProxyMotionSample> Samples;
	FString Error;
	TestTrue(TEXT("Body blend resolves to corresponding garment motions"),
			 UKawaiiClothingProxyPlaybackLibrary::ResolveProxyMotionSamples(Set.Get(), {A, B}, Samples, Error)
	);
	if (TestEqual(TEXT("Both transition clips remain present"), Samples.Num(), 2)) {
		TestTrue(TEXT("First action resolves by asset identity"), Samples[0].Motion == AttackMotion.Get());
		TestEqual(TEXT("Time is copied without an independent clock"), Samples[0].Time, A.Time);
		TestEqual(TEXT("Blend weight is retained"), Samples[1].Weight, B.Weight);
	}
	Set->Motions.Add(AttackMotion.Get());
	TestFalse(TEXT("Duplicate source mapping is rejected"), UKawaiiClothingProxyPlaybackLibrary::ResolveProxyMotionSamples(Set.Get(), {A, B}, Samples, Error));
	TestTrue(TEXT("Failed lookup cannot reuse stale capsules' source samples"), Samples.IsEmpty());
	Set->Motions = {AttackMotion.Get()};
	TestFalse(TEXT("A missing positive-weight mapping is explicit"),
			  UKawaiiClothingProxyPlaybackLibrary::ResolveProxyMotionSamples(Set.Get(), {A, B}, Samples, Error)
	);
	B.Weight = 0.f;
	TestTrue(TEXT("An inactive missing clip does not prevent sampling"),
			 UKawaiiClothingProxyPlaybackLibrary::ResolveProxyMotionSamples(Set.Get(), {A, B}, Samples, Error)
	);
	return true;
}

#endif
