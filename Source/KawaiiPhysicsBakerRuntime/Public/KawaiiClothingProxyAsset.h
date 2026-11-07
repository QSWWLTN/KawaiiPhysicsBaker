#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "KawaiiClothingProxyAsset.generated.h"

class USkeletalMesh;
struct FReferenceSkeleton;

/** A bone-bound capsule/sphere, or an oriented box attached to StartBone. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKERRUNTIME_API FKawaiiClothingProxyShape
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "代理名称"))
		FName Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "起点骨骼"))
		FName StartBone;
	/** None uses StartBone, including for spheres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "终点骨骼", EditCondition = "!bBox"))
		FName EndBone;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "起点局部偏移", ForceUnits = "cm"))
		FVector StartOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "终点局部偏移", EditCondition = "!bBox", ForceUnits = "cm"))
		FVector EndOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "半径", EditCondition = "!bBox", ClampMin = "0.01", ForceUnits = "cm"))
		float Radius = 3.f;
	/** Boxes use StartBone/StartOffset as their center. EndBone is unused. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "使用盒体"))
		bool bBox = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "盒体旋转", EditCondition = "bBox"))
		FRotator BoxRotation = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Proxy", meta = (DisplayName = "盒体半尺寸", EditCondition = "bBox", ClampMin = "0.01", ForceUnits = "cm"))
		FVector BoxExtent = FVector(3.f);
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
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision Filtering", meta = (DisplayName = "忽略目标骨链",
		ToolTip = "这套衣物代理不碰撞的目标骨链，例如 Tail_Root。预览匹配接收方 Kawaii 节点的主根名，运行时匹配 IK Correction 骨链的根名；精确匹配整个节点或骨链，不是衣物自身的代理绑定骨骼。修改规则无需重新烘焙代理运动。"))
		TArray<FName> IgnoredTargetRootBones;

	/** Deterministic content signature; changing mesh, geometry or bindings invalidates baked motion. */
	FString ComputeSignature() const;
	/** Checks geometry, identifiers and collision rules without loading the soft mesh reference. */
	bool Validate(FString& Error) const;
};

namespace KawaiiClothingProxy {
	constexpr int32 MaxShapes = 64;
	constexpr int32 MaxBones = 4096;
	constexpr int32 MaxKeys = 4000000;
	constexpr int32 MaxIgnoredTargetRoots = 64;

	KAWAIIPHYSICSBAKERRUNTIME_API bool ValidateIgnoredTargetRoots(const TArray<FName>& Roots, FString& Error);
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
