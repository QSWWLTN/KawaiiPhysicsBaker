// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimBlueprint.h"
#include "Blueprint/BlueprintExtension.h"
#include "Engine/EngineTypes.h"

#include "KawaiiPhysicsBakeBlueprint.generated.h"

class UKawaiiPhysicsClothingCollisionProfile;
class UKawaiiPhysicsIKCorrectionProfile;
class UKawaiiClothingProxyAsset;
class UKawaiiClothingProxyMotionSet;
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

	UPROPERTY(EditAnywhere, Category = "初次烘焙", meta = (DisplayName = "接收身体代理碰撞", ToolTip = "使用 Main 的代理定义，在每步身体求值完成后推动本部件的 Kawaii 骨链，写入初次衣物动画。需要设置主部件代理定义。手动修整后的动画回读不会再次运行物理。"))
		bool bCollideWithMainProxy = false;

	/** Optional compact proxy motion is recorded from the same final pose as this part's animation. */
	UPROPERTY(EditAnywhere,
			  Category = "代理运动",
			  meta = (DisplayName = "衣物代理定义",
					  ToolTip = "选择为此网格体创建的衣物代理。烘焙时导出代理运动；开启“预览运行时代理修正”后，也用于同帧修正预览。单独指定本资产不会开启修正。"))
		TObjectPtr<UKawaiiClothingProxyAsset> ProxyDefinition = nullptr;

	UPROPERTY(EditAnywhere,
			  Category = "代理运动",
			  meta = (DisplayName = "代理动作集合",
					  ToolTip = "可选。烘焙后将本次源动作的代理运动自动写入该集合，替换相同源动作的映射。未指定时创建新集合。需要同时指定衣物代理定义。"))
		TObjectPtr<UKawaiiClothingProxyMotionSet> ProxyMotionSet = nullptr;
};

