#include "AnimNode_KawaiiAutoClothingProxy.h"

#include "Animation/AnimInstanceProxy.h"
#include "Misc/ScopeExit.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiClothingProxyActionTracking.h"

void FAnimNode_KawaiiAutoClothingProxy::PreUpdate(const UAnimInstance* Instance) {
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_AutoProxy_PreUpdate);
	Correction.Profile = Profile;
	Correction.PreUpdate(Instance);
	SnapshotCorrectionRoots.Reset();
	if (Profile) {
		for (const auto& Chain : Profile->Chains) {
			SnapshotCorrectionRoots.Add(Chain.RootBone.BoneName);
		}
	}
	SnapshotSet = ProxyMotionSet;
	SnapshotInputMotions = ProxyMotions;
	SnapshotError.Reset();
	IgnoredRoots.Reset();
	TArray<TObjectPtr<UKawaiiClothingProxyMotion>> Assets = ProxyMotionSet ? ProxyMotionSet->Motions : ProxyMotions;
	if (ProxyMotionSet && !ProxyMotions.IsEmpty()) {
		SnapshotError = TEXT("Choose either Proxy Motion Set or Proxy Motions, not both.");
	}
	if (Assets.Num() > 256) {
		SnapshotError = TEXT("Automatic proxy lists support at most 256 motion assets.");
	}
	bool Rebuild = Assets != SnapshotAssets || Tracks.Num() != Assets.Num();
	int64 Keys = 0;
	TMap<const UKawaiiClothingProxyAsset*, FString> DefinitionSignatures;
	for (int32 Index = 0; Index < Assets.Num() && SnapshotError.IsEmpty(); ++Index) {
		const UKawaiiClothingProxyMotion* Motion = Assets[Index];
		if (!IsValid(Motion) || !IsValid(Motion->Definition)) {
			SnapshotError = TEXT("The current clothing list contains an unloaded or invalid motion/definition.");
			break;
		}
		Keys += Motion->LocalKeys.Num();
		if (!Motion->ValidateAnimationRevisions(SnapshotError)) { break; }
		if (Keys > KawaiiClothingProxy::MaxKeys) {
			SnapshotError = TEXT("Automatic proxy snapshots exceed the 4,000,000 total bone-key budget.");
			break;
		}
		FString* Signature = DefinitionSignatures.Find(Motion->Definition);
		if (!Signature) {
			if (!Motion->Definition->Validate(SnapshotError)) {
				break;
			}
			Signature = &DefinitionSignatures.Add(Motion->Definition, Motion->Definition->ComputeSignature());
		}
		if (*Signature != Motion->DefinitionSignature) {
			SnapshotError = TEXT("The clothing proxy geometry changed. Rebake the garment motions.");
			break;
		}
		IgnoredRoots.Add(Motion->Definition->IgnoredTargetRootBones);
		if (!Rebuild) {
			const auto& Cached = *Tracks[Index];
			Rebuild = Cached.DefinitionSignature != Motion->DefinitionSignature || Cached.DefinitionIdentity != FSoftObjectPath(Motion->Definition.Get())
				|| Cached.SourceMesh != Motion->SourceMesh.ToSoftObjectPath() || Cached.NumFrames != Motion->NumFrames
				|| Cached.SampleRate != Motion->SampleRate || Cached.SourceDuration != Motion->SourceDuration
				|| Cached.LocalKeys.Num() != Motion->LocalKeys.Num() || !Cached.RelativeTransform.Equals(Motion->RelativeTransform)
				|| Cached.SourceAnimation != Motion->SourceAnimation.ToSoftObjectPath() || Cached.BakedAnimation != Motion->BakedAnimation.ToSoftObjectPath()
				|| Cached.BodyBakedAnimation != Motion->BodyBakedAnimation.ToSoftObjectPath() || Cached.MotionRevision != Motion->MotionRevision;
		}
	}
	if (!SnapshotError.IsEmpty()) {
		Tracks.Reset();
		ActionMap.Reset();
		SnapshotAssets.Reset();
		IgnoredRoots.Reset();
		return;
	}
	if (!Rebuild) {
		return;
	}
	// A new garment/action cache has no continuous collision history with the old one.
	Correction.ResetTemporalState();
	Tracks.Reset();
	ActionMap.Reset();
	SnapshotAssets = Assets;
	for (int32 Index = 0; Index < Assets.Num(); ++Index) {
		auto Track = MakeShared<KawaiiClothingProxy::FSnapshot, ESPMode::ThreadSafe>();
		if (!KawaiiClothingProxy::BuildSnapshot(*Assets[Index], *Track, SnapshotError)) {
			break;
		}
		if (!Tracks.IsEmpty()) {
			const auto& First = *Tracks[0];
			if (First.DefinitionIdentity != Track->DefinitionIdentity || First.DefinitionSignature != Track->DefinitionSignature
				|| First.SourceMesh != Track->SourceMesh || First.BoneNames != Track->BoneNames || First.ParentIndices != Track->ParentIndices
				|| !First.RelativeTransform.Equals(Track->RelativeTransform, 1.e-6))
			{
				SnapshotError = TEXT("The action list must contain compatible motions for one garment definition, skeleton and placement.");
				break;
			}
		}
		for (const FSoftObjectPath& Path : {Track->SourceAnimation, Track->BakedAnimation, Track->BodyBakedAnimation}) {
			if (Path.IsNull()) {
				continue;
			}
			const int32* Existing = ActionMap.Find(Path);
			if (Existing && *Existing != Index) {
				SnapshotError = FString::Printf(TEXT("Duplicate clothing action mapping: %s"), *Path.ToString());
				break;
			}
			ActionMap.Add(Path, Index);
		}
		if (!SnapshotError.IsEmpty()) {
			break;
		}
		Tracks.Add(Track);
	}
	if (!SnapshotError.IsEmpty()) {
		Tracks.Reset();
		ActionMap.Reset();
	}
}

