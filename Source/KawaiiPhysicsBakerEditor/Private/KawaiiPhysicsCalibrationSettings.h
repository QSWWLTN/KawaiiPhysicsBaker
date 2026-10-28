#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "KawaiiPhysicsClothingCollisionProfile.h"
#include "KawaiiPhysicsCalibrationProfile.h"

#include "KawaiiPhysicsCalibrationSettings.generated.h"

class UAnimSequence;
class UAnimBlueprint;
class USkeletalMesh;
class UAnimGraphNode_KawaiiPhysics;

USTRUCT(meta = (DisplayName = "可编辑身体碰撞形状"))
struct FKawaiiCalibrationCollisionShape {
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "启用"))
		bool bEnabled = true;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "人物骨骼", GetOptions = "GetBodyBoneOptions"))
		FName Bone;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "骨骼局部中心（厘米）"))
		FVector Center = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "骨骼局部旋转（度）", ToolTip = "胶囊沿旋转后的局部 Z 轴延伸。球体无需调整旋转。"))
		FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "半径（厘米）", ClampMin = "0.01"))
		float Radius = 3.f;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "轴段长度（厘米，0 为球体）", ClampMin = "0", ToolTip = "两端半球中心间的距离，不包含半球；胶囊总长为轴段长度加两倍半径。"))
		float Length = 0.f;
	UPROPERTY(VisibleAnywhere, Category = "碰撞", meta = (DisplayName = "读取来源"))
		FString Source;
};

USTRUCT(meta = (DisplayName = "校准测试动作"))
struct FKawaiiCalibrationMotion
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = "动作", meta = (DisplayName = "动画序列", ToolTip = "用于测试的人物动画序列，必须匹配人物骨架，且不能是叠加动画。"))
		TObjectPtr<UAnimSequence> Animation;
	UPROPERTY(EditAnywhere, Category = "动作", meta = (ClampMin = "0.25", ClampMax = "3", DisplayName = "播放倍率", ToolTip = "测试时的动画播放速度倍率。"))
		float PlayRate = 1.f;
	/** 播放此片段前重置并预热物理；关闭时延续上一片段的模拟状态。 */
	UPROPERTY(EditAnywhere, Category = "动作", meta = (DisplayName = "播放前重置", ToolTip = "播放此片段前重置并预热物理。关闭时延续上一片段的物理状态，用于测试动画切换。"))
		bool bResetBefore = false;
	UPROPERTY(EditAnywhere, Category = "动作", meta = (ClampMin = "0", ClampMax = "2", DisplayName = "过渡时长（秒）", ToolTip = "未重置物理时，从上一片段末尾姿势混合到当前动画的时长。"))
		float BlendInSeconds = .2f;
	/** 不参与 GPU 筛选，最终 CPU 验证时要求这些片段的结果不劣于基准。 */
	UPROPERTY(EditAnywhere, Category = "动作", meta = (DisplayName = "仅用于验证", ToolTip = "不参与 GPU 候选筛选，仅在最终 CPU 验证中检查。建议使用独立片段，并开启播放前重置。"))
		bool bValidationOnly = false;
	/** 衣物跟随前，对指定人物骨骼施加可选的模拟 IK 偏移。 */
	UPROPERTY(EditAnywhere, Category = "IK 压力测试", meta = (DisplayName = "压力测试骨骼", ToolTip = "在人物姿势上施加额外局部偏移，模拟 IK 带来的姿势变化；留空可关闭。"))
		FName OffsetBone;
	UPROPERTY(EditAnywhere, Category = "IK 压力测试", meta = (DisplayName = "局部位置偏移", ToolTip = "压力测试骨骼相对父骨骼的额外位置偏移，单位为厘米。"))
		FVector LocalOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "IK 压力测试", meta = (DisplayName = "局部旋转偏移", ToolTip = "压力测试骨骼在父骨骼空间中的额外旋转，单位为度。"))
		FRotator LocalRotation = FRotator::ZeroRotator;
};

/** 仅用于编辑器的校准测试集；输出配置不引用测试集或测试动画。 */
UCLASS(meta = (DisplayName = "Kawaii 物理校准测试集"))
class UKawaiiPhysicsCalibrationSettings : public UDataAsset
{
	GENERATED_BODY()
public:
	UKawaiiPhysicsCalibrationSettings() { OutputDirectory.Path = TEXT("/Game/KawaiiCalibration"); }

