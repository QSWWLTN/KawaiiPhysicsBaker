// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimBlueprint.h"
#include "Blueprint/BlueprintExtension.h"
#include "Engine/EngineTypes.h"
#include "KawaiiPhysicsBakeBlueprint.generated.h"

class UKawaiiPhysicsClothingCollisionProfile;
class UAnimSequence;
class USkeletalMesh;

/** A second independently evaluated mesh in the same deterministic bake session. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKER_API FKawaiiPhysicsBakePart
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Part")
		FName Name;

	UPROPERTY(EditAnywhere, Category = "Part")
		TObjectPtr<USkeletalMesh> Mesh = nullptr;

	/** The graph is evaluated on this mesh after all preceding parts. */
	UPROPERTY(EditAnywhere, Category = "Part")
		TObjectPtr<UAnimBlueprint> Graph = nullptr;

	/** Leave empty to copy the completed pose of Copy Pose From, matching bone names. */
	UPROPERTY(EditAnywhere, Category = "Source")
		TObjectPtr<UAnimSequence> Animation = nullptr;

	/** Main, or the unique name of an earlier part. */
	UPROPERTY(EditAnywhere, Category = "Source", meta = (EditCondition = "Animation == nullptr"))
		FName CopyPoseFrom = TEXT("Main");

	UPROPERTY(EditAnywhere, Category = "Part")
		FTransform RelativeTransform = FTransform::Identity;
};

/** Settings belong to the graph asset; baking never edits the source animation. */
UCLASS(EditInlineNew, DefaultToInstanced)
class KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings : public UBlueprintExtension
{
	GENERATED_BODY()

public:
	UKawaiiPhysicsBakeSettings();

	UPROPERTY(EditAnywhere, Category = "Source")
		TObjectPtr<USkeletalMesh> MainMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "Source")
		TObjectPtr<UAnimSequence> SourceAnimation = nullptr;

	UPROPERTY(EditAnywhere, Category = "Sampling", meta = (ClampMin = "1", ClampMax = "240"))
		int32 SampleRate = 60;

	/** Fixed simulation steps held at source time zero before the first recorded frame. */
	UPROPERTY(EditAnywhere, Category = "Sampling", meta = (ClampMin = "0", ClampMax = "3600"))
		int32 WarmUpFrames = 60;

	/** Long package directory, for example /Game/KawaiiBakes. */
	UPROPERTY(EditAnywhere, Category = "Output")
		FDirectoryPath OutputDirectory;

	UPROPERTY(EditAnywhere, Category = "Output")
		FString OutputPrefix = TEXT("Baked");

	/** Ordered dependencies: a part can only copy or collide against Main or an earlier part. */
	UPROPERTY(EditAnywhere, Category = "Parts", meta = (TitleProperty = "Name"))
		TArray<FKawaiiPhysicsBakePart> Parts;

	/** Enable the project's clothing collision bridge, with Main as the receiving mesh. */
	UPROPERTY(EditAnywhere, Category = "Clothing Collision")
		bool bEnableClothingCollision = false;

	/** The name of the part whose final pose supplies the Main mesh's clothing proxy. */
	UPROPERTY(EditAnywhere, Category = "Clothing Collision", meta = (EditCondition = "bEnableClothingCollision"))
		FName CollisionSourcePart = TEXT("Clothing");

	UPROPERTY(EditAnywhere, Category = "Clothing Collision", meta = (EditCondition = "bEnableClothingCollision"))
		TObjectPtr<UKawaiiPhysicsClothingCollisionProfile> ClothingCollisionProfile = nullptr;
};

/** Use Unreal's native AnimBlueprint/compiler; persist bake settings as an owned extension. */
namespace KawaiiPhysicsBake
{
	KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings* GetSettings(const UAnimBlueprint* Blueprint);
	KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings* FindOrAddSettings(UAnimBlueprint* Blueprint);
	KAWAIIPHYSICSBAKER_API bool IsBakeBlueprint(const UAnimBlueprint* Blueprint);
}
