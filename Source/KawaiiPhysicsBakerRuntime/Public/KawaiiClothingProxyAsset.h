#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "KawaiiClothingProxyAsset.generated.h"

class USkeletalMesh;
struct FReferenceSkeleton;

/** A capsule bound to two clothing bones. Coincident endpoints form a sphere. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiClothingProxyShape
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "代理名称"))
		FName Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "起点骨骼"))
		FName StartBone;
	/** None uses StartBone, including for spheres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "终点骨骼"))
		FName EndBone;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "起点局部偏移", ForceUnits = "cm"))
		FVector StartOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "终点局部偏移", ForceUnits = "cm"))
		FVector EndOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "半径", ClampMin = "0.01", ForceUnits = "cm"))
		float Radius = 3.f;
};

/** One reusable collision definition per garment, independent of animation clips. */
UCLASS(BlueprintType, meta = (DisplayName = "Kawaii 衣物代理"))
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiClothingProxyAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Proxy", meta = (DisplayName = "衣物网格体"))
		TSoftObjectPtr<USkeletalMesh> PreviewMesh;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Proxy", meta = (DisplayName = "碰撞代理", TitleProperty = "Id"))
		TArray<FKawaiiClothingProxyShape> Shapes;

	/** Deterministic content signature; changing mesh, geometry or bindings invalidates baked motion. */
	FString ComputeSignature() const;
	/** Checks geometry and identifiers without loading the soft mesh reference. */
	bool Validate(FString& Error) const;
};

namespace KawaiiClothingProxy {
	constexpr int32 MaxShapes = 64;
	constexpr int32 MaxBones = 4096;
	constexpr int32 MaxKeys = 4000000;

	KAWAIIPHYSICSBAKERRUNTIME_API bool ValidateShapes(const TArray<FKawaiiClothingProxyShape>& Shapes, FString& Error);
	KAWAIIPHYSICSBAKERRUNTIME_API FString ComputeDefinitionSignature(const FSoftObjectPath& Mesh, const TArray<FKawaiiClothingProxyShape>& Shapes);
	KAWAIIPHYSICSBAKERRUNTIME_API FString ComputeSkeletonSignature(const FReferenceSkeleton& Skeleton);
	/** All endpoint ancestors are retained once, in reference-skeleton order. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool BuildMinimalHierarchy(const FReferenceSkeleton& Skeleton,
															 const TArray<FKawaiiClothingProxyShape>& Shapes,
															 TArray<FName>& OutNames,
															 TArray<int32>& OutParents,
															 TArray<int32>& OutMeshIndices,
															 FString& Error);
}
