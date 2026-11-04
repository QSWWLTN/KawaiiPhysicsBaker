#include "KawaiiClothingProxyMotion.h"

#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiClothingProxyPlayback.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxySerializedPayloadTest, "KawaiiPhysicsBaker.ClothingProxy.SerializedExportPayload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)  bool FKawaiiProxySerializedPayloadTest::RunTest(const FString&) {
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>());
	Definition->PreviewMesh = Mesh.Get();
	FKawaiiClothingProxyShape Shape;
	Shape.Id = TEXT("BackCollar");
	Shape.StartBone = TEXT("collar");
	Shape.EndBone = TEXT("root");
	Shape.StartOffset = FVector(1, 0, 0);
	Shape.EndOffset = FVector(0, 2, 0);
	Shape.Radius = 2.f;
	Definition->Shapes.Add(Shape);

	TStrongObjectPtr<UKawaiiClothingProxyMotion> Original(NewObject<UKawaiiClothingProxyMotion>());
	Original->Definition = Definition.Get();
	Original->DefinitionSignature = Definition->ComputeSignature();
	Original->SourceMesh = Mesh.Get();
	Original->SourceAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(TEXT("/Game/KawaiiProxyTest/Source.Source")));
	Original->BakedAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(TEXT("/Game/KawaiiProxyTest/Baked.Baked")));
	Original->BodyBakedAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(TEXT("/Game/KawaiiProxyTest/Body.Body")));
	Original->SourceDataGuid = FGuid::NewGuid();
	Original->SourceSkeletonSignature = KawaiiClothingProxy::ComputeSkeletonSignature(Mesh->GetRefSkeleton());
	Original->BakeGroup = FGuid::NewGuid();
	Original->PartName = TEXT("Clothing");
	Original->RelativeTransform = FTransform(FVector(3, 4, 5));
	Original->SampleRate = 60;
	Original->NumFrames = 2;
	Original->SourceDuration = 0.015f;
	Original->BoneNames = {TEXT("root"), TEXT("collar")};
	Original->ParentIndices = {INDEX_NONE, 0};
	Original->Shapes = Definition->Shapes;
	Original->LocalKeys = {FTransform::Identity,
						   FTransform(FVector(0, 0, 10)),
						   FTransform(FVector(2, 0, 0)),
						   FTransform(FQuat(FVector::ZAxisVector, UE_PI / 4), FVector(0, 0, 12))};
	FString Error;
	if (!TestTrue(TEXT("Complete export payload validates before serialization"), Original->Validate(Error))) {
		AddError(Error);
		return false;
	}

	// Exercise reflected UObject serialization, including the non-visible LocalKeys
	// payload, soft source identities and hard definition reference. No content fixtures
	// or package files are created by this test.
	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes, true);
		FObjectAndNameAsStringProxyArchive Archive(Writer, false);
		Archive.ArNoDelta = true;
		Original->Serialize(Archive);
		TestFalse(TEXT("Export payload serializes without archive errors"), Archive.IsError());
	}
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Restored(NewObject<UKawaiiClothingProxyMotion>());
	{
		FMemoryReader Reader(Bytes, true);
		FObjectAndNameAsStringProxyArchive Archive(Reader, false);
		Archive.ArNoDelta = true;
		Restored->Serialize(Archive);
		TestFalse(TEXT("Export payload deserializes without archive errors"), Archive.IsError());
	}
	TestTrue(TEXT("Hard proxy definition reference survives"), Restored->Definition == Definition.Get());
	TestEqual(TEXT("Stable geometry signature survives"), Restored->DefinitionSignature, Original->DefinitionSignature);
	TestTrue(TEXT("Source action identity survives"), Restored->SourceAnimation.ToSoftObjectPath() == Original->SourceAnimation.ToSoftObjectPath());
	TestTrue(TEXT("Paired display animation identity survives"), Restored->BakedAnimation.ToSoftObjectPath() == Original->BakedAnimation.ToSoftObjectPath());
	TestTrue(TEXT("Baked body playback identity survives"), Restored->BodyBakedAnimation.ToSoftObjectPath() == Original->BodyBakedAnimation.ToSoftObjectPath());
	TestTrue(TEXT("Source revision and group survive"), Restored->SourceDataGuid == Original->SourceDataGuid && Restored->BakeGroup == Original->BakeGroup);
	TestTrue(TEXT("Reduced hierarchy survives"), Restored->BoneNames == Original->BoneNames && Restored->ParentIndices == Original->ParentIndices);
	TestEqual(TEXT("Flattened key payload is serialized"), Restored->LocalKeys.Num(), Original->LocalKeys.Num());
	TestTrue(TEXT("Stored placement survives"), Restored->RelativeTransform.Equals(Original->RelativeTransform));
	TestTrue(TEXT("Restored runtime payload validates"), Restored->Validate(Error));

	TArray<FKawaiiClothingProxyCapsule> Before, After;
	const float Midpoint = 0.5f / Original->SampleRate;
	const bool bBefore = UKawaiiClothingProxyLibrary::SampleProxyMotion(Original.Get(), Midpoint, Original->RelativeTransform, Before, Error);
	const bool bAfter = UKawaiiClothingProxyLibrary::SampleProxyMotion(Restored.Get(), Midpoint, Restored->RelativeTransform, After, Error);
	TestTrue(TEXT("Both original and deserialized assets can be sampled"), bBefore && bAfter);
	if (Before.Num() == 1 && After.Num() == 1) {
		TestTrue(TEXT("Serialized tracks produce identical interpolated collision"),
				 Before[0].Id == After[0].Id && Before[0].Start.Equals(After[0].Start, 1.e-6) && Before[0].End.Equals(After[0].End, 1.e-6)
					 && FMath::IsNearlyEqual(Before[0].Radius, After[0].Radius, 1.e-6f)
		);
	} else {
		AddError(TEXT("Expected one collision capsule after serialization roundtrip."));
	}

	TStrongObjectPtr<UKawaiiClothingProxyMotionSet> Set(NewObject<UKawaiiClothingProxyMotionSet>());
	Set->Motions.Add(Restored.Get());
	Bytes.Reset();
	{
		FMemoryWriter Writer(Bytes, true);
		FObjectAndNameAsStringProxyArchive Archive(Writer, false);
		Archive.ArNoDelta = true;
		Set->Serialize(Archive);
	}
	TStrongObjectPtr<UKawaiiClothingProxyMotionSet> RestoredSet(NewObject<UKawaiiClothingProxyMotionSet>());
	{
		FMemoryReader Reader(Bytes, true);
		FObjectAndNameAsStringProxyArchive Archive(Reader, false);
		Archive.ArNoDelta = true;
		RestoredSet->Serialize(Archive);
	}
	TestTrue(TEXT("Generated action collection retains its motion references"), RestoredSet->Motions.Num() == 1 && RestoredSet->Motions[0] == Restored.Get());
	return true;
}

#endif
