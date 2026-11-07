#include "KawaiiPhysicsIKCorrectionMath.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiBridgeAggregationTest, "KawaiiPhysicsBaker.IKCorrection.BridgeAggregation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiBridgeAggregationTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsIKCorrection;
	const TArray<FVector> Reference{{-4,0,0},{4,0,0},{4,0,2}};
	FContactBridge A; A.A=0; A.B=1; A.Samples=3;
	A.Shapes={{FVector(0,-.5,0),FVector(0,-.5,0),1.}};
	auto One=Reference; ProjectBridges(One,Reference,{A});
	TestTrue(TEXT("Non-contacting subdivision points do not dilute the middle contact"),One[0].Equals(FVector(-4,.25,0),1.e-6));
	FContactBridge B=A; B.B=2; B.Samples=10;
	B.Shapes={{FVector(0,-.5,1),FVector(0,-.5,1),1.5}};
	auto Forward=Reference, Reverse=Reference;
	ProjectBridges(Forward,Reference,{A,B}); ProjectBridges(Reverse,Reference,{B,A});
	for (int32 I=0;I<Reference.Num();++I) { TestTrue(TEXT("Shared endpoint feedback is independent of bridge order"),Forward[I].Equals(Reverse[I],1.e-6)); }
	A.Samples=1;
	auto Two=Reference, Six=Reference;
	ProjectBridges(Two,Reference,{A,A}); ProjectBridges(Six,Reference,{A,A,A,A,A,A});
	TestTrue(TEXT("Dense contacts are averaged instead of multiplied per bridge"),Two[0].Equals(Six[0],1.e-6));
	A.FixedA=true;
	auto Fixed=Reference; ProjectBridges(Fixed,Reference,{A});
	TestTrue(TEXT("Fixed roots remain untouched"),Fixed[0].Equals(Reference[0]));
	A.FixedA=false; A.RadiusA=A.RadiusB=4.;
	auto Covered=Reference; ProjectBridges(Covered,Reference,{A});
	TestTrue(TEXT("Already-overlapping endpoint spheres need no bridge dummies"),Covered[0].Equals(Reference[0]) && Covered[1].Equals(Reference[1]));
	return true;
}
#endif
