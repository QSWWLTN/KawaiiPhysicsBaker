#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "KawaiiClothingProxyPlayback.h"
#include "KawaiiClothingProxySnapshot.h"
#include "AnimNode_KawaiiAutoClothingProxy.generated.h"

/** Follows players in the input branch and corrects the current pose against baked garment proxies. */
USTRUCT(BlueprintInternalUseOnly)
struct KAWAIIPHYSICSBAKERRUNTIME_API FAnimNode_KawaiiAutoClothingProxy : public FAnimNode_Base
{
	GENERATED_BODY()

	// The output pin is named Pose. A matching input property would generate an
	// exposed-value handler for the output and overwrite this link at runtime.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Links", meta = (DisplayName = "Pose"))
	FPoseLink SourcePose;

	/** Current garment's action set. Use this OR Proxy Motions. Changing the pin invalidates old colliders immediately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clothing Proxy", meta = (PinShownByDefault))
	TObjectPtr<UKawaiiClothingProxyMotionSet> ProxyMotionSet = nullptr;

	/** Optional explicit action list for a single garment. Do not combine different garments in one list. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clothing Proxy", meta = (PinShownByDefault))
	TArray<TObjectPtr<UKawaiiClothingProxyMotion>> ProxyMotions;

	/** Defines the target hair/clothing chains. The current input pose is the correction baseline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (PinHiddenByDefault))
	TObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Profile = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Correction", meta = (PinShownByDefault, ClampMin = "0", ClampMax = "1"))
	float Alpha = 1.f;

	/** Use the placement recorded during baking when the target is the original Main mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clothing Proxy", meta = (PinHiddenByDefault))
	bool bUseBakedRelativeTransform = true;

	/** Garment component space to this mesh's component space, used when Use Baked Relative Transform is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clothing Proxy", meta = (PinHiddenByDefault))
	FTransform ProxyToTarget = FTransform::Identity;

	/** Proxy tracks are baked without root motion extraction. When the live pose extracts root motion
	 * (Montage or Root Motion mode), its root bone stays locked while the recorded proxies keep travelling.
	 * This moves the sampled proxies rigidly from their recorded root to the live root bone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clothing Proxy",
		meta = (PinHiddenByDefault, DisplayName = "代理跟随当前根骨骼",
			ToolTip = "代理轨道烘焙时不提取 Root Motion。运行时若提取了 Root Motion（蒙太奇或 Root Motion 模式），身体根骨骼被锁定，而录制的代理仍沿原轨迹移动，导致碰撞体偏离身体、头发错乱。开启后代理会从录制的根骨骼刚性地移到当前根骨骼。未提取 Root Motion 时两者重合，不产生任何变化。"))
	bool bFollowLiveRoot = true;

	/** Optional single full-body Slot in this input branch. Configure once for Montage playback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action Tracking", meta = (PinHiddenByDefault))
	FName MontageSlot;

	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;
	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;

	const FString& GetProxyDebugReason() const { return DebugReason; }

private:
	friend struct FKawaiiAutoClothingProxyTestAccess;
	UPROPERTY(Transient)
	FAnimNode_KawaiiPhysicsIKCorrection Correction;
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiClothingProxyMotionSet> SnapshotSet = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UKawaiiClothingProxyMotion>> SnapshotInputMotions;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UKawaiiClothingProxyMotion>> SnapshotAssets;
	TArray<TSharedPtr<const KawaiiClothingProxy::FSnapshot, ESPMode::ThreadSafe>> Tracks;
	TArray<TArray<FName>> IgnoredRoots;
	TMap<FSoftObjectPath, int32> ActionMap;
	FString SnapshotError;
	FString DebugReason;
	uint32 TrackingSerial = 0;
	float ScopeWeight = 0.f;
	int32 ActiveActions = 0;
	int32 CapsuleCount = 0;
	/** Distance between the recorded proxy root and the live root in the last evaluation, for debugging. */
	float RootReanchorDistance = 0.f;
};
