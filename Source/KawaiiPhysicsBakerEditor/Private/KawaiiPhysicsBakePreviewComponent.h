#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "KawaiiPhysicsBakeAnimInstance.h"

#include "KawaiiPhysicsBakePreviewComponent.generated.h"

/** Evaluates just Bake Source Pose so garments can copy an already baked body, including its curves. */
UCLASS(Transient, NotBlueprintable)
class UKawaiiBakedBodyPlaybackInstance final : public UKawaiiPhysicsBakeAnimInstance
{
	GENERATED_BODY()
protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
};

/** Displays recorded poses without evaluating the graph or publishing collision snapshots. */
UCLASS(Transient)
class UKawaiiPhysicsBakePreviewComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()
public:
	void DisplayPose(const TArray<FTransform3f>& Pose, const TMap<FName, float>& MaterialCurves, const TMap<FName, float>& MorphCurves);
};
