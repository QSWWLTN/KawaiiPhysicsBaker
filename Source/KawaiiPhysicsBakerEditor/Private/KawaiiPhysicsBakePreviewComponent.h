#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"

#include "KawaiiPhysicsBakePreviewComponent.generated.h"

/** Displays recorded poses without evaluating the graph or publishing collision snapshots. */
UCLASS(Transient)
class UKawaiiPhysicsBakePreviewComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()
public:
	void DisplayPose(const TArray<FTransform3f>& Pose, const TMap<FName, float>& MaterialCurves, const TMap<FName, float>& MorphCurves);
};
