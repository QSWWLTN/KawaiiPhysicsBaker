// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimBlueprint.h"
#include "Blueprint/BlueprintExtension.h"
#include "Engine/EngineTypes.h"

#include "KawaiiPhysicsBakeBlueprint.generated.h"

class UKawaiiPhysicsClothingCollisionProfile;
class UAnimSequence;
class USkeletalMesh;

/** A second independently evaluated mesh in the same deterministic bake session. */
USTRUCT(BlueprintType)
struct KAWAIIPHYSICSBAKER_API FKawaiiPhysicsBakePart
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "部件", meta = (DisplayName = "部件名称", ToolTip = "部件的唯一名称，不能为空或重复；Main 为主部件保留名称。"))
		FName Name;

	UPROPERTY(EditAnywhere, Category = "部件", meta = (DisplayName = "骨骼网格体", ToolTip = "该部件使用的骨骼网格体。"))
		TObjectPtr<USkeletalMesh> Mesh = nullptr;

	/** The graph is evaluated on this mesh after all preceding parts. */
	UPROPERTY(EditAnywhere, Category = "部件", meta = (DisplayName = "烘焙动画蓝图", ToolTip = "该网格体对应的烘焙动画蓝图，在列表中排在它前面的部件求值完成后执行。"))
		TObjectPtr<UAnimBlueprint> Graph = nullptr;

	/** Leave empty to copy the completed pose of Copy Pose From, matching bone names. */
	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "部件动画", ToolTip = "留空时从姿态来源复制动作；指定动画时需与主动画时长相同，并使用本部件的骨架。"))
		TObjectPtr<UAnimSequence> Animation = nullptr;

	/** Main, or the unique name of an earlier part. */
	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "姿态来源", ToolTip = "部件动画留空时生效。填写 Main 或列表中更早的部件名称，按骨骼名称匹配姿态。", EditCondition = "Animation == nullptr"))
		FName CopyPoseFrom = TEXT("Main");

	UPROPERTY(EditAnywhere, Category = "部件", meta = (DisplayName = "相对变换", ToolTip = "该部件在预览场景中的相对位置、旋转和缩放。"))
		FTransform RelativeTransform = FTransform::Identity;
};

/** Settings belong to the graph asset; baking never edits the source animation. */
UCLASS(EditInlineNew, DefaultToInstanced)
class KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings : public UBlueprintExtension
{
	GENERATED_BODY()

public:
	UKawaiiPhysicsBakeSettings();

	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "主骨骼网格体", ToolTip = "主角色的骨骼网格体，其部件名称为 Main。"))
		TObjectPtr<USkeletalMesh> MainMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "源动画", ToolTip = "与主骨骼网格体使用相同骨架的非叠加动画。"))
		TObjectPtr<UAnimSequence> SourceAnimation = nullptr;

	UPROPERTY(EditAnywhere, Category = "采样", meta = (DisplayName = "采样帧率", ToolTip = "固定物理模拟和动画输出帧率，范围为 1–240。", ClampMin = "1", ClampMax = "240"))
		int32 SampleRate = 60;

	/** Fixed simulation steps held at source time zero before the first recorded frame. */
	UPROPERTY(EditAnywhere, Category = "采样", meta = (DisplayName = "预热帧数", ToolTip = "正式采样前保持动画起始姿态进行物理预热；预热帧不写入输出动画。", ClampMin = "0", ClampMax = "3600"))
		int32 WarmUpFrames = 60;

	/** Long package directory, for example /Game/KawaiiBakes. */
	UPROPERTY(EditAnywhere, Category = "输出", meta = (DisplayName = "输出目录", ToolTip = "Unreal 内容目录，例如 /Game/KawaiiBakes，不能填写磁盘路径。"))
		FDirectoryPath OutputDirectory;

	UPROPERTY(EditAnywhere, Category = "输出", meta = (DisplayName = "输出名称前缀", ToolTip = "新动画资产的名称前缀，必须是合法且非空的资产名称。"))
		FString OutputPrefix = TEXT("Baked");

	/** Ordered dependencies: a part can only copy or collide against Main or an earlier part. */
	UPROPERTY(EditAnywhere, Category = "附加部件", meta = (DisplayName = "附加部件", ToolTip = "身体、衣物等独立求值的部件，按列表顺序模拟；姿态来源只能引用更早的部件。", TitleProperty = "Name"))
		TArray<FKawaiiPhysicsBakePart> Parts;

	/** Enable the project's clothing collision bridge, with Main as the receiving mesh. */
	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "启用衣物碰撞", ToolTip = "为主部件启用衣物碰撞代理，需要有效配置、来源部件和至少 2 个预热帧。"))
		bool bEnableClothingCollision = false;

	/** The name of the part whose final pose supplies the Main mesh's clothing proxy. */
	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "碰撞来源部件", ToolTip = "为主部件提供衣物碰撞姿态的部件名称，不能为 Main。", EditCondition = "bEnableClothingCollision"))
		FName CollisionSourcePart = TEXT("Clothing");

	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "衣物碰撞配置", ToolTip = "定义衣物碰撞代理和目标骨链的配置资产。", EditCondition = "bEnableClothingCollision"))
		TObjectPtr<UKawaiiPhysicsClothingCollisionProfile> ClothingCollisionProfile = nullptr;
};

/** Use Unreal's native AnimBlueprint/compiler; persist bake settings as an owned extension. */
namespace KawaiiPhysicsBake
{
	KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings* GetSettings(const UAnimBlueprint* Blueprint);
	KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings* FindOrAddSettings(UAnimBlueprint* Blueprint);
	KAWAIIPHYSICSBAKER_API bool IsBakeBlueprint(const UAnimBlueprint* Blueprint);
}
