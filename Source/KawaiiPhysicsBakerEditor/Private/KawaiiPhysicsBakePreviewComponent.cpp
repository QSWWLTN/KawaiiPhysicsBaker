#include "KawaiiPhysicsBakePreviewComponent.h"

void UKawaiiPhysicsBakePreviewComponent::DisplayPose(const TArray<FTransform3f>& Pose, const TMap<FName, float>& MaterialCurves, const TMap<FName, float>& MorphCurves) {
	TArray<FTransform>& Editable = GetEditableComponentSpaceTransforms();
	Editable.SetNum(Pose.Num());
	for (int32 Bone = 0; Bone < Pose.Num(); ++Bone) {
		Editable[Bone] = FTransform(Pose[Bone]);
	}

	// Deliberately skip USkeletalMeshComponent's finalized-bone delegates:
	// scrubbing must not replace the live solver's clothing collision history.
	bNeedToFlipSpaceBaseBuffers = true;
	USkinnedMeshComponent::FinalizeBoneTransform();
	ApplyAnimationCurvesToComponent(&MaterialCurves, &MorphCurves);
	InvalidateCachedBounds();
	UpdateBounds();
	MarkRenderTransformDirty();
	MarkRenderDynamicDataDirty();
}
