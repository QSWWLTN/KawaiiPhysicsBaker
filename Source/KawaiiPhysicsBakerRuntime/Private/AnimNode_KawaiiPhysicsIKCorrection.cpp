#include "AnimNode_KawaiiPhysicsIKCorrection.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CountersTrace.h"

#include "KawaiiPhysicsIKCorrectionMath.h"
#include "KawaiiPhysicsBakerProfiling.h"
#include "KawaiiPhysicsGPUCorrection.h"

namespace {
	/** A longer gap (hitch, pause, teleport-like discontinuity) restarts from a stateless solve. */
	constexpr float MaxTemporalGapSeconds = 0.25f;
	/** Upper bound accepted for the profile's temporal time constants. */
	constexpr float MaxTemporalTimeConstant = 10.f;

	FVector4f GPUQuaternion(const FQuat& Q) {
		return FVector4f(float(Q.X), float(Q.Y), float(Q.Z), float(Q.W));
	}

	struct FResolvedChain
	{
		TArray<int32> Bones;
		FName RootName;
		int32 Anchor = INDEX_NONE;
		int32 Driver = INDEX_NONE;
		float Weight = 0.f;
		float Radius = 0.f;
	};

	bool FiniteTransform(const FTransform& Transform) {
		const FVector Scale = Transform.GetScale3D();
		return !Transform.ContainsNaN() && Transform.IsRotationNormalized() && Scale.X > UE_SMALL_NUMBER && Scale.Y > UE_SMALL_NUMBER
			&& Scale.Z > UE_SMALL_NUMBER;
	}

	int32 ResolveBone(FBoneReference Bone, const FBoneContainer& RequiredBones) {
		Bone.Initialize(RequiredBones);
		return Bone.IsValidToEvaluate(RequiredBones) ? Bone.GetCompactPoseIndex(RequiredBones).GetInt() : INDEX_NONE;
	}

	bool IsInSubtree(int32 Bone, int32 Root, const TArray<int32>& Parents) {
		for (int32 Index = Bone; Index != INDEX_NONE; Index = Parents[Index]) {
			if (Index == Root) {
				return true;
			}
		}
		return false;
	}

	float ValidTimeConstant(float Value) {
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.f, MaxTemporalTimeConstant) : 0.f;
	}

	/** Re-impose rest lengths from the pinned root outward, keeping each segment's direction. */
	void RestoreChainLengths(TArray<FVector>& Points, const TArray<double>& Lengths, const TArray<FVector>& FallbackPoints) {
		for (int32 Index = 1; Index < Points.Num(); ++Index) {
			FVector Direction = Points[Index] - Points[Index - 1];
			if (!Direction.Normalize(UE_KINDA_SMALL_NUMBER)) {
				Direction = FallbackPoints[Index] - FallbackPoints[Index - 1];
				if (!Direction.Normalize(UE_KINDA_SMALL_NUMBER)) {
					continue;
				}
			}
			Points[Index] = Points[Index - 1] + Direction * Lengths[Index - 1];
		}
	}

	bool BuildComponentPose(const FCompactPose& Pose, const TArray<int32>& Parents, TArray<FTransform>& Out) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_BuildComponentPose);
		Out.SetNum(Pose.GetNumBones());
		for (int32 Index = 0; Index < Out.Num(); ++Index) {
			const FTransform& Local = Pose[FCompactPoseBoneIndex(Index)];
			if (!FiniteTransform(Local)) {
				return false;
			}
			Out[Index] = Parents[Index] == INDEX_NONE ? Local : Local * Out[Parents[Index]];
			Out[Index].NormalizeRotation();
			if (!FiniteTransform(Out[Index])) {
				return false;
			}
		}
		return true;
	}
}

void FAnimNode_KawaiiPhysicsIKCorrection::Initialize_AnyThread(const FAnimationInitializeContext& Context) {
	FAnimNode_Base::Initialize_AnyThread(Context);
	GetEvaluateGraphExposedInputs().Execute(Context);
	BakedPose.Initialize(Context);
	DriverPose.Initialize(Context);
	ResetTemporalState();
}

void FAnimNode_KawaiiPhysicsIKCorrection::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) {
	BakedPose.CacheBones(Context);
	DriverPose.CacheBones(Context);
	// Compact bone indices may change with LOD; stored solutions refer to the old indices.
	ResetTemporalState();
}

