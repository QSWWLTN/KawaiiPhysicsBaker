#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "KawaiiClothingProxyAsset.h"

#include "KawaiiClothingProxyMotion.generated.h"

class UAnimSequence;

/** Reduced local-bone tracks for collision only. No AnimSequence decoding or live component reads. */
UCLASS(BlueprintType, meta = (DisplayName = "Kawaii 衣物代理运动"))
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiClothingProxyMotion : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		TObjectPtr<UKawaiiClothingProxyAsset> Definition = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		FString DefinitionSignature;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		TSoftObjectPtr<USkeletalMesh> SourceMesh;
	/** Identity/matching only: this asset is never sampled by the proxy runtime. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		TSoftObjectPtr<UAnimSequence> SourceAnimation;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		TSoftObjectPtr<UAnimSequence> BakedAnimation;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		FGuid BakeGroup;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		FGuid SourceDataGuid;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		FString SourceSkeletonSignature;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		FName PartName;
	/** Recorded bake placement only. Sampling applies its explicit ProxyToTarget argument exactly once. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
		FTransform RelativeTransform = FTransform::Identity;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling")
		int32 SampleRate = 60;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling")
		int32 NumFrames = 0;
	/** Original source duration. Output keys are at i / SampleRate, including the padded last frame. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling")
		float SourceDuration = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tracks")
		TArray<FName> BoneNames;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tracks")
		TArray<int32> ParentIndices;
	/** Frame-major local transforms. Includes every ancestor needed to reconstruct the endpoint bones. */
	UPROPERTY()
		TArray<FTransform> LocalKeys;
	/** Immutable geometry copied at bake time; edits to Definition require a rebake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tracks", meta = (TitleProperty = "Id"))
		TArray<FKawaiiClothingProxyShape> Shapes;

	/** Full validation for authoring/export/tests; examines every transform. */
	bool Validate(FString& Error) const;
	/** Validates metadata and only the accessed keys, without scanning the full clip on each frame. */
	bool SampleLocalPose(float Time, TArray<FTransform>& OutPose, FString& Error) const;
	float GetDuration() const { return SampleRate > 0 && NumFrames > 0 ? static_cast<float>(NumFrames - 1) / SampleRate : 0.f; }

private:
	bool ValidateHeader(FString& Error) const;
};

USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiClothingProxyMotionSample
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
		TObjectPtr<UKawaiiClothingProxyMotion> Motion = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ForceUnits = "s"))
		float Time = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ClampMin = "0"))
		float Weight = 1.f;
};

/** Value-only output suitable for passing into an animation node's exposed input. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiClothingProxyCapsule
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision")
		FName Id;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision")
		FVector Start = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision")
		FVector End = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision")
		float Radius = 0.f;
};

namespace KawaiiClothingProxy {
	/** Game thread. Blend compatible local tracks first, then reconstruct the hierarchy and shapes. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool SampleBlended(const TArray<FKawaiiClothingProxyMotionSample>& Samples,
													 const FTransform& ProxyToTarget,
													 TArray<FKawaiiClothingProxyCapsule>& OutCapsules,
													 FString& Error);
}

UCLASS()
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiClothingProxyLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Full asset validation, including loaded source revisions; use when equipping/loading, not every frame. */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Baked Clothing Proxy")
		static bool ValidateProxyMotion(UKawaiiClothingProxyMotion* Motion, FString& Error);
	/** Call once on the game thread with the same time as clothing display; outputs are in target component space. */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Baked Clothing Proxy", meta = (DisplayName = "Sample Clothing Proxy Motion"))
		static bool SampleProxyMotion(UKawaiiClothingProxyMotion* Motion,
									  float Time,
									  const FTransform& ProxyToTarget,
									  TArray<FKawaiiClothingProxyCapsule>& Capsules,
									  FString& Error);
	/** Weights are normalized. Zero weights are ignored; incompatible positive-weight clips fail atomically. */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Baked Clothing Proxy", meta = (DisplayName = "Sample Blended Clothing Proxy Motion"))
		static bool SampleBlendedProxyMotion(const TArray<FKawaiiClothingProxyMotionSample>& Samples,
											 const FTransform& ProxyToTarget,
											 TArray<FKawaiiClothingProxyCapsule>& Capsules,
											 FString& Error);
};
