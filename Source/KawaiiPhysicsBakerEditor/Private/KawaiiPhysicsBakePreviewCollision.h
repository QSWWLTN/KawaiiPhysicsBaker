#pragma once

#include "CoreMinimal.h"

class UKawaiiClothingProxyAsset;
class UKawaiiPhysicsClothingCollisionProfile;

namespace KawaiiPhysicsBakePreviewCollision {
	/** Uses the definition's target exclusions in preview too. Failure clears shapes and consumers. */
	bool BuildProfile(const UKawaiiClothingProxyAsset& Definition,
					  const TArray<FName>& AvailableRoots,
					  const TArray<FName>& RequestedRoots,
					  UKawaiiPhysicsClothingCollisionProfile& OutProfile,
					  FString& OutError);
}
