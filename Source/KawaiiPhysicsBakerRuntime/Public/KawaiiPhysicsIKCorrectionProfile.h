#pragma once

#include "CoreMinimal.h"
#include "BoneContainer.h"
#include "Engine/DataAsset.h"
#include "KawaiiPhysicsIKCorrectionProfile.generated.h"

/** A direct root-to-tip clothing path. Its entire root subtree retains baked local motion. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiPhysicsIKCorrectionChain
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Chain")
		FBoneReference RootBone;

	UPROPERTY(EditAnywhere, Category = "Chain")
		FBoneReference TipBone;

	/** Body driver outside every corrected subtree, normally the corresponding thigh. */
	UPROPERTY(EditAnywhere, Category = "Chain")
		FBoneReference DriverBone;

	/** Additional driver influence rises from zero at the fixed root to this value at the tip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chain", meta = (ClampMin = "0.0", ClampMax = "1.0"))
		float DriverWeight = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision", meta = (ClampMin = "0.0", ForceUnits = "cm"))
		float CollisionRadius = 1.f;
};

/** Current-frame body capsule, authored in its endpoint bones' local spaces. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiPhysicsIKCorrectionCapsule
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Capsule")
		FBoneReference StartBone;

	UPROPERTY(EditAnywhere, Category = "Capsule")
		FBoneReference EndBone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capsule")
		FVector StartOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capsule")
		FVector EndOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capsule", meta = (ClampMin = "0.0", ForceUnits = "cm"))
		float Radius = 5.f;
};

/** Approximate runtime correction of baked clothing after body IK; not a mesh collision solver. */
UCLASS(BlueprintType)
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiPhysicsIKCorrectionProfile : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction")
		TArray<FKawaiiPhysicsIKCorrectionChain> Chains;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction")
		TArray<FKawaiiPhysicsIKCorrectionCapsule> Capsules;

	/** Read capsule bones from the nearest skeletal attach parent and convert them to this mesh's space.
	 * The parent must finish animation before this mesh ticks. False uses the Driver Pose directly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction")
		bool bUseAttachedParentForCollision = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (ClampMin = "1", ClampMax = "64"))
		int32 Iterations = 16;
};
