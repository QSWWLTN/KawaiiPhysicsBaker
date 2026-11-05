#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

class FAdvancedPreviewScene;
class FPreviewScene;
class UAnimBlueprint;
class UKawaiiPhysicsBakeSettings;
class UAnimSequence;
class USkeletalMeshComponent;
class UWorld;
class AActor;
class FKawaiiPhysicsBakePreviewCorrection;

/** Shared by the visible viewport and offline recorder. No gameplay actor is touched. */
class FKawaiiPhysicsBakeSession : public FGCObject
{
public:
	FKawaiiPhysicsBakeSession();
	virtual ~FKawaiiPhysicsBakeSession() override;
	bool Initialize(UAnimBlueprint* InBlueprint, FString& OutError);
	void Tick(float DeltaSeconds = -1.f);
	bool StartPreview(FString& OutError);
	void SetPreviewPaused(bool bPause);
	void SeekPreview(int32 Frame);
	void SetPreviewLooping(bool bLoop);
	bool CanScrubPreview() const;

	bool IsPreviewPaused() const { return bPreviewPaused; }

	bool IsSeeking() const;
	int32 GetPreviewFrame() const;
	int32 GetRequestedFrame() const;

	int32 GetLastFrame() const { return IntervalCount; }

	int32 GetCachedFrameCount() const { return SampleIndex; }

	float GetPreviewTime() const;

	float GetDuration() const { return SourceLength; }

	void StopPreview();
	bool ResetPreview(FString& OutError);
	bool StartBake(FString& OutError);
	void Cancel();

	bool IsRunning() const { return bRunning; }

	bool IsBaking() const;

	FText GetStatus() const { return Status; }

	FText GetCollisionStatus() const { return CollisionStatus; }

	float GetProgress() const;
	UWorld* GetWorld() const;
	FPreviewScene* GetPreviewScene() const;
	USkeletalMeshComponent* GetMainComponent() const;

	const TArray<TWeakObjectPtr<UObject>>& GetOutputAssets() const { return OutputAssets; }

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	virtual FString GetReferencerName() const override { return TEXT("FKawaiiPhysicsBakeSession"); }

private:
	struct FPart;
	bool Prepare(FString& OutError, bool bForBake = false);
	bool Evaluate(float SourceTime, FString& OutError);
	bool ValidateInputs(FString& OutError) const;
	void CachePreviewPose();
	void DisplayPreviewPose(int32 Frame);
	void TickPreview(float DeltaSeconds);
	bool Capture(FString& OutError);
	bool WriteAssets(FString& OutError);
	void ClearScene();
	void Fail(const FString& Error);

private:
	TUniquePtr<FAdvancedPreviewScene> PreviewScene;
	TObjectPtr<UAnimBlueprint> Blueprint = nullptr;
	TObjectPtr<UKawaiiPhysicsBakeSettings> Settings = nullptr;
	TObjectPtr<AActor> PreviewActor = nullptr;
	TArray<TUniquePtr<FPart>> Parts;
	TUniquePtr<FKawaiiPhysicsBakePreviewCorrection> PreviewCorrection;
	int32 PreviewCollisionSourceIndex = INDEX_NONE;
	TArray<TObjectPtr<UObject>> CreatedAssets;
	TArray<TWeakObjectPtr<UObject>> OutputAssets;
	FText Status;
	FText CollisionStatus;
	uint64 LastEngineFrame = MAX_uint64;
	int32 WarmUpIndex = 0;
	int32 SampleIndex = 0;
	int32 IntervalCount = 0;
	float FixedDelta = 1.f / 60.f;
	float SourceLength = 0.f;
	double PreviewAccumulator = 0.0;
	int32 DisplayedSample = INDEX_NONE;
	int32 SeekTarget = INDEX_NONE;
	bool bPreviewAvailable = false;
	bool bPreviewPaused = false;
	bool bLoopPreview = false;
	bool bRunning = false;
	bool bBaking = false;
	bool bPreviewProxyCollisionActive = false;
};
