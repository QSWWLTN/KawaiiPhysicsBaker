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

/** Shared by the visible viewport and offline recorder. No gameplay actor is touched. */
class FKawaiiPhysicsBakeSession : public FGCObject
{
public:
	FKawaiiPhysicsBakeSession();
	virtual ~FKawaiiPhysicsBakeSession() override;
	bool Initialize(UAnimBlueprint* InBlueprint, FString& OutError);
	void Tick();
	bool StartPreview(FString& OutError);
	void StopPreview();
	bool ResetPreview(FString& OutError);
	bool StartBake(FString& OutError);
	void Cancel();
	bool IsRunning() const { return bRunning; }
	bool IsBaking() const { return bRunning && bBaking; }
	FText GetStatus() const { return Status; }
	float GetProgress() const;
	UWorld* GetWorld() const;
	FPreviewScene* GetPreviewScene() const;
	USkeletalMeshComponent* GetMainComponent() const;
	const TArray<TWeakObjectPtr<UAnimSequence>>& GetOutputAssets() const { return OutputAssets; }
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FKawaiiPhysicsBakeSession"); }

private:
	struct FPart;
	bool Prepare(FString& OutError);
	bool Evaluate(float SourceTime, FString& OutError);
	bool Capture(FString& OutError);
	bool WriteAssets(FString& OutError);
	void ClearScene();
	void Fail(const FString& Error);
	TUniquePtr<FAdvancedPreviewScene> PreviewScene;
	TObjectPtr<UAnimBlueprint> Blueprint = nullptr;
	TObjectPtr<UKawaiiPhysicsBakeSettings> Settings = nullptr;
	TObjectPtr<AActor> PreviewActor = nullptr;
	TArray<TUniquePtr<FPart>> Parts;
	TArray<TObjectPtr<UAnimSequence>> CreatedAssets;
	TArray<TWeakObjectPtr<UAnimSequence>> OutputAssets;
	FText Status;
	uint64 LastEngineFrame = MAX_uint64;
	int32 WarmUpIndex = 0;
	int32 SampleIndex = 0;
	int32 IntervalCount = 0;
	float FixedDelta = 1.f / 60.f;
	float SourceLength = 0.f;
	bool bRunning = false;
	bool bBaking = false;
};
