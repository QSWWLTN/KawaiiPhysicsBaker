#pragma once

#include "CoreMinimal.h"

#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "KawaiiClothingProxySnapshot.h"

class USkeletalMeshComponent;

/** Session-owned CPU preview of the runtime solver. Inputs remain the uncorrected bake poses. */
class FKawaiiPhysicsBakePreviewCorrection
{
public:
	bool Initialize(const UKawaiiClothingProxyAsset& Definition,
					const USkeletalMeshComponent& Source,
					UKawaiiPhysicsIKCorrectionProfile* Profile,
					int32 SampleRate,
					FString& OutError);
	bool Evaluate(USkeletalMeshComponent& Target,
				  const USkeletalMeshComponent& Source,
				  float DeltaTime,
				  float Alpha,
				  TArray<FTransform3f>& OutPose,
				  FString& OutError);

private:
	FAnimNode_KawaiiPhysicsIKCorrection Correction;
	KawaiiClothingProxy::FSnapshot Snapshot;
	TArray<int32> SourceBones;
};
