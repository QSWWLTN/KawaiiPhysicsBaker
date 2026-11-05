#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"

#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "KawaiiPhysicsIKCorrectionMath.h"
#include "KawaiiClothingProxyMotion.h"

#include "AnimNode_KawaiiPhysicsIKCorrection.generated.h"

namespace KawaiiPhysicsGPU {
	struct FState;
}

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

	/** Current-frame capsules already transformed into this mesh's component space.
	 * Sample the custom proxy motion assets using the same action times/weights as the displayed clothing.
	 * Value data is consumed after exposed inputs update; no previous-frame component pose is read. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clothing Proxy", meta = (PinShownByDefault, DisplayName = "Clothing Proxy Capsules"))
		TArray<FKawaiiClothingProxyCapsule> ExternalCapsules;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;

	virtual bool HasPreUpdate() const override { return true; }

	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	/** Reuse the snapshotted solver with already evaluated local poses. */
	void CorrectPose(FPoseContext& Output, const FPoseContext& BakedContext);
	/** Owners that call CorrectPose directly report their update time here; Update_AnyThread does it for this node. */
	void AccumulateDeltaTime(float DeltaTime);
	/** Forget the previous solution, e.g. when the owner skips correction for a frame. The next solve is stateless. */
	void ResetTemporalState();
	void ReportGPUReason(const FString& Reason);

	const FString& GetCorrectionDebugReason() const { return DebugReason; }

	double GetMaxPenetration() const { return MaxPenetration; }

	double GetMaxLengthError() const { return MaxLengthError; }

private:
	friend class FKawaiiGPUProjectClimbTest;
	TSharedPtr<KawaiiPhysicsGPU::FState, ESPMode::ThreadSafe> GPUState;
	/** Only a successful correction evaluation requests a deformer for the following update. */
	bool bGPUCorrectionRequested = false;

	/** Previous solution of one chain, stored relative to its live anchor so body motion never lags. */
	struct FChainTemporalState
	{
		TArray<int32> Bones;
		int32 Anchor = INDEX_NONE;
		TArray<FVector> SolvedLocal;
		TArray<FVector> SmoothedOffsetLocal;
	};

	// Only PreUpdate reads the UObject. Worker evaluation uses owned value snapshots.
	TArray<FKawaiiPhysicsIKCorrectionChain> SnapshotChains;
	TArray<FKawaiiPhysicsIKCorrectionCapsule> SnapshotCapsules;
	TArray<KawaiiPhysicsIKCorrection::FCapsule> SnapshotParentCapsules;
	bool bSnapshotUseAttachedParent = false;
	FString SnapshotError;
	int32 SnapshotIterations = 16;
	float SnapshotSettleTime = 0.f;
	float SnapshotSmoothingTime = 0.f;
	bool bHasProfileSnapshot = false;
	FString DebugReason;
	double MaxPenetration = 0;
	double MaxLengthError = 0;
	TArray<FChainTemporalState> TemporalStates;
	float PendingDeltaTime = 0.f;
};