void FAnimNode_KawaiiAutoClothingProxy::Initialize_AnyThread(const FAnimationInitializeContext& Context) {
	FAnimNode_Base::Initialize_AnyThread(Context);
	GetEvaluateGraphExposedInputs().Execute(Context);
	SourcePose.Initialize(Context);
	TrackingSerial = 0;
	Correction.ResetTemporalState();
}

void FAnimNode_KawaiiAutoClothingProxy::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) {
	SourcePose.CacheBones(Context);
	// The embedded correction never receives its own CacheBones; LOD changes invalidate its compact indices.
	Correction.ResetTemporalState();
}

void FAnimNode_KawaiiAutoClothingProxy::Update_AnyThread(const FAnimationUpdateContext& Context) {
	GetEvaluateGraphExposedInputs().Execute(Context);
	// The embedded correction is driven through CorrectPose, so it only learns the frame time from here.
	Correction.AccumulateDeltaTime(Context.GetDeltaTime());
	ScopeWeight = Context.GetFinalBlendWeight();
	UE::Anim::TScopedGraphMessage<KawaiiClothingProxy::FActionTrackingScope> Scope(Context, Context, this, ++TrackingSerial);
	SourcePose.Update(Context);
}

bool FAnimNode_KawaiiAutoClothingProxy::BuildActionSamples(
	const TArray<KawaiiClothingProxy::FTrackedAction>& Actions,
	TArray<KawaiiClothingProxy::FWeightedSnapshot>& Samples) {
	Samples.Reset();
	float MappedWeight = 0.f;
	for (const auto& Action : Actions) {
		const int32* Index = ActionMap.Find(Action.Animation);
		if (!Index || !FAnimWeight::IsRelevant(Action.Weight)) {
			continue;
		}
		KawaiiClothingProxy::FWeightedSnapshot& Sample = Samples.AddDefaulted_GetRef();
		Sample.Motion = Tracks[*Index].Get();
		Sample.Time = Action.Time;
		Sample.Weight = Action.Weight;
		Sample.IgnoredTargetRootBones = &IgnoredRoots[*Index];
		MappedWeight += Action.Weight;
	}
	// Sampling normalizes the mapped subset to produce a valid garment shape. Its influence must
	// still follow the ORIGINAL action weights, including when blending out to an unbaked idle.
	// Tracking errors (missing pose contributions, per-bone blends, etc.) are rejected before here.
	Correction.Alpha = Alpha * FMath::Clamp(MappedWeight, 0.f, 1.f);
	if (Samples.IsEmpty()) {
		DebugReason = TEXT("no_clothing_proxy_mapping_for_current_actions");
		return false;
	}
	return true;
}

