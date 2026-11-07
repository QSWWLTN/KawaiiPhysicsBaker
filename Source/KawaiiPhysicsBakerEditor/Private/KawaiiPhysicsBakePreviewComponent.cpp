#include "KawaiiPhysicsBakePreviewComponent.h"
#include "Animation/AnimInstanceProxy.h"
#include "AnimNode_KawaiiPhysicsBakeSource.h"

namespace {
	class FBakedBodyPlaybackProxy final : public FAnimInstanceProxy
	{
	public:
		explicit FBakedBodyPlaybackProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}
		virtual FAnimNode_Base* GetCustomRootNode() override { return &Source; }
		virtual void GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes) override { OutNodes.Add(&Source); }
	private:
		FAnimNode_KawaiiPhysicsBakeSource Source;
	};
}

FAnimInstanceProxy* UKawaiiBakedBodyPlaybackInstance::CreateAnimInstanceProxy() {
	return new FBakedBodyPlaybackProxy(this);
}

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