void FAnimNode_KawaiiPhysicsIKCorrection::AccumulateDeltaTime(float DeltaTime) {
	if (FMath::IsFinite(DeltaTime) && DeltaTime > 0.f) {
		PendingDeltaTime = FMath::Min(PendingDeltaTime + DeltaTime, 1.f);
	}
}

void FAnimNode_KawaiiPhysicsIKCorrection::ResetTemporalState() {
	TemporalStates.Reset();
	PendingDeltaTime = 0.f;
	bGPUCorrectionRequested = false;
	if (GPUState) {
		GPUState->Invalidate(this);
	}
}

void FAnimNode_KawaiiPhysicsIKCorrection::ReportGPUReason(const FString& Reason) {
	if (GPUState) {
		FScopeLock Lock(&GPUState->Mutex);
		if (GPUState->Owner == this && !GPUState->bRequiresCPU) {
			GPUState->LastReason = Reason;
		}
	}
}

void FAnimNode_KawaiiPhysicsIKCorrection::PreUpdate(const UAnimInstance* InAnimInstance) {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_PreUpdate);
	SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerCorrectionSnapshot);
	check(IsInGameThread());
	GPUState = KawaiiPhysicsGPU::Prepare(InAnimInstance, this, Profile && bGPUCorrectionRequested, GPUState);
	bGPUCorrectionRequested = false;
	bHasProfileSnapshot = Profile != nullptr;
	SnapshotChains.Reset();
	SnapshotCapsules.Reset();
	SnapshotParentCapsules.Reset();
	SnapshotError.Reset();
	bSnapshotUseAttachedParent = false;
	SnapshotSettleTime = 0.f;
	SnapshotSmoothingTime = 0.f;
	if (Profile) {
		SnapshotChains = Profile->Chains;
		SnapshotCapsules = Profile->Capsules;
		SnapshotIterations = Profile->Iterations;
		SnapshotSettleTime = Profile->SettleTime;
		SnapshotSmoothingTime = Profile->SmoothingTime;
		bSnapshotUseAttachedParent = Profile->bUseAttachedParentForCollision;
	}
	// Game-thread gauges describe the last node snapshot, not a sum across characters.
	TRACE_INT_VALUE(TEXT("KawaiiBaker/Correction/ConfiguredChainsLastSnapshot"), SnapshotChains.Num());
	TRACE_INT_VALUE(TEXT("KawaiiBaker/Correction/BodyCapsulesLastSnapshot"), SnapshotCapsules.Num());
	TRACE_INT_VALUE(TEXT("KawaiiBaker/Correction/IterationsLastSnapshot"), Profile ? SnapshotIterations : 0);
	if (!bSnapshotUseAttachedParent) {
		return;
	}
	if (SnapshotCapsules.Num() > 64) {
		SnapshotError = TEXT("attached_parent_capsule_limit_exceeded");
		return;
	}
	const USkeletalMeshComponent* Target = InAnimInstance ? InAnimInstance->GetSkelMeshComponent() : nullptr;
	if (!Target || !Target->IsRegistered()) {
		SnapshotError = TEXT("attached_parent_collision_missing_target");
		return;
	}
	const USkeletalMeshComponent* Source = nullptr;
	for (const USceneComponent* Parent = Target->GetAttachParent(); Parent; Parent = Parent->GetAttachParent()) {
		Source = Cast<USkeletalMeshComponent>(Parent);
		if (Source) {
			break;
		}
	}
	if (!Source || !Source->IsRegistered() || !Source->GetSkeletalMeshAsset()) {
		SnapshotError = TEXT("attached_parent_collision_missing_source");
		return;
	}
	if (Source->LeaderPoseComponent.IsValid()) {
		SnapshotError = TEXT("attached_parent_collision_leader_pose_unsupported");
		return;
	}
	const FTransform SourceWorld = Source->GetComponentTransform();
	const FTransform TargetWorld = Target->GetComponentTransform();
	if (!FiniteTransform(SourceWorld) || !FiniteTransform(TargetWorld)) {
		SnapshotError = TEXT("attached_parent_collision_invalid_component_transform");
		return;
	}
	const double SpaceStretch = SourceWorld.GetScale3D().GetAbsMax() / TargetWorld.GetScale3D().GetMin();
	if (!FMath::IsFinite(SpaceStretch) || SpaceStretch <= 0.0) {
		SnapshotError = TEXT("attached_parent_collision_invalid_component_scale");
		return;
	}
	const FReferenceSkeleton& SourceSkeleton = Source->GetSkeletalMeshAsset()->GetRefSkeleton();
	const TArray<FTransform>& SourcePose = Source->GetComponentSpaceTransforms();
	for (const FKawaiiPhysicsIKCorrectionCapsule& Definition : SnapshotCapsules) {
		const int32 Start = SourceSkeleton.FindBoneIndex(Definition.StartBone.BoneName);
		const int32 End = Definition.EndBone.BoneName.IsNone() ? Start : SourceSkeleton.FindBoneIndex(Definition.EndBone.BoneName);
		if (!SourcePose.IsValidIndex(Start) || !SourcePose.IsValidIndex(End)) {
			SnapshotError = TEXT("attached_parent_collision_bone_missing");
			return;
		}
		if (Definition.StartOffset.ContainsNaN() || Definition.EndOffset.ContainsNaN() || !FMath::IsFinite(Definition.Radius) || Definition.Radius <= 0.f
			|| !FiniteTransform(SourcePose[Start]) || !FiniteTransform(SourcePose[End]))
		{
			SnapshotError = TEXT("attached_parent_collision_invalid_geometry");
			return;
		}
		KawaiiPhysicsIKCorrection::FCapsule& Capsule = SnapshotParentCapsules.AddDefaulted_GetRef();
		// Keep the transforms separate: collapsing nonuniform scales/rotations into an
		// FTransform composition would discard shear and move the collision surface.
		Capsule.Start = TargetWorld.InverseTransformPosition(SourceWorld.TransformPosition(SourcePose[Start].TransformPosition(Definition.StartOffset)));
		Capsule.End = TargetWorld.InverseTransformPosition(SourceWorld.TransformPosition(SourcePose[End].TransformPosition(Definition.EndOffset)));
		Capsule.Radius = Definition.Radius * SpaceStretch * FMath::Max(SourcePose[Start].GetScale3D().GetAbsMax(), SourcePose[End].GetScale3D().GetAbsMax());
		if (Capsule.Start.ContainsNaN() || Capsule.End.ContainsNaN() || !FMath::IsFinite(Capsule.Radius)) {
			SnapshotError = TEXT("attached_parent_collision_nonfinite_capsule");
			return;
		}
	}
}