	virtual bool IsEditorOnly() const override { return true; }

public:
	UFUNCTION()
		TArray<FString> GetPhysicsRootOptions() const;
	UFUNCTION()
		TArray<FString> GetBodyBoneOptions() const;
	UFUNCTION()
		TArray<FString> GetChainRootOptions() const;
	UAnimGraphNode_KawaiiPhysics* FindPhysicsNode() const;
	bool ReadCollisionAndChains(FString& OutMessage);
	bool ResolveConstraints(FString& OutSource, FString& OutError);
	void CopyConstraintsToProfile(UKawaiiPhysicsCalibrationProfile& Profile) const;

public:
	UPROPERTY(EditAnywhere, Category = "角色", meta = (DisplayName = "人物骨骼网格体", ToolTip = "播放测试动作的人物网格体，也是身体碰撞胶囊的骨骼来源。"))
		TObjectPtr<USkeletalMesh> BodyMesh;
	UPROPERTY(EditAnywhere, Category = "角色", meta = (DisplayName = "衣物骨骼网格体", ToolTip = "需要校准 Kawaii 物理参数的衣物网格体。"))
		TObjectPtr<USkeletalMesh> ClothingMesh;
	UPROPERTY(EditAnywhere, Category = "角色", meta = (DisplayName = "衣物相对变换", ToolTip = "衣物相对于人物组件的位置、旋转和缩放，应与实际挂接关系一致。"))
		FTransform ClothingTransform = FTransform::Identity;
	/** 读取一个 Kawaii 节点的原始设置，校准时不执行或修改该蓝图。 */
	UPROPERTY(EditAnywhere, Category = "角色", meta = (DisplayName = "衣物动画蓝图", ToolTip = "读取其中一个 Kawaii 节点的原始配置。校准过程中不执行或修改此蓝图；已绑定的旧校准配置仅在临时副本中忽略，避免叠加倍率。"))
		TObjectPtr<UAnimBlueprint> ClothingBlueprint;
	UPROPERTY(EditAnywhere, Category = "角色", meta = (GetOptions = "GetPhysicsRootOptions", DisplayName = "物理根骨骼", ToolTip = "从所选衣物动画蓝图的 Kawaii 节点主根骨骼中选择。None 表示自动选择，仅适用于蓝图中只有一个 Kawaii 节点的情况。"))
		FName PhysicsRoot;
	/** 留空时优先复用节点和约束资产的身体球体、胶囊体；没有可用形状时才读取人物物理资产。 */
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "身体碰撞胶囊（端点模式）", EditCondition = "!bEditCollisionShapes", EditConditionHides, ToolTip = "提供 1 至 32 个骨骼局部空间胶囊，不支持插槽。留空时自动导入。点击“读取碰撞与骨链”可改用中心、旋转、半径和长度编辑。"))
		TArray<FKawaiiClothingCapsuleDefinition> BodyCapsules;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "使用形状编辑", ToolTip = "启用后用下方列表替代端点模式。列表内至少需要一个启用的形状，全部禁用不会重新自动导入。"))
		bool bEditCollisionShapes = false;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "身体碰撞形状", TitleProperty = "Bone", EditCondition = "bEditCollisionShapes", EditConditionHides))
		TArray<FKawaiiCalibrationCollisionShape> CollisionShapes;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (DisplayName = "替换原碰撞的骨骼", ToolTip = "读取时记录原碰撞骨骼，应用后替换这些骨骼上的原生胶囊和外侧球体。删除或禁用形状时保留此列表，防止原碰撞重新生效；设为空则叠加原碰撞。"))
		TArray<FName> ReplacedCollisionBones;
	UPROPERTY(EditAnywhere, Category = "骨链活动范围", meta = (DisplayName = "骨链活动范围", TitleProperty = "RootBone", ToolTip = "读取所选节点的主根和附加根骨骼；勾选覆盖后调整该骨链。未勾选的项目沿用原节点设置。"))
		TArray<FKawaiiCalibrationChainLimit> ChainLimits;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (ClampMin = "0", ClampMax = "5", DisplayName = "柔性缓冲距离（厘米）", ToolTip = "在硬碰撞表面外增加柔性缓冲区域，提前推开靠近的衣物骨骼。"))
		float SoftMargin = .5f;
	UPROPERTY(EditAnywhere, Category = "碰撞", meta = (ClampMin = "0", ClampMax = "100", DisplayName = "柔性响应速度", ToolTip = "柔性缓冲区域内的推开响应速度；越大响应越快，设为 0 仅保留硬碰撞修正。"))
		float SoftResponse = 12.f;
	UPROPERTY(EditAnywhere, Category = "测试动作", meta = (TitleProperty = "Animation", DisplayName = "测试动作列表", ToolTip = "按列表顺序播放多个动作，可连续测试切换，也可添加仅用于验证的独立片段。"))
		TArray<FKawaiiCalibrationMotion> Motions;
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (ClampMin = "30", ClampMax = "120", DisplayName = "采样帧率", ToolTip = "离线测试每秒采样的帧数。提高此值会增加采样数量和计算耗时。"))
		int32 SampleRate = 60;
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (ClampMin = "0", ClampMax = "240", DisplayName = "预热帧数", ToolTip = "重置物理后保持片段首帧姿势进行的预热帧数，预热帧不参与评分。"))
		int32 WarmUpFrames = 30;
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (ClampMin = "8", ClampMax = "1024", DisplayName = "GPU 候选数量", ToolTip = "GPU 筛选的参数组合总数，包含基准配置。数量越多，搜索范围越充分，计算耗时越长。"))
		int32 CandidateCount = 128;
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (ClampMin = "1", ClampMax = "16", DisplayName = "CPU 复验候选数量", ToolTip = "从 GPU 筛选结果中选取多少组候选进行原始 Kawaii CPU 模拟验证；基准配置会额外验证。"))
		int32 CPUFinalists = 4;
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (DisplayName = "随机种子", ToolTip = "控制候选参数采样；相同输入和种子可重复生成同一组候选。"))
		int32 Seed = 1337;
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (DisplayName = "阻尼倍率范围", ToolTip = "相对于原始节点阻尼的搜索倍率范围，X 为下限，Y 为上限，范围为 0.1 至 4。"))
		FVector2D DampingScaleRange = FVector2D(.5, 2);
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (DisplayName = "刚度倍率范围", ToolTip = "相对于原始节点刚度的搜索倍率范围，X 为下限，Y 为上限，范围为 0.1 至 4。"))
		FVector2D StiffnessScaleRange = FVector2D(.5, 2);
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (DisplayName = "骨骼碰撞半径倍率范围", ToolTip = "相对于原始节点骨骼碰撞半径的搜索倍率范围，X 为下限，Y 为上限，范围为 0.25 至 4。"))
		FVector2D RadiusScaleRange = FVector2D(.75, 1.5);
	UPROPERTY(EditAnywhere, Category = "搜索", meta = (ClampMin = "1", ClampMax = "8", DisplayName = "候选碰撞迭代上限", ToolTip = "候选配置允许的最大碰撞求解次数。次数越多，运行时 CPU 开销通常越高；基准配置固定为 2 次。"))
		int32 MaxContactIterations = 4;
	UPROPERTY(EditAnywhere, Category = "评分", meta = (ClampMin = "0", DisplayName = "穿透惩罚权重", ToolTip = "对碰撞代理穿透深度的评分权重，越大越倾向于减少穿透。"))
		float PenetrationWeight = 100.f;
	UPROPERTY(EditAnywhere, Category = "评分", meta = (ClampMin = "0", DisplayName = "姿势偏离惩罚权重", ToolTip = "对模拟骨骼偏离输入动画姿势的评分权重。"))
		float DeviationWeight = .05f;
	UPROPERTY(EditAnywhere, Category = "评分", meta = (ClampMin = "0", DisplayName = "抖动惩罚权重", ToolTip = "对相邻帧位移变化的评分权重，用于抑制抖动。"))
		float JitterWeight = .5f;
	UPROPERTY(EditAnywhere, Category = "评分", meta = (ClampMin = "0", DisplayName = "迭代开销惩罚权重", ToolTip = "按碰撞迭代次数惩罚高开销配置，不直接使用实测 CPU 耗时评分。"))
		float IterationCostWeight = .01f;
	UPROPERTY(EditAnywhere, Category = "评分", meta = (ClampMin = "0", ClampMax = "512", DisplayName = "蒙皮顶点采样数", ToolTip = "离线 CPU 验证的顶点数，其中最多 128 个可移动接触点用于运行时修正。接触点仅根据训练动作选择。设为 0 时关闭蒙皮接触修正，只验证骨骼代理。"))
		int32 SkinSampleCount = 512;
	UPROPERTY(EditAnywhere, Category = "验收", meta = (ClampMin = "0", DisplayName = "最大允许穿透（厘米）", ToolTip = "训练和独立验证片段都必须满足此上限，才会生成可应用配置。存在蒙皮采样时按实际布面验收，否则按固定半径骨骼代理验收；两类数据均保留在报告中。"))
		float MaxAcceptedPenetration = .5f;
	UPROPERTY(EditAnywhere, Category = "验收", meta = (ClampMin = "0", DisplayName = "最大允许骨长误差（厘米）", ToolTip = "限制碰撞推开造成的骨链拉伸，超标时校准失败并保留诊断报告。"))
		float MaxAcceptedLengthError = .5f;
	UPROPERTY(EditAnywhere, Category = "输出", meta = (DisplayName = "输出目录", ToolTip = "保存测试集和校准配置的内容目录，必须是有效的 /Game 子目录。"))
		FDirectoryPath OutputDirectory;
};