void FAnimNode_KawaiiAutoClothingProxy::Evaluate_AnyThread(FPoseContext& Output) {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_AutoProxy_Evaluate);
	SourcePose.Evaluate(Output);
	Correction.ExternalCapsules.Reset();
	ActiveActions = CapsuleCount = 0;
	RootReanchorDistance = 0.f;
	// A skipped frame emits the uncorrected input, so the next solve must not continue from older history.
	bool bCorrectionRan = false;
	ON_SCOPE_EXIT {
		if (!bCorrectionRan) {
			Correction.ResetTemporalState();
			Correction.ReportGPUReason(DebugReason);
		}
	};
	// Exposed pins run after PreUpdate. Never apply the previous garment during that transition.
	if (SnapshotSet != ProxyMotionSet || SnapshotInputMotions != ProxyMotions || Correction.Profile != Profile) {
		DebugReason = TEXT("clothing_list_changed_waiting_for_game_thread_snapshot");
		return;
	}
	if (!SnapshotError.IsEmpty()) {
		DebugReason = SnapshotError;
		return;
	}
	if (Tracks.IsEmpty()) {
		DebugReason = TEXT("no_clothing_proxy_assets");
		return;
	}
	TArray<KawaiiClothingProxy::FTrackedAction> Actions;
	if (!KawaiiClothingProxy::CollectActions(*Output.AnimInstanceProxy, this, TrackingSerial, ScopeWeight, MontageSlot, Actions, DebugReason)) {
		return;
	}
	TArray<KawaiiClothingProxy::FWeightedSnapshot> Samples;
	if (!BuildActionSamples(Actions, Samples)) {
		return;
	}
	const FTransform Space = bUseBakedRelativeTransform ? Samples[0].Motion->RelativeTransform : ProxyToTarget;
	KawaiiClothingProxy::FLiveBodyPose LiveBody;
	const bool bFollowBody = bFollowLiveRoot && bFollowLiveBodyPose;
	if (bFollowBody && !KawaiiClothingProxy::BuildLiveBodyPose(Output.Pose, SnapshotCorrectionRoots, LiveBody, DebugReason)) {
		return;
	}
	FTransform RecordedRoot;
	if (!KawaiiClothingProxy::SampleSnapshots(Samples, Space, Correction.ExternalCapsules, DebugReason, &RecordedRoot, bFollowBody ? &LiveBody : nullptr)) {
		return;
	}
	if (bFollowLiveRoot && Output.Pose.GetNumBones() > 0) {
		// SampleSnapshots has already mapped the recorded garment root into target component space.
		// The live root is already in that space, including root motion extraction. Applying Space
		// again adds the garment placement twice and makes contacts drift as the authored root moves.
		// Motion Warping moves the whole component; its world transform must not be applied here.
		FTransform LiveRoot = Output.Pose[FCompactPoseBoneIndex(0)];
		LiveRoot.NormalizeRotation();
		if (LiveRoot.ContainsNaN() || RecordedRoot.ContainsNaN()) {
			DebugReason = TEXT("invalid_root_for_proxy_reanchor");
			return;
		}
		RootReanchorDistance = FVector::Distance(RecordedRoot.GetTranslation(), LiveRoot.GetTranslation());
		if (!bFollowBody) {
			KawaiiClothingProxy::ReanchorCapsules(Correction.ExternalCapsules, RecordedRoot, LiveRoot);
		}
	}
	ActiveActions = Samples.Num();
	CapsuleCount = Correction.ExternalCapsules.Num();
	FPoseContext Baseline(Output);
	Baseline.Pose.CopyBonesFrom(Output.Pose);
	Correction.CorrectPose(Output, Baseline);
	bCorrectionRan = true;
	DebugReason = Correction.GetCorrectionDebugReason();
}

void FAnimNode_KawaiiAutoClothingProxy::GatherDebugData(FNodeDebugData& DebugData) {
	DebugData.AddDebugItem(FString::Printf(
		TEXT("%s (%s; actions %d; capsules %d; root reanchor %.1f cm; correction alpha %.3f)"),
		*DebugData.GetNodeName(this),
		*DebugReason,
		ActiveActions,
		CapsuleCount,
		RootReanchorDistance,
		Correction.Alpha
	));
	SourcePose.GatherDebugData(DebugData.BranchFlow(1.f));
}