void FAnimNode_KawaiiPhysicsIKCorrection::Update_AnyThread(const FAnimationUpdateContext& Context) {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_Update);
	GetEvaluateGraphExposedInputs().Execute(Context);
	AccumulateDeltaTime(Context.GetDeltaTime());
	BakedPose.Update(Context);
	DriverPose.Update(Context);
}

void FAnimNode_KawaiiPhysicsIKCorrection::Evaluate_AnyThread(FPoseContext& Output) {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_Evaluate);
	SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerCorrectionEvaluate);
	// A rejected profile always leaves the live body pose intact.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_InputDriverPose);
		DriverPose.Evaluate(Output);
	}
	FPoseContext BakedContext(Output);
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_InputBakedPose);
		BakedPose.Evaluate(BakedContext);
	}
	CorrectPose(Output, BakedContext);
}

void FAnimNode_KawaiiPhysicsIKCorrection::CorrectPose(FPoseContext& Output, const FPoseContext& BakedContext) {
	bGPUCorrectionRequested = false;
	MaxPenetration = MaxLengthError = 0.0;
	DebugReason = TEXT("driver_pose_fallback");
	// Each solve consumes the time reported since the previous one.
	const float DeltaTime = PendingDeltaTime;
	PendingDeltaTime = 0.f;
	// Every early return emits the uncorrected driver pose; continuing from older history afterwards would pop.
	bool bWroteCorrection = false;
	ON_SCOPE_EXIT {
		ReportGPUReason(DebugReason);
		if (!bWroteCorrection) {
			TemporalStates.Reset();
			if (GPUState) {
				GPUState->Invalidate(this);
			}
		}
	};
	if (!SnapshotError.IsEmpty()) {
		DebugReason = SnapshotError;
		return;
	}
	if (!bHasProfileSnapshot || SnapshotChains.IsEmpty()) {
		DebugReason = TEXT("missing_profile_or_chains");
		return;
	}
	if (SnapshotChains.Num() > 64 || SnapshotCapsules.Num() > 64 || SnapshotIterations < 1 || SnapshotIterations > 64 || !FMath::IsFinite(Alpha)) {
		DebugReason = TEXT("invalid_profile_limits_or_alpha");
		return;
	}

	const FBoneContainer& RequiredBones = Output.Pose.GetBoneContainer();
	const int32 NumBones = Output.Pose.GetNumBones();
	if (!RequiredBones.IsValid() || NumBones == 0) {
		DebugReason = TEXT("missing_required_bones");
		return;
	}
	TArray<int32> Parents;
	Parents.SetNum(NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone) {
		Parents[Bone] = RequiredBones.GetParentBoneIndex(FCompactPoseBoneIndex(Bone)).GetInt();
		if (Parents[Bone] >= Bone || Parents[Bone] < INDEX_NONE) {
			DebugReason = TEXT("invalid_bone_hierarchy");
			return;
		}
	}

	TArray<FResolvedChain> Chains;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_ResolveChains);
		for (const FKawaiiPhysicsIKCorrectionChain& Definition : SnapshotChains) {
			const int32 Root = ResolveBone(Definition.RootBone, RequiredBones);
			const int32 Tip = ResolveBone(Definition.TipBone, RequiredBones);
			const int32 Driver = ResolveBone(Definition.DriverBone, RequiredBones);
			if (Root == INDEX_NONE || Tip == INDEX_NONE || Driver == INDEX_NONE) {
				DebugReason = TEXT("chain_bone_missing_at_current_lod");
				return;
			}
			if (Root == Tip || Parents[Root] == INDEX_NONE || !IsInSubtree(Tip, Root, Parents)) {
				DebugReason = TEXT("chain_requires_descendant_tip_and_parent_anchor");
				return;
			}
			if (!FMath::IsFinite(Definition.DriverWeight) || Definition.DriverWeight < 0.f || Definition.DriverWeight > 1.f
				|| !FMath::IsFinite(Definition.CollisionRadius) || Definition.CollisionRadius < 0.f)
			{
				DebugReason = TEXT("invalid_chain_weight_or_radius");
				return;
			}
			for (const FResolvedChain& Existing : Chains) {
				if (IsInSubtree(Root, Existing.Bones[0], Parents) || IsInSubtree(Existing.Bones[0], Root, Parents)) {
					DebugReason = TEXT("overlapping_corrected_subtrees");
					return;
				}
			}
			FResolvedChain& Chain = Chains.AddDefaulted_GetRef();
			Chain.RootName = Definition.RootBone.BoneName;
			for (int32 Bone = Tip; Bone != Root; Bone = Parents[Bone]) {
				Chain.Bones.Insert(Bone, 0);
			}
			Chain.Bones.Insert(Root, 0);
			Chain.Anchor = Parents[Root];
			Chain.Driver = Driver;
			Chain.Weight = Definition.DriverWeight;
			Chain.Radius = Definition.CollisionRadius;
		}
		for (const FResolvedChain& Chain : Chains) {
			for (const FResolvedChain& Other : Chains) {
				if (IsInSubtree(Chain.Driver, Other.Bones[0], Parents)) {
					DebugReason = TEXT("driver_inside_corrected_subtree");
					return;
				}
			}
		}
	}

	if (BakedContext.Pose.GetNumBones() != NumBones) {
		DebugReason = TEXT("input_pose_bone_count_mismatch");
		return;
	}
	TArray<FTransform> BakedCS, DriverCS;
	if (!BuildComponentPose(BakedContext.Pose, Parents, BakedCS) || !BuildComponentPose(Output.Pose, Parents, DriverCS)) {
		DebugReason = TEXT("nonfinite_or_unsupported_input_transform");
		return;
	}

	TArray<KawaiiPhysicsIKCorrection::FCapsule> Capsules = SnapshotParentCapsules;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_PrepareCapsules);
		if (!bSnapshotUseAttachedParent) {
			for (const FKawaiiPhysicsIKCorrectionCapsule& Definition : SnapshotCapsules) {
				const int32 Start = ResolveBone(Definition.StartBone, RequiredBones);
				const int32 End = Definition.EndBone.BoneName.IsNone() ? Start : ResolveBone(Definition.EndBone, RequiredBones);
				if (Start == INDEX_NONE || End == INDEX_NONE) {
					DebugReason = TEXT("capsule_bone_missing_at_current_lod");
					return;
				}
				if (Definition.StartOffset.ContainsNaN() || Definition.EndOffset.ContainsNaN() || !FMath::IsFinite(Definition.Radius)
					|| Definition.Radius <= 0.f)
				{
					DebugReason = TEXT("invalid_capsule_geometry");
					return;
				}
				for (const FResolvedChain& Chain : Chains) {
					if (IsInSubtree(Start, Chain.Bones[0], Parents) || IsInSubtree(End, Chain.Bones[0], Parents)) {
						DebugReason = TEXT("capsule_driver_inside_corrected_subtree");
						return;
					}
				}
				KawaiiPhysicsIKCorrection::FCapsule& Capsule = Capsules.AddDefaulted_GetRef();
				Capsule.Start = DriverCS[Start].TransformPosition(Definition.StartOffset);
				Capsule.End = DriverCS[End].TransformPosition(Definition.EndOffset);
				Capsule.Radius = Definition.Radius * FMath::Max(DriverCS[Start].GetScale3D().GetAbsMax(), DriverCS[End].GetScale3D().GetAbsMax());
			}
		}
	}

	TArray<uint8> Affected;
	Affected.Init(0, NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone) {
		for (const FResolvedChain& Chain : Chains) {
			if (IsInSubtree(Bone, Chain.Bones[0], Parents)) {
				Affected[Bone] = 1;
				break;
			}
		}
	}
	// Restore the baked subtree under its live parent exactly once. Nothing is written
	// to Output until every chain and transform has passed validation.
	TArray<FTransform> BaselineCS = DriverCS;
	for (int32 Bone = 0; Bone < NumBones; ++Bone) {
		if (Affected[Bone]) {
			BaselineCS[Bone] = BakedContext.Pose[FCompactPoseBoneIndex(Bone)] * BaselineCS[Parents[Bone]];
			BaselineCS[Bone].NormalizeRotation();
		}
	}
	TArray<FTransform> CorrectedCS = BaselineCS;
	TArray<uint8> ExplicitTransform;
	ExplicitTransform.Init(0, NumBones);
	const float Strength = FMath::Clamp(Alpha, 0.f, 1.f);

	TSharedPtr<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe> GPUFrame;
	TArray<int32> CompactToPoint;
	bool bGPUChainsCompatible = Strength > 0.f;
	for (const FResolvedChain& Chain : Chains) {
		bGPUChainsCompatible &= Chain.Bones.Num() <= int32(KawaiiPhysicsGPU::MaxChainPoints);
	}
	if (bGPUChainsCompatible && GPUState && GPUState->CanSubmit(this)) {
		GPUFrame = MakeShared<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe>();
		GPUFrame->DeltaTime = DeltaTime;
		GPUFrame->SettleTime = ValidTimeConstant(SnapshotSettleTime);
		GPUFrame->SmoothingTime = ValidTimeConstant(SnapshotSmoothingTime);
		CompactToPoint.Init(INDEX_NONE, NumBones);
		TemporalStates.Reset();
	} else if (GPUState) {
		GPUState->Invalidate(this);
	}

	// Temporal stability. History is stored relative to each chain's live anchor, so body motion never lags.
	const float SettleTime = ValidTimeConstant(SnapshotSettleTime);
	const float SmoothingTime = ValidTimeConstant(SnapshotSmoothingTime);
	const bool bTemporalEnabled = !GPUFrame && (SettleTime > 0.f || SmoothingTime > 0.f);
	if (!bTemporalEnabled || DeltaTime > MaxTemporalGapSeconds) {
		TemporalStates.Reset();
	}
	// Without reported time (direct callers, re-evaluation without update) the solve is stateless and history is kept.
	const bool bAdvanceHistory = bTemporalEnabled && DeltaTime > 0.f && DeltaTime <= MaxTemporalGapSeconds;
	const double SettleAlpha = SettleTime > 0.f ? 1.0 - FMath::Exp(-static_cast<double>(DeltaTime) / SettleTime) : 1.0;
	const double SmoothingAlpha = SmoothingTime > 0.f ? 1.0 - FMath::Exp(-static_cast<double>(DeltaTime) / SmoothingTime) : 1.0;
	if (bAdvanceHistory && TemporalStates.Num() != Chains.Num()) {
		TemporalStates.SetNum(Chains.Num());
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_SolveAndReconstruct);
		TArray<KawaiiPhysicsIKCorrection::FCapsule> ChainCapsules;
		ChainCapsules.Reserve(Capsules.Num() + KawaiiClothingProxy::MaxShapes);
		for (int32 ChainIndex = 0; ChainIndex < Chains.Num(); ++ChainIndex) {
			const FResolvedChain& Chain = Chains[ChainIndex];
			ChainCapsules.Reset();
			ChainCapsules.Append(Capsules);
			if (!KawaiiPhysicsIKCorrection::AppendProxyCapsules(ExternalCapsules, ChainCapsules, Chain.RootName)) {
				DebugReason = TEXT("invalid_external_proxy_capsules");
				return;
			}
			TArray<FVector> Points, ReferencePoints;
			TArray<double> Lengths;
			TArray<FTransform> Warped;
			const int32 Count = Chain.Bones.Num();
			for (int32 Index = 0; Index < Count; ++Index) {
				const int32 Bone = Chain.Bones[Index];
				const float Weight = Chain.Weight * float(Index) / float(Count - 1);
				Warped.Add(KawaiiPhysicsIKCorrection::WarpTransform(
					BakedCS[Bone],
					BakedCS[Chain.Anchor],
					DriverCS[Chain.Anchor],
					BakedCS[Chain.Driver],
					DriverCS[Chain.Driver],
					Weight
				));
				Points.Add(Warped.Last().GetTranslation());
				ReferencePoints.Add(BaselineCS[Bone].GetTranslation());
				// Preserve baked local proportions under the current anchor's scale.
				if (Index > 0) {
					Lengths.Add(FVector::Distance(BaselineCS[Bone].GetTranslation(), BaselineCS[Chain.Bones[Index - 1]].GetTranslation()));
				}
			}
			// Keep the root fixed to the live anchor even when driver and anchor move in opposite directions.
			Points[0] = ReferencePoints[0];
			if (GPUFrame) {
				using namespace KawaiiPhysicsGPU;
				FChain& GPUChain = GPUFrame->Chains.AddDefaulted_GetRef();
				const int32 FirstPoint = GPUFrame->Points.Num() / PointStride;
				GPUChain.Range = FUintVector4(FirstPoint, Count, GPUFrame->Capsules.Num() / 2, ChainCapsules.Num());
				GPUChain.Parameters = FVector4f(Chain.Radius, Strength, SnapshotIterations, 0.f);
				const FTransform& Anchor = DriverCS[Chain.Anchor];
				GPUChain.AnchorRotation = GPUQuaternion(Anchor.GetRotation());
				GPUChain.AnchorPosition = FVector4f(FVector3f(Anchor.GetTranslation()), 0.f);
				GPUChain.AnchorScale = FVector4f(FVector3f(Anchor.GetScale3D()), 0.f);
				GPUFrame->Topology = HashCombineFast(GPUFrame->Topology, GetTypeHash(Chain.Anchor));
				GPUFrame->Topology = HashCombineFast(GPUFrame->Topology, GetTypeHash(Chain.Driver));
				GPUFrame->Topology = HashCombineFast(GPUFrame->Topology, GetTypeHash(Chain.Weight));
				GPUFrame->Topology = HashCombineFast(GPUFrame->Topology, GetTypeHash(Chain.Radius));
				for (int32 Index = 0; Index < Count; ++Index) {
					const FTransform& Baseline = BaselineCS[Chain.Bones[Index]];
					if (!FiniteTransform(Warped[Index])) {
						DebugReason = TEXT("nonfinite_gpu_input");
						return;
					}
					GPUFrame->Points.Add(FVector4f(FVector3f(Points[Index]), Index > 0 ? float(Lengths[Index - 1]) : 0.f));
					GPUFrame->PointBones.Add(RequiredBones.MakeMeshPoseIndex(FCompactPoseBoneIndex(Chain.Bones[Index])).GetInt());
					GPUFrame->Points.Add(FVector4f(FVector3f(ReferencePoints[Index]), 0.f));
					GPUFrame->Points.Add(GPUQuaternion(Baseline.GetRotation()));
					GPUFrame->Points.Add(FVector4f(FVector3f(Baseline.GetScale3D()), 0.f));
					GPUFrame->Points.Add(GPUQuaternion(Warped[Index].GetRotation()));
					GPUFrame->Points.Add(FVector4f(FVector3f(Warped[Index].GetScale3D()), 0.f));
					CompactToPoint[Chain.Bones[Index]] = FirstPoint + Index;
				}
				for (const auto& Capsule : ChainCapsules) {
					GPUFrame->Capsules.Add(FVector4f(FVector3f(Capsule.Start), float(Capsule.Radius)));
					GPUFrame->Capsules.Add(FVector4f(FVector3f(Capsule.End), 0.f));
				}
				continue;
			}
			// Driver-warped input before contacts: the pose temporal settling and smoothing return to.
			const TArray<FVector> TargetPoints = Points;
			const FTransform& AnchorCS = DriverCS[Chain.Anchor];
			FChainTemporalState* History = bAdvanceHistory ? &TemporalStates[ChainIndex] : nullptr;
			if (History
				&& (History->Bones != Chain.Bones || History->Anchor != Chain.Anchor || History->SolvedLocal.Num() != Count
					|| History->SmoothedOffsetLocal.Num() != Count))
			{
				*History = FChainTemporalState();
			}
			const bool bHasHistory = History && !History->SolvedLocal.IsEmpty();
			TArray<FVector> SidePoints;
			if (bHasHistory && SettleTime > 0.f) {
				SidePoints.SetNumUninitialized(Count);
				for (int32 Index = 0; Index < Count; ++Index) {
					SidePoints[Index] = AnchorCS.TransformPosition(History->SolvedLocal[Index]);
				}
				// Start from the previous corrected chain and settle back toward the input pose. A stateless
				// solve can push a strand out of either side of a capsule and flips between frames.
				for (int32 Index = 1; Index < Count; ++Index) {
					Points[Index] = FMath::Lerp(SidePoints[Index], TargetPoints[Index], SettleAlpha);
				}
			}
			const KawaiiPhysicsIKCorrection::FDiagnostics Diagnostics = KawaiiPhysicsIKCorrection::SolveChain(
				Points,
				Lengths,
				ReferencePoints,
				ChainCapsules,
				SnapshotIterations,
				Chain.Radius,
				SidePoints.IsEmpty() ? nullptr : &SidePoints
			);
			if (!Diagnostics.bFinite) {
				DebugReason = TEXT("nonfinite_chain_solution");
				return;
			}
			if (History) {
				History->Bones = Chain.Bones;
				History->Anchor = Chain.Anchor;
				History->SolvedLocal.SetNumUninitialized(Count);
				for (int32 Index = 0; Index < Count; ++Index) {
					History->SolvedLocal[Index] = AnchorCS.InverseTransformPosition(Points[Index]);
				}
				if (!bHasHistory) {
					History->SmoothedOffsetLocal.Init(FVector::ZeroVector, Count);
				}
				const bool bSmooth = bHasHistory && SmoothingTime > 0.f;
				for (int32 Index = 1; Index < Count; ++Index) {
					const FVector OffsetLocal = AnchorCS.InverseTransformVector(Points[Index] - TargetPoints[Index]);
					FVector& SmoothedOffset = History->SmoothedOffsetLocal[Index];
					SmoothedOffset = bSmooth ? FMath::Lerp(SmoothedOffset, OffsetLocal, SmoothingAlpha) : OffsetLocal;
					if (bSmooth) {
						Points[Index] = TargetPoints[Index] + AnchorCS.TransformVector(SmoothedOffset);
					}
				}
				// Smoothing each point independently would stretch the strand; keep the baked bone lengths.
				if (bSmooth) {
					RestoreChainLengths(Points, Lengths, TargetPoints);
				}
			}
			FQuat PreviousSwing = FQuat::Identity;
			for (int32 Index = 0; Index < Count; ++Index) {
				FTransform Solved = Warped[Index];
				Solved.SetTranslation(Points[Index]);
				if (Index + 1 < Count) {
					const FVector OldDirection = Warped[Index + 1].GetTranslation() - Warped[Index].GetTranslation();
					const FVector NewDirection = Points[Index + 1] - Points[Index];
					if (!OldDirection.IsNearlyZero() && !NewDirection.IsNearlyZero()) {
						PreviousSwing = FQuat::FindBetweenNormals(OldDirection.GetSafeNormal(), NewDirection.GetSafeNormal());
					} else {
						PreviousSwing = FQuat::Identity;
					}
				}
				// The tip has no outgoing edge; carry its incoming segment's swing.
				Solved.SetRotation(PreviousSwing * Solved.GetRotation());
				Solved.NormalizeRotation();
				const int32 Bone = Chain.Bones[Index];
				CorrectedCS[Bone].Blend(BaselineCS[Bone], Solved, Strength);
				CorrectedCS[Bone].NormalizeRotation();
				ExplicitTransform[Bone] = 1;
			}
			// Report the pose actually emitted after Alpha, not the stronger unblended solution.
			for (int32 Index = 1; Index < Count; ++Index) {
				const FVector A = CorrectedCS[Chain.Bones[Index - 1]].GetTranslation();
				const FVector B = CorrectedCS[Chain.Bones[Index]].GetTranslation();
				MaxLengthError = FMath::Max(MaxLengthError, FMath::Abs(FVector::Distance(A, B) - Lengths[Index - 1]));
				for (const KawaiiPhysicsIKCorrection::FCapsule& Capsule : ChainCapsules) {
					FVector OnChain, OnCapsule;
					FMath::SegmentDistToSegmentSafe(A, B, Capsule.Start, Capsule.End, OnChain, OnCapsule);
					MaxPenetration = FMath::Max(MaxPenetration, Capsule.Radius + Chain.Radius - FVector::Distance(OnChain, OnCapsule));
				}
			}
		}
	}
	// Side branches and descendants below the tip keep their baked local animation.
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_WriteOutputPose);
	TArray<FTransform> FinalLocal;
	FinalLocal.SetNum(NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone) {
		if (!Affected[Bone]) {
			FinalLocal[Bone] = Output.Pose[FCompactPoseBoneIndex(Bone)];
			continue;
		}
		if (!ExplicitTransform[Bone]) {
			CorrectedCS[Bone] = BakedContext.Pose[FCompactPoseBoneIndex(Bone)] * CorrectedCS[Parents[Bone]];
		}
		CorrectedCS[Bone].NormalizeRotation();
		FinalLocal[Bone] = CorrectedCS[Bone].GetRelativeTransform(CorrectedCS[Parents[Bone]]);
		FinalLocal[Bone].NormalizeRotation();
		if (!FiniteTransform(FinalLocal[Bone])) {
			DebugReason = TEXT("nonfinite_corrected_pose");
			return;
		}
	}
	if (GPUFrame) {
		GPUFrame->BoneToPoint.Init(INDEX_NONE, RequiredBones.GetReferenceSkeleton().GetNum());
		for (int32 Bone = 0; Bone < NumBones; ++Bone) {
			if (CompactToPoint[Bone] == INDEX_NONE && Parents[Bone] != INDEX_NONE) {
				CompactToPoint[Bone] = CompactToPoint[Parents[Bone]];
			}
			const int32 MeshBone = RequiredBones.MakeMeshPoseIndex(FCompactPoseBoneIndex(Bone)).GetInt();
			if (GPUFrame->BoneToPoint.IsValidIndex(MeshBone)) {
				GPUFrame->BoneToPoint[MeshBone] = CompactToPoint[Bone];
			}
		}
		for (int32 Point : GPUFrame->BoneToPoint) {
			GPUFrame->Topology = HashCombineFast(GPUFrame->Topology, GetTypeHash(Point));
		}
		if (!GPUState->Submit(this, GPUFrame)) {
			DebugReason = TEXT("gpu_submission_unavailable");
			return;
		}
	}
	for (int32 Bone = 0; Bone < NumBones; ++Bone) {
		if (Affected[Bone]) {
			Output.Pose[FCompactPoseBoneIndex(Bone)] = FinalLocal[Bone];
		}
	}
	bWroteCorrection = true;
	bGPUCorrectionRequested = bGPUChainsCompatible;
	if (GPUState && bGPUCorrectionRequested) {
		GPUState->RequestCorrection(this);
	}
	if (GPUFrame) {
		DebugReason = TEXT("gpu_visual_correction_cpu_pose_is_baseline");
		return;
	}
	DebugReason = Strength < 1.f ? TEXT("corrected_alpha_reduces_collision_strength")
								 : (MaxPenetration > 0.01 || MaxLengthError > 0.01 ? TEXT("corrected_with_constraint_residual") : TEXT("corrected"));
}

void FAnimNode_KawaiiPhysicsIKCorrection::GatherDebugData(FNodeDebugData& DebugData) {
	DebugData.AddDebugItem(
		FString::Printf(TEXT("%s (%s, penetration %.3f cm, length error %.3f cm)"), *DebugData.GetNodeName(this), *DebugReason, MaxPenetration, MaxLengthError)
	);
	BakedPose.GatherDebugData(DebugData.BranchFlow(1.f));
	DriverPose.GatherDebugData(DebugData.BranchFlow(1.f));
}
