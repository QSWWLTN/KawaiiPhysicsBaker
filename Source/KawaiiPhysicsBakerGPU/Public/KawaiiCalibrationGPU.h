#pragma once

#include "CoreMinimal.h"

/** Editor-only screening model: independent candidates, temporally sequential frames. */
struct FKawaiiCalibrationGPUInput
{
	// Parent index, damping, stiffness, particle radius.
	TArray<FVector4f> Bones;
	// Frame-major particle targets.
	TArray<FVector4f> Poses;
	// Optional frame-major plane normal / angle (degrees); w < 0 means inherit screening behavior.
	TArray<FVector4f> Ranges;
	// Frame-major pairs: start/radius, end.
	TArray<FVector4f> Capsules;
	// Reset state, include in score, unused, unused.
	TArray<FVector4f> Frames;
	// Damping scale, stiffness scale, radius scale, contact iterations.
	TArray<FVector4f> Candidates;
	FVector3f Gravity = FVector3f(0, 0, -980);
	uint32 CapsuleCount = 0;
	float DeltaTime = 1.f / 60.f;
	float SoftMargin = .5f;
	float SoftResponse = 12.f;
};

class KAWAIIPHYSICSBAKERGPU_API FKawaiiCalibrationGPU : public TSharedFromThis<FKawaiiCalibrationGPU, ESPMode::ThreadSafe>
{
public:
	static TSharedPtr<FKawaiiCalibrationGPU, ESPMode::ThreadSafe> Start(const FKawaiiCalibrationGPUInput& Input, FString& OutError);
	~FKawaiiCalibrationGPU();
	/** Nonblocking; schedules bounded simulation chunks and polls asynchronous readback. */
	void Tick();
	bool IsDone() const;
	float GetProgress() const;
	/** Squared penetration sum, maximum penetration, squared deviation sum, squared displacement-change sum. */
	const TArray<FVector4f>& GetScores() const;
	const FString& GetError() const;

private:
	struct FState;
	FKawaiiCalibrationGPU();
	TSharedPtr<FState, ESPMode::ThreadSafe> State;
};
