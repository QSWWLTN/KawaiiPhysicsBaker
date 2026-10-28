#pragma once

#include "CoreMinimal.h"
#include "ReferenceSkeleton.h"

namespace KawaiiPhysicsBake
{
	/** Copy named bones in component space, then derive local transforms in the target hierarchy. */
	bool BuildCopiedLocalPose(const FReferenceSkeleton& SourceSkeleton,
							  const TArray<FTransform>& SourceCS,
							  const FReferenceSkeleton& TargetSkeleton,
							  const FTransform& SourceToTarget,
							  TArray<FTransform>& OutLocalPose,
							  FString& OutError);
}
