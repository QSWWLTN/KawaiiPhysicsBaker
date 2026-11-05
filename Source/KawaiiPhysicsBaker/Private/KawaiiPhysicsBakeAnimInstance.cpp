#include "KawaiiPhysicsBakeAnimInstance.h"

#include "Animation/AnimSequence.h"

#include "KawaiiPhysicsBakeBlueprint.h"

UKawaiiPhysicsBakeAnimInstance::UKawaiiPhysicsBakeAnimInstance() {
	RootMotionMode = ERootMotionMode::NoRootMotionExtraction;
	SourceState = MakeShared<FKawaiiPhysicsBakeSourceState, ESPMode::ThreadSafe>();
}

void UKawaiiPhysicsBakeAnimInstance::SetBakeSource(UAnimSequence* InAnimation, float InTime, USkeletalMeshComponent* InCopyPoseSource) {
	check(IsInGameThread());
	bUseExplicitSource = true;
	SourceAnimation = InAnimation;
	SourceTime = InTime;
	CopyPoseSource = InCopyPoseSource;
}

void UKawaiiPhysicsBakeAnimInstance::ClearBakeSourceOverride() {
	check(IsInGameThread());
	bUseExplicitSource = false;
	SourceAnimation = nullptr;
	CopyPoseSource = nullptr;
	SourceTime = 0.f;
}

uint64 UKawaiiPhysicsBakeAnimInstance::GetSourceEvaluationSerial() const {
	return static_cast<uint64>(SourceState->EvaluationSerial.GetValue());
}

float UKawaiiPhysicsBakeAnimInstance::GetLastEvaluatedSourceTime() const {
	return SourceState->LastEvaluatedTime.Load();
}

const FString& UKawaiiPhysicsBakeAnimInstance::GetSourceError() const {
	check(IsInGameThread());
	return SourceState->Error;
}

void UKawaiiPhysicsBakeAnimInstance::NativeInitializeAnimation() {
	Super::NativeInitializeAnimation();
	RootMotionMode = ERootMotionMode::NoRootMotionExtraction;
	if (!bUseExplicitSource) {
		const UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Cast<UAnimBlueprint>(GetClass()->ClassGeneratedBy));
		SourceAnimation = Settings ? Settings->SourceAnimation.Get() : nullptr;
		CopyPoseSource = nullptr;
		SourceTime = 0.f;
	}
}

void UKawaiiPhysicsBakeAnimInstance::NativeUpdateAnimation(float DeltaSeconds) {
	Super::NativeUpdateAnimation(DeltaSeconds);
	if (!bUseExplicitSource) {
		const UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Cast<UAnimBlueprint>(GetClass()->ClassGeneratedBy));
		UAnimSequence* PreviewAnimation = Settings ? Settings->SourceAnimation.Get() : nullptr;
		if (SourceAnimation != PreviewAnimation) {
			SourceAnimation = PreviewAnimation;
			SourceTime = 0.f;
		} else if (SourceAnimation && SourceAnimation->GetPlayLength() > UE_SMALL_NUMBER) {
			SourceTime = FMath::Fmod(SourceTime + FMath::Max(0.f, DeltaSeconds), SourceAnimation->GetPlayLength());
		}
	}
}
