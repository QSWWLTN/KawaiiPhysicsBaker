#pragma once

#include "CoreMinimal.h"
#include "KawaiiPhysicsCalibrationProfile.h"

struct FAnimNode_KawaiiPhysics;
class UKawaiiPhysicsCalibrationSettings;

namespace KawaiiCalibrationSearch
{
struct FCandidate
{
	FVector4f ProfileParameters = FVector4f(1, 1, 1, 2);
	FKawaiiCalibrationNodeSettings NodeSettings;
	FString Label;
	bool bOriginalBaseline = false;
};

bool Generate(const UKawaiiPhysicsCalibrationSettings& Settings, const FAnimNode_KawaiiPhysics& Node,
	TArray<FCandidate>& OutCandidates, FString& OutError);
void Capture(const FAnimNode_KawaiiPhysics& Node, FKawaiiCalibrationNodeSettings& OutSettings);
void Apply(const FKawaiiCalibrationNodeSettings& Settings, FAnimNode_KawaiiPhysics& OutNode);
double Cost(const FCandidate& Candidate);
}