/** Settings belong to the graph asset; source replacement is an explicit export option. */
UCLASS(EditInlineNew, DefaultToInstanced)
class KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings : public UBlueprintExtension
{
	GENERATED_BODY()

public:
	UKawaiiPhysicsBakeSettings();

	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "主骨骼网格体", ToolTip = "主角色的骨骼网格体，其部件名称为 Main。"))
		TObjectPtr<USkeletalMesh> MainMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "源动画", EditCondition = "ExistingBakedBodyAnimation == nullptr", ToolTip = "与主骨骼网格体使用相同骨架的非叠加动画。"))
		TObjectPtr<UAnimSequence> SourceAnimation = nullptr;

	UPROPERTY(EditAnywhere, Category = "源动画", meta = (DisplayName = "已烘焙人物动画", ToolTip = "追加烘焙衣物时选择。Main 直接播放此序列，衣物按它的时间轴烘焙，导出的衣物代理也引用它。留空时使用源动画烘焙 Main。"))
		TObjectPtr<UAnimSequence> ExistingBakedBodyAnimation = nullptr;

	UPROPERTY(EditAnywhere,
			  Category = "代理运动",
			  meta = (DisplayName = "主部件代理定义（身体）",
					  ToolTip = "为 Main 导出代理运动；附加部件开启“接收身体代理碰撞”后，也在初次衣物烘焙中作为身体碰撞体。衣物对头发的代理仍在对应附加部件中指定。"))
		TObjectPtr<UKawaiiClothingProxyAsset> ProxyDefinition = nullptr;

	UPROPERTY(EditAnywhere,
			  Category = "代理运动",
			  meta = (DisplayName = "主部件代理动作集合", EditCondition = "ExistingBakedBodyAnimation == nullptr",
					  ToolTip = "可选。烘焙后自动更新 Main 的源动作映射；未指定时创建新集合。需要同时指定主部件衣物代理定义。"))
		TObjectPtr<UKawaiiClothingProxyMotionSet> ProxyMotionSet = nullptr;

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

	UPROPERTY(EditAnywhere, Category = "输出", meta = (DisplayName = "烘焙后替换源动画（保持名称和引用）", EditCondition = "ExistingBakedBodyAnimation == nullptr",
		ToolTip = "默认关闭。开启后将 Main 烘焙结果写回源动画，代理的身体动画也直接引用该源动画，不再额外生成 Main 动画资产；保留原资产路径、名称、Notify、同步标记和 Root Motion 设置，现有蒙太奇及动画蓝图引用无需重接。覆盖前在输出目录保存 SourceBackup 备份。附加衣物仍单独导出；此选项不会替换部件动画。"))
		bool bReplaceSourceAnimation = false;

	/** Ordered dependencies: a part can only copy or collide against Main or an earlier part. */
	UPROPERTY(EditAnywhere, Category = "附加部件", meta = (DisplayName = "附加部件", ToolTip = "身体、衣物等独立求值的部件，按列表顺序模拟；姿态来源只能引用更早的部件。", TitleProperty = "Name"))
		TArray<FKawaiiPhysicsBakePart> Parts;

	/** Preview uses the runtime correction after every part has produced its uncorrected bake pose. */
	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "预览运行时代理修正（仅预览）",
		ToolTip = "使用运行时相同的代理采样与 IK 修正器，在本帧衣物求值完成后修正 Main。请选择与 Automatic Clothing Proxy Correction 节点相同的修正配置。预览在 CPU 上执行；运行时仍可切换 GPU。修正不写入烘焙，也不反馈到原生物理模拟。"))
		bool bPreviewClothingProxyCollision = false;

	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "运行时修正配置", EditCondition = "bPreviewClothingProxyCollision",
		ToolTip = "与角色 Automatic Clothing Proxy Correction 节点共用的 Profile，包含目标骨链、身体胶囊、迭代和时间平滑参数。预览不再从 Kawaii 节点猜测另一套碰撞设置。修改配置后重新编译并预览。"))
		TObjectPtr<UKawaiiPhysicsIKCorrectionProfile> PreviewCorrectionProfile = nullptr;

	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "运行时修正权重", EditCondition = "bPreviewClothingProxyCollision", ClampMin = "0", ClampMax = "1"))
		float PreviewCorrectionAlpha = 1.f;

	/** Retained for loading older settings. Runtime chain selection now belongs to PreviewCorrectionProfile. */
	UPROPERTY()
		TArray<FName> PreviewCollisionRootBones;

	/** Legacy bridge is independent of the preview-only option and affects exported tracks. */
	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "启用旧配置碰撞（写入烘焙）", EditCondition = "ExistingBakedBodyAnimation == nullptr", ToolTip = "使用旧碰撞配置，影响预览和导出动画。若同时预览运行时修正，会在相同的烘焙基础姿态上追加修正。需要有效配置、来源部件和至少 2 个预热帧。"))
		bool bEnableClothingCollision = false;

	/** The name of the part whose final pose supplies the Main mesh's clothing proxy. */
	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "碰撞来源部件", ToolTip = "为 Main 提供碰撞的附加部件名称。新代理预览需要该部件的衣物代理定义；不能为 Main。", EditCondition = "bEnableClothingCollision || bPreviewClothingProxyCollision"))
		FName CollisionSourcePart = TEXT("Clothing");

	UPROPERTY(EditAnywhere, Category = "衣物碰撞", meta = (DisplayName = "衣物碰撞配置", ToolTip = "定义衣物碰撞代理和目标骨链的配置资产。", EditCondition = "bEnableClothingCollision"))
		TObjectPtr<UKawaiiPhysicsClothingCollisionProfile> ClothingCollisionProfile = nullptr;
};

/** Use Unreal's native AnimBlueprint/compiler; persist bake settings as an owned extension. */
namespace KawaiiPhysicsBake {
	KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings* GetSettings(const UAnimBlueprint* Blueprint);
	KAWAIIPHYSICSBAKER_API UKawaiiPhysicsBakeSettings* FindOrAddSettings(UAnimBlueprint* Blueprint);
	KAWAIIPHYSICSBAKER_API bool IsBakeBlueprint(const UAnimBlueprint* Blueprint);
}
