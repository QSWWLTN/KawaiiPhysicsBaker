#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "KawaiiPhysicsBakeAnimInstance.h"
#include "AnimNode_KawaiiPhysicsBakeSource.generated.h"

/** A controlled sequence/copy-pose source. All asset/component access happens during PreUpdate. */
USTRUCT(BlueprintInternalUseOnly)
struct KAWAIIPHYSICSBAKER_API FAnimNode_KawaiiPhysicsBakeSource : public FAnimNode_Base
{
	GENERATED_BODY()

	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;

private:
	TArray<FTransform> MeshLocalPose;
	FBlendedHeapCurve SourceCurves;
	TSharedPtr<FKawaiiPhysicsBakeSourceState, ESPMode::ThreadSafe> SourceState;
	float SnapshotTime = 0.f;
	bool bSnapshotValid = false;
};
