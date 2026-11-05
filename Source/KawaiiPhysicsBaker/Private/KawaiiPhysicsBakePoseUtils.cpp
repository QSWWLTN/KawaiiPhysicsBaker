#include "KawaiiPhysicsBakePoseUtils.h"

bool KawaiiPhysicsBake::BuildCopiedLocalPose(const FReferenceSkeleton& SourceSkeleton, const TArray<FTransform>& SourceCS, const FReferenceSkeleton& TargetSkeleton, const FTransform& SourceToTarget, TArray<FTransform>& OutLocalPose, FString& OutError) {
	OutError.Reset();
	OutLocalPose = TargetSkeleton.GetRefBonePose();
	if (SourceCS.Num() != SourceSkeleton.GetNum()) {
		OutError = TEXT("Copy Pose From has no complete evaluated pose. Tick and finalize the source component first.");
		return false;
	}
	TArray<FTransform> TargetCS;
	TargetCS.SetNumUninitialized(TargetSkeleton.GetNum());
	int32 MatchingBones = 0;
	for (int32 TargetIndex = 0; TargetIndex < TargetSkeleton.GetNum(); ++TargetIndex) {
		const int32 TargetParent = TargetSkeleton.GetParentIndex(TargetIndex);
		const int32 SourceIndex = SourceSkeleton.FindBoneIndex(TargetSkeleton.GetBoneName(TargetIndex));
		if (SourceCS.IsValidIndex(SourceIndex)) {
			TargetCS[TargetIndex] = SourceCS[SourceIndex] * SourceToTarget;
			OutLocalPose[TargetIndex] = TargetParent != INDEX_NONE ? TargetCS[TargetIndex].GetRelativeTransform(TargetCS[TargetParent]) : TargetCS[TargetIndex];
			++MatchingBones;
		} else {
			TargetCS[TargetIndex] = TargetParent != INDEX_NONE ? OutLocalPose[TargetIndex] * TargetCS[TargetParent] : OutLocalPose[TargetIndex];
		}
	}
	if (MatchingBones == 0) {
		OutError = TEXT("Copy Pose From and the target mesh have no shared bone names.");
		return false;
	}
	return true;
}
