#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"

#include "AnimNode_KawaiiPhysics.h"

#include "KawaiiCalibrationAnimInstance.generated.h"

/** Transient native animation graph for replaying sampled poses through the real CPU Kawaii node. */
UCLASS(Transient)
class UKawaiiCalibrationAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	UKawaiiCalibrationAnimInstance();
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override;

public:
	int32 GetSimulatedBoneCount() const;
	/** Counts actual source-pose evaluations, including multiple fixed samples in one engine frame. */
	uint64 GetSourceEvaluationSerial() const;

public:
	UPROPERTY(Transient)
		FAnimNode_KawaiiPhysics PhysicsTemplate;
	TArray<FTransform> InputPose;
	bool bEnablePhysics = false;
};
