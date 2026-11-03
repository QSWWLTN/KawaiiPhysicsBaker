#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "KawaiiClothingProxyMotion.h"

#include "KawaiiClothingProxyPlayback.generated.h"

class UAnimSequence;

/** One garment's source-action to baked proxy-motion mapping, maintained by the baker. */
UCLASS(BlueprintType, meta = (DisplayName = "Kawaii 衣物代理动作集合"))
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiClothingProxyMotionSet : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "衣物代理", meta = (DisplayName = "代理运动资产"))
		TArray<TObjectPtr<UKawaiiClothingProxyMotion>> Motions;
};

/** Published by the body action player AFTER resolving montage segments/state blending.
 * Time is sequence-local seconds, not montage position or time since entering a state. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiClothingProxyActionSample
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
		TObjectPtr<UAnimSequence> SourceAnimation = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
		float Time = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation", meta = (ClampMin = "0"))
		float Weight = 1.f;
};

UCLASS()
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiClothingProxyPlaybackLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Resolve once on the game thread for each body action snapshot. Missing/duplicate mappings fail
	 * atomically and clear outputs. The returned BakedAnimation references identify the matching garment poses.
	 * This does not infer source animation identities from an already blended body pose. */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Baked Clothing Proxy", meta = (DisplayName = "Resolve Clothing Proxy Actions"))
		static bool ResolveProxyMotionSamples(const UKawaiiClothingProxyMotionSet* MotionSet,
											  const TArray<FKawaiiClothingProxyActionSample>& Actions,
											  TArray<FKawaiiClothingProxyMotionSample>& OutSamples,
											  FString& Error);
};
