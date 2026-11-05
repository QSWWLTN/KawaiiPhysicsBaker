#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "KawaiiClothingProxyMotion.h"
#include "UObject/GCObject.h"
#include "KawaiiClothingMotionReadback.generated.h"

class UAnimSequence;
class UKawaiiPhysicsBakePreviewComponent;
class UKawaiiPhysicsIKCorrectionProfile;

UCLASS(Transient)
class UKawaiiClothingReadbackSettings : public UObject {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="回读", meta=(DisplayName="原衣物代理运动", ToolTip="选择初次烘焙生成的衣物 ProxyMotion。更新保持该资产引用及动作集合映射。"))
	TObjectPtr<UKawaiiClothingProxyMotion> Motion;
	UPROPERTY(EditAnywhere, Category="回读", meta=(DisplayName="修整后的衣物动画", ToolTip="可选原烘焙动画或手工修整副本。先应用骨骼关键帧修改并保存；回读直接采样该动画，不运行物理图。时长必须保持不变。"))
	TObjectPtr<UAnimSequence> EditedAnimation;
	UPROPERTY(EditAnywhere, Category="预览", meta=(DisplayName="身体网格体", ToolTip="与原代理关联的身体烘焙动画使用同一骨架。通常自动读取该动画的预览网格体。"))
	TObjectPtr<USkeletalMesh> BodyMesh;
	UPROPERTY(EditAnywhere, Category="预览", meta=(DisplayName="头发运行时修正配置", ToolTip="可选。使用角色 Automatic Clothing Proxy Correction 节点相同的 Profile 预览衣物对头发的影响；预览修正不写回身体动画。"))
	TObjectPtr<UKawaiiPhysicsIKCorrectionProfile> CorrectionProfile;
	UPROPERTY(EditAnywhere, Category="预览", meta=(DisplayName="修正权重", ClampMin="0", ClampMax="1"))
	float CorrectionAlpha = 1.f;
};

namespace KawaiiClothingReadback {
	struct FFrame {
		TArray<FTransform3f> Pose;
		TMap<FName, float> MaterialCurves, MorphCurves;
	};
	FGuid AnimationRevision(const UAnimSequence* Animation);
	bool Decode(UAnimSequence& Animation, USkeletalMesh& Mesh, float Time, FFrame& Frame, FString& Error);
	/** Pure readback into an independently owned transient take. Never evaluates Kawaii or changes inputs. */
	bool Build(const UKawaiiClothingProxyMotion& Original, UAnimSequence& Edited, UKawaiiClothingProxyMotion& Result,
		TArray<FFrame>& Frames, FString& Error);
	/** Preserve object identity so existing action sets keep working; save a backup before replacement. */
	bool Publish(UKawaiiClothingProxyMotion& Original, const UKawaiiClothingProxyMotion& Candidate, FString& BackupPath, FString& Error);
}

/** Owns the exact frames used to generate the candidate and its review viewport. */
class FKawaiiClothingReadbackPreview : public FGCObject {
public:
	FKawaiiClothingReadbackPreview();
	virtual ~FKawaiiClothingReadbackPreview();
	bool Read(const UKawaiiClothingReadbackSettings& Settings, FString& Error);
	void Clear();
	bool IsCurrent(FString& Error) const;
	void ShowFrame(int32 Frame);
	void Draw(FPrimitiveDrawInterface* PDI) const;
	bool Publish(FString& BackupPath, FString& Error);
	FAdvancedPreviewScene& GetScene() { return Scene; }
	FBox GetBounds() const;
	int32 NumFrames() const { return ClothingFrames.Num(); }
	int32 GetFrame() const { return DisplayedFrame; }
	int32 GetRate() const { return Candidate ? Candidate->SampleRate : 60; }
	UKawaiiClothingProxyMotion* GetCandidate() const { return Candidate; }
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("KawaiiClothingReadbackPreview"); }
private:
	FAdvancedPreviewScene Scene;
	TObjectPtr<UKawaiiPhysicsBakePreviewComponent> BodyComponent, ClothingComponent;
	TObjectPtr<UKawaiiClothingProxyMotion> Original, Candidate;
	TObjectPtr<UAnimSequence> BodyAnimation;
	TObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Profile;
	FGuid OriginalRevision, BodyRevision;
	FString ClothingSkeletonSignature, BodySkeletonSignature;
	TArray<FName> IgnoredTargetRoots;
	TArray<KawaiiClothingReadback::FFrame> BodyFrames, ClothingFrames;
	TArray<FKawaiiClothingProxyCapsule> DisplayedCapsules;
	int32 DisplayedFrame = 0;
};
