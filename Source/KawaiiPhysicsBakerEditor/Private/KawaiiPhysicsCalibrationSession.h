#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

#include "AnimNode_KawaiiPhysics.h"

#include "KawaiiCalibrationGPU.h"
#include "KawaiiPhysicsCalibrationSearch.h"

class UKawaiiPhysicsCalibrationSettings;
class UKawaiiPhysicsCalibrationProfile;
class USkeletalMeshComponent;
class UKawaiiCalibrationAnimInstance;
class FPreviewScene;

class FKawaiiPhysicsCalibrationSession : public FGCObject
{
public:
	FKawaiiPhysicsCalibrationSession();
	~FKawaiiPhysicsCalibrationSession();
	bool Start(UKawaiiPhysicsCalibrationSettings* InSettings, FString& OutError);
	bool CreateManualProfile(UKawaiiPhysicsCalibrationSettings* InSettings, FString& OutError);
	void Tick();
	void Cancel();
	bool IsRunning() const;

	FString GetStatus() const { return Status; }

	float GetProgress() const;

	UKawaiiPhysicsCalibrationProfile* GetResult() const { return Result; }

	FString GetReportPath() const { return ReportPath; }

	bool SaveResult(FString& OutError, bool bAllowUnaccepted = false);
	bool IsResultAccepted() const { return bResultAccepted; }
	bool HasUnacceptedResult() const { return Stage == EStage::Complete && Result && !bResultAccepted; }
	bool ApplyToBlueprint(FString& OutError);

public:
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	virtual FString GetReferencerName() const override { return TEXT("KawaiiCalibrationSession"); }

private:
	struct FSample
	{
		TArray<FTransform> BodyLocal, ClothingLocal;
		TArray<KawaiiContact::FCapsule> Capsules;
		bool bReset = false, bScore = true, bValidation = false;
		int32 Motion = 0;
		float Time = 0;
	};

	struct FSkinPoint
	{
		FVector Position;
		TArray<int32> Bones;
		TArray<float> Weights;
	};

	struct FMetrics
	{
		void AddDepth(double Depth, int32 Motion, float Time, int32 MeshBone, int32 Capsule);

		double Depth2 = 0, BoneDepth2 = 0, SkinDepth2 = 0, Deviation2 = 0, Jitter2 = 0, MaxDepth = 0, MaxLengthError = 0, CPUSeconds = 0;
		double MaxBoneDepth = 0, MaxSkinDepth = 0;
		int32 WorstMotion = INDEX_NONE, WorstMeshBone = INDEX_NONE, WorstCapsule = INDEX_NONE;
		int32 WorstSkinSample = INDEX_NONE;
		int32 WorstSkinMotion = INDEX_NONE, WorstSkinCapsule = INDEX_NONE;
		float WorstSkinTime = 0.f;
		float WorstTime = 0.f;
		int64 Count = 0, BoneCount = 0, SkinCount = 0;
	};

	struct FVerified
	{
		int32 Candidate = 0;
		FMetrics Training, Validation;
		double Rank = 0;
	};

	bool Prepare(FString& OutError);
	bool SampleMotion(int32 Motion, FString& OutError);
	bool BeginCPU(int32 Candidate, FString& OutError);
	bool StepCPU(FString& OutError);
	void SelectSurfacePoints();
	void Finish();
	void ClearScene();
	double Rank(const FMetrics& M, int32 Candidate) const;
	double AcceptanceDepth(const FMetrics& M) const;
	double MeanSquaredPenetration(const FMetrics& M) const;
	void Fail(const FString& Error);

private:
	TObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings;
	TObjectPtr<UKawaiiPhysicsCalibrationProfile> WorkingProfile, Result;
	TObjectPtr<UKawaiiPhysicsCalibrationProfile> OriginalProfile;
	TObjectPtr<USkeletalMeshComponent> Body, Clothing;
	TObjectPtr<UKawaiiCalibrationAnimInstance> BodyInstance, ClothingInstance;
	TObjectPtr<AActor> Actor;
	FAnimNode_KawaiiPhysics Template;
	FAnimNode_KawaiiPhysics OriginalTemplate;
	TArray<KawaiiCalibrationSearch::FCandidate> Candidates;
	TUniquePtr<FPreviewScene> Scene;
	TSharedPtr<FKawaiiCalibrationGPU, ESPMode::ThreadSafe> GPU;
	FKawaiiCalibrationGPUInput GPUInput;
	TArray<FSample> Samples;
	TArray<int32> ParticleMeshBones, ParticleParents, Finalists;
	TArray<FVector2f> ParticleRanges;
	TArray<FSkinPoint> SkinPoints;
	TArray<int32> SurfacePointIndices;
	TArray<double> SurfacePointDepths;
	bool bSurfacePointsSelected = false;
	TArray<FTransform> InverseRefPose;
	TArray<FVector> PreviousOffsets, PreviousDisplacements;
	bool bResultAccepted = false;
	TArray<FVerified> Verified;
	TArray<TObjectPtr<UKawaiiPhysicsCalibrationProfile>> VerifiedProfiles;
	TArray<FVector4f> GPUScores;
	TArray<FGuid> SourceAnimationGuids;
	FVerified Current;
	FString Status = TEXT("请选择人物与衣物网格体、衣物动画蓝图和测试动作。"), ReportPath;
	FString CollisionSource;
	enum class EStage
	{
		Idle,
		Sampling,
		GPU,
		CPU,
		Complete,
		Failed,
		Cancelled
	} Stage = EStage::Idle;
	int32 MotionIndex = 0, FinalistIndex = 0, SampleIndex = 0;
	double StartTime = 0;
	uint32 SourceNodeHash = 0;
};
