#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "KawaiiPhysicsIKCorrectionMath.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.generated.h"

/** Both inputs use this mesh's Skeleton. DriverPose must contain the body's final IK pose. */
USTRUCT(BlueprintInternalUseOnly)
struct KAWAIIPHYSICSBAKERRUNTIME_API FAnimNode_KawaiiPhysicsIKCorrection : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Links")
		FPoseLink BakedPose;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Links")
		FPoseLink DriverPose;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (PinHiddenByDefault))
		TObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Profile = nullptr;

	/** Zero retains baked clothing relative to current anchors. Values below one weaken collision correction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (PinShownByDefault, ClampMin = "0.0", ClampMax = "1.0"))
		float Alpha = 1.f;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;

	const FString& GetCorrectionDebugReason() const { return DebugReason; }
	double GetMaxPenetration() const { return MaxPenetration; }
	double GetMaxLengthError() const { return MaxLengthError; }

private:
	// Only PreUpdate reads the UObject. Worker evaluation uses owned value snapshots.
	TArray<FKawaiiPhysicsIKCorrectionChain> SnapshotChains;
	TArray<FKawaiiPhysicsIKCorrectionCapsule> SnapshotCapsules;
	TArray<KawaiiPhysicsIKCorrection::FCapsule> SnapshotParentCapsules;
	bool bSnapshotUseAttachedParent = false;
	FString SnapshotError;
	int32 SnapshotIterations = 16;
	bool bHasProfileSnapshot = false;
	FString DebugReason;
	double MaxPenetration = 0;
	double MaxLengthError = 0;
};
