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

	/** Each frame starts from the previous corrected chain (relative to its anchor) and returns toward the
	 * input pose with this time constant before contacts are resolved. Contacts stay on the side they were on
	 * and are still fully resolved every frame. 0 restores the stateless per-frame solve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Temporal Stability",
		meta = (ClampMin = "0", ClampMax = "1", ForceUnits = "s", DisplayName = "接触稳定时间",
			ToolTip = "每帧从上一帧的修正结果（相对锚点）出发，按此时间常数回到输入姿态后再解算接触；接触保持在原来的一侧，仍每帧完全解算，用于消除逐帧独立解算造成的头发抖动。0 恢复旧的逐帧独立解算。"))
		float SettleTime = 0.08f;

	/** Exponential smoothing of the final correction offset relative to each chain anchor. 0 disables it.
	 * Larger values are steadier but leave more residual overlap during fast contact. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Temporal Stability",
		meta = (ClampMin = "0", ClampMax = "1", ForceUnits = "s", DisplayName = "输出平滑时间",
			ToolTip = "对最终修正位移（相对骨链锚点）做指数平滑的时间常数；0 关闭。越大越稳定，但快速接触时残余穿插越多。"))
		float SmoothingTime = 0.03f;
};
