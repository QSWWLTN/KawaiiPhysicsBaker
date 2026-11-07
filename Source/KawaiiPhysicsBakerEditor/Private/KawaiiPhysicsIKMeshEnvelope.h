#pragma once
#include "CoreMinimal.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
class USkeletalMesh;
namespace KawaiiPhysicsIKMeshEnvelope {
	/** Conservative bind-pose mesh distance from each corrected chain, in mesh component units. */
	bool Measure(const USkeletalMesh& Mesh, const TArray<FKawaiiPhysicsIKCorrectionChain>& Chains,
		TArray<float>& OutRadii, FString& Error);
}
