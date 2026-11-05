#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimSubsystem.h"
#include "Animation/AnimSubsystemInstance.h"

#include "KawaiiClothingProxyActionTracking.h"

#include "KawaiiClothingProxyAnimSubsystem.generated.h"

/** Per-AnimInstance owned values, produced on the game thread before graph evaluation. */
USTRUCT()
struct KAWAIIPHYSICSBAKERRUNTIME_API FAnimSubsystemInstance_KawaiiClothingProxy : public FAnimSubsystemInstance
{
	GENERATED_BODY()
	KawaiiClothingProxy::FTrackedMontageSnapshot MontageSnapshot;
};

/** Automatically installed by the proxy graph node's compiler extension. */
USTRUCT()
struct KAWAIIPHYSICSBAKERRUNTIME_API FAnimSubsystem_KawaiiClothingProxy : public FAnimSubsystem
{
	GENERATED_BODY()
	virtual void OnPreUpdate_GameThread(FAnimSubsystemUpdateContext& Context) const override;
};
