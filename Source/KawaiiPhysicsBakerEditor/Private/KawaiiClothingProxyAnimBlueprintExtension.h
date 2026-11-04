#pragma once

#include "AnimBlueprintExtension.h"
#include "KawaiiClothingProxyAnimSubsystem.h"
#include "KawaiiClothingProxyAnimBlueprintExtension.generated.h"

/** Declaring both structs lets the standard AnimBP compiler install class/instance subsystem data. */
UCLASS()
class UKawaiiClothingProxyAnimBlueprintExtension : public UAnimBlueprintExtension
{
	GENERATED_BODY()

private:
	UPROPERTY()
	FAnimSubsystem_KawaiiClothingProxy Subsystem;
	UPROPERTY()
	FAnimSubsystemInstance_KawaiiClothingProxy SubsystemInstance;
};
