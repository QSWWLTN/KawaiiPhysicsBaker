#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "KawaiiPhysicsIKEditorLibrary.generated.h"

class UEdGraphNode;
class UBlueprint;
class UAnimBlueprint;
class UKawaiiPhysicsIKCorrectionProfile;

/** Small editor scripting bridge for the native cached-pose reference. */
UCLASS()
class UKawaiiPhysicsIKEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Bind a Use Cached Pose to a Save Cached Pose in the same animation graph. */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Editor")
		static bool BindUseCachedPose(UEdGraphNode* UseNode, UEdGraphNode* SaveNode);

	/** 在组件空间输入前插入骨骼旋转修正；同名节点只更新参数，不重复创建。 */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Editor")
		static UEdGraphNode* InsertBoneRotationBeforeNode(UEdGraphNode* TargetNode, FName NodeName, FName BoneName, FRotator Rotation, const FString& Comment);

	/** Read the compiler result that UBlueprint does not expose to Python. */
	UFUNCTION(BlueprintPure, Category = "Kawaii Physics|Editor")
		static FString GetBlueprintStatus(const UBlueprint* Blueprint);

	UFUNCTION(BlueprintPure, Category = "Kawaii Physics|Editor")
		static bool IsBlueprintUpToDate(const UBlueprint* Blueprint);

	/** 根据动画蓝图中已配置的 KawaiiPhysics 节点自动生成（或重新生成）DA_<蓝图名>_IKCorrection，
	 *  可导入节点的外侧球体/胶囊限制作为身体碰撞体，并填入同一蓝图中 Profile 为空的自动衣物代理修正节点。
	 *  bDryRun 只返回报告，不创建或修改任何资产。 */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Editor")
		static UKawaiiPhysicsIKCorrectionProfile* GenerateIKCorrectionProfile(UAnimBlueprint* AnimBlueprint,
																			  bool bImportBodyCapsules,
																			  bool bAssignToProxyNodes,
																			  bool bSave,
																			  bool bDryRun,
																			  FString& Report);
};
