#include "Engine/SkeletalMesh.h"
#include "KawaiiPhysicsClothingCollisionProfile.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsBakePreviewCollision.h"
#include "KawaiiClothingProxyAsset.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace {
	void InitializeDefinition(UKawaiiClothingProxyAsset& Definition) {
		Definition.PreviewMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/PreviewProxyTest/Clothing.Clothing")));
		FKawaiiClothingProxyShape Shape;
		Shape.Id = TEXT("BackCollar");
		Shape.StartBone = TEXT("collar");
		Shape.EndBone = TEXT("coat_back");
		Shape.StartOffset = FVector(1, 2, 3);
		Shape.EndOffset = FVector(-4, 5, 6);
		Shape.Radius = 2.5f;
		Definition.Shapes = {Shape};
	}

	void SeedProfile(UKawaiiPhysicsClothingCollisionProfile& Profile) {
		Profile.StaticCapsules.AddDefaulted();
		Profile.DynamicCapsules.AddDefaulted();
		Profile.ComponentCapsules.AddDefaulted();
		Profile.TargetRootBones.Add(TEXT("OldRoot"));
	}

	bool IsProfileEmpty(const UKawaiiPhysicsClothingCollisionProfile& Profile) {
		return Profile.StaticCapsules.IsEmpty() && Profile.DynamicCapsules.IsEmpty() && Profile.ComponentCapsules.IsEmpty()
			&& Profile.TargetRootBones.IsEmpty();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiPreviewProxyGeometryTest,
	"KawaiiPhysicsBaker.PreviewProxy.ProfileGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiPreviewProxyGeometryTest::RunTest(const FString&) {
	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>());
	TStrongObjectPtr<UKawaiiPhysicsClothingCollisionProfile> Profile(NewObject<UKawaiiPhysicsClothingCollisionProfile>());
	InitializeDefinition(*Definition);
	FKawaiiClothingProxyShape Sphere = Definition->Shapes[0];
	Sphere.Id = TEXT("CollarSphere");
	Sphere.EndBone = NAME_None;
	Sphere.EndOffset = Sphere.StartOffset;
	Definition->Shapes.Add(Sphere);
	const FString Signature = Definition->ComputeSignature();
	SeedProfile(*Profile);
	FString Error = TEXT("stale error");
	if (!TestTrue(
			TEXT("A proxy definition builds without a legacy collision asset"),
			KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, {TEXT("Hair_back")}, {}, *Profile, Error)
		))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Successful conversion clears stale errors"), Error.IsEmpty());
	TestTrue(TEXT("Old static and component geometry is discarded"), Profile->StaticCapsules.IsEmpty() && Profile->ComponentCapsules.IsEmpty());
	if (TestEqual(TEXT("Every proxy shape becomes a dynamic collider"), Profile->DynamicCapsules.Num(), Definition->Shapes.Num())) {
		for (int32 Index = 0; Index < Definition->Shapes.Num(); ++Index) {
			const FKawaiiClothingProxyShape& Shape = Definition->Shapes[Index];
			const FKawaiiClothingCapsuleDefinition& Capsule = Profile->DynamicCapsules[Index];
			TestFalse(TEXT("Proxy attachments retain bone semantics"), Capsule.bUseSockets);
			TestEqual(TEXT("Start attachment is preserved"), Capsule.StartBone, Shape.StartBone);
			TestEqual(TEXT("End attachment, including None, is preserved"), Capsule.EndBone, Shape.EndBone);
			TestTrue(TEXT("Both local offsets are preserved"), Capsule.StartOffset.Equals(Shape.StartOffset) && Capsule.EndOffset.Equals(Shape.EndOffset));
			TestEqual(TEXT("Radius is preserved"), Capsule.Radius, Shape.Radius);
		}
	}
	TestEqual(TEXT("Wall time does not expire offline snapshots"), Profile->MaxSnapshotAgeSeconds, MAX_flt);
	TestEqual(TEXT("Preview profile does not cull by LOD"), Profile->MaxLOD, -1);
	TestEqual(TEXT("Preview profile does not cull by camera distance"), Profile->MaxDistance, 0.f);
	TestEqual(TEXT("Building preview collision never changes the reusable definition"), Definition->ComputeSignature(), Signature);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiPreviewProxyRootsTest,
	"KawaiiPhysicsBaker.PreviewProxy.RootSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiPreviewProxyRootsTest::RunTest(const FString&) {
	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>());
	TStrongObjectPtr<UKawaiiPhysicsClothingCollisionProfile> Profile(NewObject<UKawaiiPhysicsClothingCollisionProfile>());
	InitializeDefinition(*Definition);
	const TArray<FName> Available = {NAME_None, TEXT("Hair_back"), TEXT("Hair_side_L"), TEXT("Hair_back")};
	FString Error;
	TestTrue(
		TEXT("Empty selection discovers every nonempty compiled root"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {}, *Profile, Error)
	);
	TestTrue(TEXT("Automatic roots are deduplicated in stable order"), Profile->TargetRootBones == TArray<FName>({TEXT("Hair_back"), TEXT("Hair_side_L")}));
	TestTrue(
		TEXT("Explicit selection is supported"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {TEXT("Hair_side_L"), TEXT("Hair_side_L")}, *Profile, Error)
	);
	TestTrue(TEXT("Explicit selection is deduplicated without enabling other roots"), Profile->TargetRootBones == TArray<FName>({TEXT("Hair_side_L")}));
	TestFalse(
		TEXT("A partially valid selection cannot silently skip an invalid root"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {TEXT("Hair_back"), TEXT("MissingRoot")}, *Profile, Error)
	);
	TestTrue(TEXT("Invalid root clears all earlier geometry and consumers"), IsProfileEmpty(*Profile));
	TestTrue(TEXT("Invalid root is named in the error"), Error.Contains(TEXT("MissingRoot")));
	SeedProfile(*Profile);
	TestFalse(TEXT("None is not a valid explicit root"), KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {NAME_None}, *Profile, Error));
	TestTrue(TEXT("Invalid None selection clears the result"), IsProfileEmpty(*Profile));
	SeedProfile(*Profile);
	TestFalse(TEXT("No available consumers is an error"), KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, {NAME_None}, {}, *Profile, Error));
	TestTrue(TEXT("No consumers clears the result and reports why"), IsProfileEmpty(*Profile) && !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiPreviewProxyInvalidDefinitionTest,
	"KawaiiPhysicsBaker.PreviewProxy.InvalidDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiPreviewProxyInvalidDefinitionTest::RunTest(const FString&) {
	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>());
	TStrongObjectPtr<UKawaiiPhysicsClothingCollisionProfile> Profile(NewObject<UKawaiiPhysicsClothingCollisionProfile>());
	InitializeDefinition(*Definition);
	Definition->Shapes[0].Radius = -1.f;
	const FString InvalidSignature = Definition->ComputeSignature();
	SeedProfile(*Profile);
	FString Error;
	TestFalse(TEXT("Invalid geometry is rejected"), KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, {TEXT("Hair_back")}, {}, *Profile, Error));
	TestTrue(TEXT("Invalid geometry cannot leave stale preview collision"), IsProfileEmpty(*Profile) && !Error.IsEmpty());
	TestEqual(TEXT("Validation does not repair or mutate the source asset"), Definition->ComputeSignature(), InvalidSignature);

	InitializeDefinition(*Definition);
	const FKawaiiClothingProxyShape Template = Definition->Shapes[0];
	Definition->Shapes.Reset();
	for (int32 Index = 0; Index < KawaiiClothingProxy::MaxShapes; ++Index) {
		FKawaiiClothingProxyShape Shape = Template;
		Shape.Id = FName(*FString::Printf(TEXT("Shape_%d"), Index));
		Definition->Shapes.Add(Shape);
	}
	TestTrue(
		TEXT("The existing 64-shape limit is accepted exactly"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, {TEXT("Hair_back")}, {}, *Profile, Error)
	);
	FKawaiiClothingProxyShape Extra = Template;
	Extra.Id = TEXT("Overflow");
	Definition->Shapes.Add(Extra);
	TestFalse(
		TEXT("Exceeding the runtime shape limit is rejected without truncation"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, {TEXT("Hair_back")}, {}, *Profile, Error)
	);
	TestTrue(TEXT("Oversized input clears the previously successful profile"), IsProfileEmpty(*Profile) && !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiPreviewProxyIgnoredRootsTest,
	"KawaiiPhysicsBaker.PreviewProxy.IgnoredRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiPreviewProxyIgnoredRootsTest::RunTest(const FString&) {
	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>());
	TStrongObjectPtr<UKawaiiPhysicsClothingCollisionProfile> Profile(NewObject<UKawaiiPhysicsClothingCollisionProfile>());
	InitializeDefinition(*Definition);
	const TArray<FName> Available = {TEXT("Hair_back"), TEXT("Tail_Root"), TEXT("Hair_side_L"), TEXT("Tail_Root")};
	FString Error;
	Definition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	TestTrue(
		TEXT("Automatic discovery can ignore the tail without enumerating every hair root"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {}, *Profile, Error)
	);
	TestTrue(TEXT("Ignoring the tail preserves the remaining root order"), Profile->TargetRootBones == TArray<FName>({TEXT("Hair_back"), TEXT("Hair_side_L")}));
	Definition->IgnoredTargetRootBones = {TEXT("Tail_Root"), TEXT("Tail_Root")};
	TestTrue(TEXT("Duplicate ignored roots are accepted once"), KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {}, *Profile, Error));
	TestTrue(
		TEXT("Duplicate ignore entries do not remove other consumers"),
		Profile->TargetRootBones == TArray<FName>({TEXT("Hair_back"), TEXT("Hair_side_L")})
	);
	TestTrue(
		TEXT("Ignoring a root takes precedence over explicitly requesting it"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {TEXT("Tail_Root"), TEXT("Hair_back")}, *Profile, Error)
	);
	TestTrue(TEXT("Only the explicitly selected and nonignored root remains"), Profile->TargetRootBones == TArray<FName>({TEXT("Hair_back")}));
	Definition->IgnoredTargetRootBones = {TEXT("AnotherCharacterTail"), TEXT("Tail_Root")};
	TestTrue(
		TEXT("A reusable garment may contain exclusions for another character's roots"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {}, *Profile, Error)
	);
	TestTrue(TEXT("Unmatched exclusions do not disable hair collision"), Profile->TargetRootBones.Contains(TEXT("Hair_back")));
	SeedProfile(*Profile);
	Definition->IgnoredTargetRootBones = {NAME_None};
	TestFalse(TEXT("None cannot be used as an ignored root"), KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {}, *Profile, Error));
	TestTrue(TEXT("None in the ignore list clears the profile and reports an error"), IsProfileEmpty(*Profile) && !Error.IsEmpty());
	SeedProfile(*Profile);
	Definition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	TestTrue(
		TEXT("A garment can deliberately ignore every consumer in this preview"),
		KawaiiPhysicsBakePreviewCollision::BuildProfile(*Definition, Available, {TEXT("Tail_Root")}, *Profile, Error)
	);
	TestTrue(
		TEXT("Fully ignored preview has no consumers and no stale legacy geometry"),
		Profile->TargetRootBones.IsEmpty() && Profile->StaticCapsules.IsEmpty() && Profile->ComponentCapsules.IsEmpty()
	);
	return true;
}

#endif
