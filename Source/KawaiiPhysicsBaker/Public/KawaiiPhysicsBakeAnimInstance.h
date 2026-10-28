#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "HAL/ThreadSafeCounter64.h"
#include "KawaiiPhysicsBakeAnimInstance.generated.h"

class UAnimSequence;
class USkeletalMeshComponent;
struct FAnimNode_KawaiiPhysicsBakeSource;

/** The only data shared back from worker evaluation is atomic. Error text is game-thread-only. */
struct FKawaiiPhysicsBakeSourceState
{
	FThreadSafeCounter64 EvaluationSerial;
	TAtomic<float> LastEvaluatedTime{0.f};
	FString Error;
};

/** Native parent for bake graphs. No root extraction or gameplay notification dispatch is required. */
UCLASS(Transient, Blueprintable)
class KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UKawaiiPhysicsBakeAnimInstance();

	/** Call on the game thread before ticking the component. Explicit time does not advance itself. */
	void SetBakeSource(UAnimSequence* InAnimation, float InTime, USkeletalMeshComponent* InCopyPoseSource = nullptr);
	void ClearBakeSourceOverride();

	uint64 GetSourceEvaluationSerial() const;
	float GetLastEvaluatedSourceTime() const;
	const FString& GetSourceError() const;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Bake Source")
		TObjectPtr<UAnimSequence> SourceAnimation = nullptr;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Bake Source")
		float SourceTime = 0.f;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Bake Source")
		TObjectPtr<USkeletalMeshComponent> CopyPoseSource = nullptr;

	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

private:
	friend struct FAnimNode_KawaiiPhysicsBakeSource;
	bool bUseExplicitSource = false;
	TSharedPtr<FKawaiiPhysicsBakeSourceState, ESPMode::ThreadSafe> SourceState;
};
