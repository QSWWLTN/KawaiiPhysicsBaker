#pragma once
#include "CoreMinimal.h"
class UKawaiiClothingProxyAsset;
class USkeletalMeshComponent;
namespace KawaiiPhysicsBakeBodyCollision {
	/** Use the completed Main pose in the same bake step; does not require matching target bone names. */
	bool Update(const UKawaiiClothingProxyAsset& Definition, const USkeletalMeshComponent& Body, USkeletalMeshComponent& Garment, FString& Error);
}
