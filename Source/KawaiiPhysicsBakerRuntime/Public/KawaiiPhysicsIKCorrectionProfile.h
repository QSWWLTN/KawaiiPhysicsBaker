#pragma once

#include "CoreMinimal.h"
#include "BoneContainer.h"
#include "Engine/DataAsset.h"
#include "Curves/CurveFloat.h"

#include "KawaiiPhysicsIKCorrectionProfile.generated.h"

class USkeletalMesh;

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
	/** Multiplier evaluated by distance along the baked chain. An empty curve means one. */
	UPROPERTY(EditAnywhere, Category = "Collision", meta = (DisplayName = "碰撞半径曲线"))
		FRuntimeFloatCurve RadiusCurve;
	/** Maximum additional segment deflection from the current correction input, not the reference skeleton. 0 disables. */
	UPROPERTY(EditAnywhere, Category = "Shape Constraints", meta = (ClampMin = "0", ClampMax = "180", DisplayName = "修正偏转角上限"))
		float LimitAngle = 0.f;
	UPROPERTY(EditAnywhere, Category = "Shape Constraints", meta = (DisplayName = "偏转角曲线"))
		FRuntimeFloatCurve LimitAngleCurve;
	/** Allowed change of the angle between adjacent segments relative to the input animation. 0 disables. */
	UPROPERTY(EditAnywhere, Category = "Shape Constraints", meta = (ClampMin = "0", ClampMax = "180", DisplayName = "相邻骨段弯曲变化上限"))
		float MaxBendChange = 0.f;

	/** Measured in the editor from LOD 0 skin vertices. Serialized for cooked builds; no runtime vertex reads. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision", meta = (ForceUnits = "cm", DisplayName = "最大网格偏移（诊断）"))
		float MeshEnvelopeRadius = 0.f;
};

/** Current-frame body primitive; boxes use StartBone/StartOffset as their center. */
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
	UPROPERTY(EditAnywhere, Category = "Capsule", meta = (DisplayName = "使用盒体"))
		bool bBox = false;
	UPROPERTY(EditAnywhere, Category = "Capsule", meta = (DisplayName = "盒体旋转", EditCondition = "bBox"))
		FRotator BoxRotation = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere, Category = "Capsule", meta = (DisplayName = "盒体半尺寸", EditCondition = "bBox", ClampMin = "0.01"))
		FVector BoxExtent = FVector(5.f);
};

/** Collision-only virtual points between explicitly chosen bones on separate chains. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiPhysicsIKContactBridge {
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "Contact") FBoneReference BoneA;
	UPROPERTY(EditAnywhere, Category = "Contact") FBoneReference BoneB;
	UPROPERTY(EditAnywhere, Category = "Contact", meta = (ClampMin = "1", ClampMax = "10")) int32 Samples = 3;
	UPROPERTY(EditAnywhere, Category = "Contact", meta = (ClampMin = "0", ClampMax = "2")) float Feedback = 1.f;
};

/** Approximate runtime correction of baked clothing after body IK; not a mesh collision solver. */
UCLASS(BlueprintType)
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiPhysicsIKCorrectionProfile : public UDataAsset
{
	GENERATED_BODY()
public:
	/** Independent of bend preservation; may oppose collision avoidance when the input itself penetrates. */
	UPROPERTY(EditAnywhere, Category = "Shape Constraints", meta = (DisplayName = "启用修正偏转角限制"))
		bool bUseAngleLimits = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction")
		TArray<FKawaiiPhysicsIKCorrectionChain> Chains;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction")
		TArray<FKawaiiPhysicsIKCorrectionCapsule> Capsules;

	/** Empty by default: only explicitly authored neighbors are coupled. No gravity or distance spring is added. */
	UPROPERTY(EditAnywhere, Category = "Correction", meta = (DisplayName = "横向虚拟接触", TitleProperty = "{BoneA} - {BoneB}"))
		TArray<FKawaiiPhysicsIKContactBridge> ContactBridges;
	UPROPERTY(EditAnywhere, Category = "Correction", meta = (ClampMin = "1", ClampMax = "16", DisplayName = "横向接触迭代"))
		int32 BridgeIterations = 4;

	/** Experimental uniform envelope. Disabled by default because it can spread an otherwise valid hairstyle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (DisplayName = "实验：使用整链网格包络（可能撑开发型）"))
		bool bUseMeshEnvelope = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Correction", meta = (DisplayName = "包络来源网格"))
		TSoftObjectPtr<USkeletalMesh> MeshEnvelopeSource;

	/** Read capsule bones from the nearest skeletal attach parent and convert them to this mesh's space.
	 * The parent must finish animation before this mesh ticks. False uses the Driver Pose directly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction")
		bool bUseAttachedParentForCollision = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (ClampMin = "1", ClampMax = "64"))
		int32 Iterations = 16;

	/** Each frame starts from the previous corrected chain (relative to its anchor) and returns toward the
	 * input pose with this time constant before contacts are resolved. Contacts stay on the side they were on
	 * and are still fully resolved every frame. 0 disables settling; both time constants must be 0
	 * to disable side history and restore the stateless per-frame solve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Temporal Stability",
		meta = (ClampMin = "0", ClampMax = "1", ForceUnits = "s", DisplayName = "接触稳定时间",
			ToolTip = "每帧从上一帧的修正结果（相对锚点）出发，按此时间常数回到输入姿态后再解算接触；接触保持在原来的一侧，仍每帧完全解算，用于消除逐帧独立解算造成的头发抖动。0 关闭位置稳定；接触稳定时间和输出平滑时间都为 0 时才关闭历史，恢复逐帧独立解算。"))
		float SettleTime = 0.08f;

	/** Exponential smoothing of the correction offset relative to each chain anchor. 0 disables it.
	 * Smoothed output is projected against the current contacts again before emission. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Temporal Stability",
		meta = (ClampMin = "0", ClampMax = "1", ForceUnits = "s", DisplayName = "输出平滑时间",
			ToolTip = "对修正位移（相对骨链锚点）做指数平滑的时间常数；0 关闭。平滑后会重新解算碰撞，接触时优先避让；固定根部及近似代理仍可能有残余穿插。"))
		float SmoothingTime = 0.03f;
};
