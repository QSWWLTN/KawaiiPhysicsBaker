#include "KawaiiClothingProxyMotion.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace KawaiiProxyTests {
	UKawaiiClothingProxyMotion* MakeMotion() {
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>();
		UKawaiiClothingProxyAsset* Definition = NewObject<UKawaiiClothingProxyAsset>();
		Definition->PreviewMesh = Mesh;
		FKawaiiClothingProxyShape Shape;
		Shape.Id = TEXT("Collar");
		Shape.StartBone = TEXT("collar");
		Shape.Radius = 1.f;
		Definition->Shapes.Add(Shape);
		UKawaiiClothingProxyMotion* Motion = NewObject<UKawaiiClothingProxyMotion>();
		Motion->Definition = Definition;
		Motion->DefinitionSignature = Definition->ComputeSignature();
		Motion->SourceMesh = Mesh;
		Motion->Shapes = Definition->Shapes;
		Motion->SampleRate = 60;
		Motion->NumFrames = 2;
		Motion->SourceDuration = 1.f / 60.f;
		Motion->BoneNames = {TEXT("root"), TEXT("collar")};
		Motion->ParentIndices = {INDEX_NONE, 0};
		Motion->LocalKeys = {FTransform::Identity, FTransform(FVector(10, 0, 0)), FTransform::Identity, FTransform(FVector(10, 0, 0))};
		return Motion;
	}

	FKawaiiClothingProxyMotionSample Sample(UKawaiiClothingProxyMotion* Motion, float Time = 0.f, float Weight = 1.f) {
		FKawaiiClothingProxyMotionSample Result;
		Result.Motion = Motion;
		Result.Time = Time;
		Result.Weight = Weight;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyHierarchyTest, "KawaiiPhysicsBaker.ClothingProxy.MinimalHierarchy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxyHierarchyTest::RunTest(const FString&) {
	FReferenceSkeleton Skeleton;
	{
		FReferenceSkeletonModifier Modifier(Skeleton, nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("spine"), TEXT("spine"), 0), FTransform(FVector(0, 0, 10)));
		Modifier.Add(FMeshBoneInfo(TEXT("collar"), TEXT("collar"), 1), FTransform(FVector(0, 0, 5)));
		Modifier.Add(FMeshBoneInfo(TEXT("unrelated"), TEXT("unrelated"), 0), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("coat_tip"), TEXT("coat_tip"), 1), FTransform(FVector(0, -10, 0)));
	}
	FKawaiiClothingProxyShape Shape;
	Shape.Id = TEXT("Back");
	Shape.StartBone = TEXT("collar");
	Shape.EndBone = TEXT("coat_tip");
	TArray<FName> Names;
	TArray<int32> Parents, MeshIndices;
	FString Error;
	TestTrue(TEXT("Both endpoint chains can be reduced"), KawaiiClothingProxy::BuildMinimalHierarchy(Skeleton, {Shape}, Names, Parents, MeshIndices, Error));
	TestTrue(TEXT("Ancestors retained and unrelated branch omitted"), MeshIndices == TArray<int32>({0, 1, 2, 4}));
	TestTrue(TEXT("Parent indices remapped into reduced hierarchy"), Parents == TArray<int32>({INDEX_NONE, 0, 1, 1}));
	const FString Before = KawaiiClothingProxy::ComputeSkeletonSignature(Skeleton);
	{
		FReferenceSkeletonModifier Modifier(Skeleton, nullptr);
		Modifier.UpdateRefPoseTransform(1, FTransform(FVector(0, 0, 12)));
	}
	TestNotEqual(TEXT("Reference-pose edits invalidate skeleton signature"), Before, KawaiiClothingProxy::ComputeSkeletonSignature(Skeleton));
	Shape.EndBone = TEXT("missing");
	TestFalse(TEXT("Missing bones rejected"), KawaiiClothingProxy::BuildMinimalHierarchy(Skeleton, {Shape}, Names, Parents, MeshIndices, Error));
	TestTrue(TEXT("Invalid hierarchy cannot expose a partial result"), Names.IsEmpty() && Parents.IsEmpty() && MeshIndices.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyTimeTest, "KawaiiPhysicsBaker.ClothingProxy.PaddedTimeAndInterpolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxyTimeTest::RunTest(const FString&) {
	UKawaiiClothingProxyMotion* Motion = KawaiiProxyTests::MakeMotion();
	Motion->SourceDuration = 0.015f;
	Motion->LocalKeys[3].SetTranslation(FVector(20, 0, 0));
	FString Error;
	TestTrue(TEXT("Nonintegral source duration matches ceil-to-frame output"), Motion->Validate(Error));
	TArray<FTransform> Pose;
	TestTrue(TEXT("Half frame can be sampled"), Motion->SampleLocalPose(0.5f / 60.f, Pose, Error));
	if (Pose.Num() == 2) {
		TestTrue(TEXT("Sample interpolates local position"), Pose[1].GetTranslation().Equals(FVector(15, 0, 0), 1.e-5));
	}
	TestTrue(TEXT("Negative time clamps to start"), Motion->SampleLocalPose(-5.f, Pose, Error));
	if (Pose.Num() == 2) {
		TestTrue(TEXT("Negative sample returns first key"), Pose[1].GetTranslation().Equals(FVector(10, 0, 0), 1.e-5));
	}
	TestTrue(TEXT("Long time clamps to padded endpoint"), Motion->SampleLocalPose(50.f, Pose, Error));
	if (Pose.Num() == 2) {
		TestTrue(TEXT("Endpoint uses the captured exact source end"), Pose[1].GetTranslation().Equals(FVector(20, 0, 0), 1.e-5));
	}
	TestFalse(TEXT("NaN time rejected"), Motion->SampleLocalPose(std::numeric_limits<float>::quiet_NaN(), Pose, Error));
	TestTrue(TEXT("Failed sample clears prior pose"), Pose.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyBlendTest, "KawaiiPhysicsBaker.ClothingProxy.BlendLocalBeforeHierarchy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxyBlendTest::RunTest(const FString&) {
	UKawaiiClothingProxyMotion* A = KawaiiProxyTests::MakeMotion();
	UKawaiiClothingProxyMotion* B = DuplicateObject<UKawaiiClothingProxyMotion>(A, GetTransientPackage());
	// A half-turn has two equally short arcs. UE_PI is also a float constant whose
	// double conversion lies slightly above pi, so hemisphere correction may choose
	// -90 degrees. Use 120 degrees to test the hierarchy against an unambiguous arc.
	const FQuat Turn(FVector::ZAxisVector, FMath::DegreesToRadians(120.0));
	B->LocalKeys[0].SetRotation(Turn);
	B->LocalKeys[2].SetRotation(Turn);
	FString Error;
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	TestTrue(TEXT("Clips blend with normalized weights"),
			 KawaiiClothingProxy::SampleBlended({KawaiiProxyTests::Sample(A, 0.f, 2.f), KawaiiProxyTests::Sample(B, 0.f, 2.f)},
												FTransform::Identity,
												Capsules,
												Error)
	);
	TestEqual(TEXT("Blended sample emits one complete proxy"), Capsules.Num(), 1);
	if (Capsules.Num() == 1) {
		const FVector Expected = FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(60.0)).RotateVector(FVector(10, 0, 0));
		TestTrue(FString::Printf(TEXT("Child follows blended parent rotation: actual %s, expected %s"), *Capsules[0].Start.ToString(), *Expected.ToString()),
				 Capsules[0].Start.Equals(Expected, 1.e-4)
		);
		const FVector InterpolatedEndpoints = (FVector(10, 0, 0) + Turn.RotateVector(FVector(10, 0, 0))) * 0.5;
		TestFalse(TEXT("Local-pose blend differs from world-endpoint interpolation"), Capsules[0].Start.Equals(InterpolatedEndpoints, 1.e-4));
		TestTrue(TEXT("Blended hierarchy preserves the child offset length"), FMath::IsNearlyEqual(Capsules[0].Start.Size(), 10.0, 1.e-4));
		TestTrue(TEXT("Blended capsule geometry is finite"),
				 !Capsules[0].Start.ContainsNaN() && !Capsules[0].End.ContainsNaN() && FMath::IsFinite(Capsules[0].Radius)
		);
		TestTrue(TEXT("Sphere has coincident endpoints"), Capsules[0].Start.Equals(Capsules[0].End));
	}
	UKawaiiClothingProxyMotion* Negated = DuplicateObject<UKawaiiClothingProxyMotion>(B, GetTransientPackage());
	for (FTransform& Key : Negated->LocalKeys) {
		Key.SetRotation(Key.GetRotation() * -1.0);
	}
	TestTrue(TEXT("Opposite quaternion representations remain compatible"),
			 KawaiiClothingProxy::SampleBlended({KawaiiProxyTests::Sample(B), KawaiiProxyTests::Sample(Negated)}, FTransform::Identity, Capsules, Error)
	);
	TestEqual(TEXT("Opposite quaternion blend emits one complete proxy"), Capsules.Num(), 1);
	if (Capsules.Num() == 1) {
		const FVector Expected = Turn.RotateVector(FVector(10, 0, 0));
		TestTrue(
			FString::Printf(TEXT("Hemisphere correction prevents cancellation: actual %s, expected %s"), *Capsules[0].Start.ToString(), *Expected.ToString()),
			Capsules[0].Start.Equals(Expected, 1.e-4)
		);
		TestTrue(TEXT("Hemisphere-corrected geometry is finite"),
				 !Capsules[0].Start.ContainsNaN() && !Capsules[0].End.ContainsNaN() && FMath::IsFinite(Capsules[0].Radius)
		);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySpaceTest, "KawaiiPhysicsBaker.ClothingProxy.ExplicitSpaceAndZeroWeights", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxySpaceTest::RunTest(const FString&) {
	UKawaiiClothingProxyMotion* Motion = KawaiiProxyTests::MakeMotion();
	Motion->RelativeTransform = FTransform(FVector(100, 0, 0));
	FString Error;
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	const FTransform ProxyToTarget(FQuat::Identity, FVector(5, 0, 0), FVector(2));
	TestTrue(
		TEXT("Zero-weight missing clip ignored"),
		KawaiiClothingProxy::SampleBlended({KawaiiProxyTests::Sample(nullptr, 0.f, 0.f), KawaiiProxyTests::Sample(Motion)}, ProxyToTarget, Capsules, Error)
	);
	if (Capsules.Num() == 1) {
		TestTrue(TEXT("Explicit coordinate conversion applied once, stored bake placement not applied again"),
				 Capsules[0].Start.Equals(FVector(25, 0, 0), 1.e-5)
		);
		TestEqual(TEXT("Uniform component scale scales radius"), Capsules[0].Radius, 2.f);
	}
	TestFalse(TEXT("All zero weights produce no collision instead of stale shapes"),
			  KawaiiClothingProxy::SampleBlended({KawaiiProxyTests::Sample(Motion, 0.f, 0.f)}, FTransform::Identity, Capsules, Error)
	);
	TestTrue(TEXT("Zero-weight failure clears outputs"), Capsules.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyValidationTest, "KawaiiPhysicsBaker.ClothingProxy.RejectStaleAndMalformedAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxyValidationTest::RunTest(const FString&) {
	UKawaiiClothingProxyMotion* Motion = KawaiiProxyTests::MakeMotion();
	FString Error;
	TestTrue(TEXT("Valid asset does not require a baked animation reference"), Motion->Validate(Error));
	Motion->Definition->Shapes[0].Radius = 2.f;
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	Capsules.AddDefaulted();
	TestFalse(TEXT("Edited proxy definition invalidates its motion"),
			  UKawaiiClothingProxyLibrary::SampleProxyMotion(Motion, 0.f, FTransform::Identity, Capsules, Error)
	);
	TestTrue(TEXT("Failed sampling clears stale collision"), Capsules.IsEmpty());
	Motion->Definition->Shapes[0].Radius = 1.f;
	Motion->ParentIndices[1] = 1;
	TestFalse(TEXT("Self-parent is rejected before pose reconstruction"), Motion->Validate(Error));
	Motion->ParentIndices[1] = 0;
	Motion->LocalKeys.Pop();
	TestFalse(TEXT("Truncated key buffer rejected"), Motion->Validate(Error));
	Motion->LocalKeys.Add(FTransform::Identity);
	Motion->LocalKeys[0].SetScale3D(FVector(-1, 1, 1));
	TestFalse(TEXT("Negative scale rejected"), Motion->Validate(Error));
	Motion->LocalKeys[0] = FTransform::Identity;
	Motion->Shapes[0].StartOffset.X = 1.f;
	TestFalse(TEXT("Modified baked shape snapshot rejected"), Motion->Validate(Error));
	Motion->Shapes = Motion->Definition->Shapes;
	Motion->SourceSkeletonSignature = TEXT("stale");
	TestFalse(TEXT("Loaded mesh reference-skeleton change rejected by full validation"), Motion->Validate(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyCollisionRulesTest, "KawaiiPhysicsBaker.ClothingProxy.LiveCollisionRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxyCollisionRulesTest::RunTest(const FString&) {
	UKawaiiClothingProxyMotion* Motion = KawaiiProxyTests::MakeMotion();
	const FString GeometrySignature = Motion->DefinitionSignature;
	Motion->Definition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	FString Error;
	TestTrue(TEXT("Target collision rules do not invalidate recorded geometry or tracks"), Motion->Validate(Error));
	TestEqual(TEXT("Collision rules are independent of the geometry signature"), Motion->Definition->ComputeSignature(), GeometrySignature);
	TArray<FKawaiiClothingProxyCapsule> Capsules;
	if (!TestTrue(TEXT("Sampling carries the garment's rules into value-only collision output"),
				  UKawaiiClothingProxyLibrary::SampleProxyMotion(Motion, 0.f, FTransform::Identity, Capsules, Error)
	))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("One collision shape is sampled"), Capsules.Num(), 1)) {
		return false;
	}
	const FKawaiiClothingProxyCapsule PreviousFrame = Capsules[0];
	TestTrue(TEXT("The tail exclusion reaches the animation input"), PreviousFrame.IgnoredTargetRootBones == TArray<FName>({TEXT("Tail_Root")}));
	Motion->Definition->IgnoredTargetRootBones = {TEXT("Hair_back")};
	TestTrue(TEXT("Existing motion reads new rules without being rebaked"),
			 UKawaiiClothingProxyLibrary::SampleProxyMotion(Motion, 0.f, FTransform::Identity, Capsules, Error)
	);
	if (Capsules.Num() == 1) {
		TestTrue(TEXT("The next sample uses the changed rule"), Capsules[0].IgnoredTargetRootBones == TArray<FName>({TEXT("Hair_back")}));
	}
	TestTrue(TEXT("Editing the asset cannot mutate a worker's previous value snapshot"), PreviousFrame.IgnoredTargetRootBones == TArray<FName>({TEXT("Tail_Root")}));
	Motion->Definition->IgnoredTargetRootBones = {NAME_None};
	TestFalse(TEXT("Malformed collision rules reject sampling"),
			  UKawaiiClothingProxyLibrary::SampleProxyMotion(Motion, 0.f, FTransform::Identity, Capsules, Error)
	);
	TestTrue(TEXT("A rejected rule cannot leave stale output capsules"), Capsules.IsEmpty());
	return true;
}

#endif
