#include "AnimNode_KawaiiAutoClothingProxy.h"

#include "Animation/AnimInstanceProxy.h"
#include "KawaiiClothingProxyActionTracking.h"
#include "Misc/ScopeExit.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void FAnimNode_KawaiiAutoClothingProxy::PreUpdate(const UAnimInstance* Instance)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_AutoProxy_PreUpdate);
	Correction.Profile = Profile;
	Correction.PreUpdate(Instance);
	SnapshotSet = ProxyMotionSet;
	SnapshotInputMotions = ProxyMotions;
	SnapshotError.Reset();
	IgnoredRoots.Reset();
	TArray<TObjectPtr<UKawaiiClothingProxyMotion>> Assets = ProxyMotionSet ? ProxyMotionSet->Motions : ProxyMotions;
	if (ProxyMotionSet && !ProxyMotions.IsEmpty())
	{
		SnapshotError = TEXT("Choose either Proxy Motion Set or Proxy Motions, not both.");
	}
	if (Assets.Num() > 256)
	{
		SnapshotError = TEXT("Automatic proxy lists support at most 256 motion assets.");
	}
	bool Rebuild = Assets != SnapshotAssets || Tracks.Num() != Assets.Num();
	int64 Keys = 0;
	TMap<const UKawaiiClothingProxyAsset*, FString> DefinitionSignatures;
	for (int32 Index = 0; Index < Assets.Num() && SnapshotError.IsEmpty(); ++Index)
	{
		const UKawaiiClothingProxyMotion* Motion = Assets[Index];
		if (!IsValid(Motion) || !IsValid(Motion->Definition))
		{
			SnapshotError = TEXT("The current clothing list contains an unloaded or invalid motion/definition.");
			break;
		}
		Keys += Motion->LocalKeys.Num();
		if (Keys > KawaiiClothingProxy::MaxKeys)
		{
			SnapshotError = TEXT("Automatic proxy snapshots exceed the 4,000,000 total bone-key budget.");
			break;
		}
		FString* Signature = DefinitionSignatures.Find(Motion->Definition);
		if (!Signature)
		{
			if (!Motion->Definition->Validate(SnapshotError)) { break; }
			Signature = &DefinitionSignatures.Add(Motion->Definition, Motion->Definition->ComputeSignature());
		}
		if (*Signature != Motion->DefinitionSignature)
		{
			SnapshotError = TEXT("The clothing proxy geometry changed. Rebake the garment motions.");
			break;
		}
		IgnoredRoots.Add(Motion->Definition->IgnoredTargetRootBones);
		if (!Rebuild)
		{
			const auto& Cached = *Tracks[Index];
			Rebuild = Cached.DefinitionSignature != Motion->DefinitionSignature
				|| Cached.DefinitionIdentity != FSoftObjectPath(Motion->Definition.Get())
				|| Cached.SourceMesh != Motion->SourceMesh.ToSoftObjectPath()
				|| Cached.NumFrames != Motion->NumFrames || Cached.SampleRate != Motion->SampleRate
				|| Cached.SourceDuration != Motion->SourceDuration || Cached.LocalKeys.Num() != Motion->LocalKeys.Num()
				|| !Cached.RelativeTransform.Equals(Motion->RelativeTransform)
				|| Cached.SourceAnimation != Motion->SourceAnimation.ToSoftObjectPath()
				|| Cached.BakedAnimation != Motion->BakedAnimation.ToSoftObjectPath()
				|| Cached.BodyBakedAnimation != Motion->BodyBakedAnimation.ToSoftObjectPath();
		}
	}
	if (!SnapshotError.IsEmpty())
	{
		Tracks.Reset(); ActionMap.Reset(); SnapshotAssets.Reset(); IgnoredRoots.Reset();
		return;
	}
	if (!Rebuild) { return; }
	Tracks.Reset(); ActionMap.Reset(); SnapshotAssets = Assets;
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		auto Track = MakeShared<KawaiiClothingProxy::FSnapshot, ESPMode::ThreadSafe>();
		if (!KawaiiClothingProxy::BuildSnapshot(*Assets[Index], *Track, SnapshotError)) { break; }
		if (!Tracks.IsEmpty())
		{
			const auto& First = *Tracks[0];
			if (First.DefinitionIdentity != Track->DefinitionIdentity || First.DefinitionSignature != Track->DefinitionSignature
				|| First.SourceMesh != Track->SourceMesh || First.BoneNames != Track->BoneNames || First.ParentIndices != Track->ParentIndices
				|| !First.RelativeTransform.Equals(Track->RelativeTransform, 1.e-6))
			{
				SnapshotError = TEXT("The action list must contain compatible motions for one garment definition, skeleton and placement.");
				break;
			}
		}
		for (const FSoftObjectPath& Path : {Track->SourceAnimation, Track->BakedAnimation, Track->BodyBakedAnimation})
		{
			if (Path.IsNull()) { continue; }
			const int32* Existing = ActionMap.Find(Path);
			if (Existing && *Existing != Index)
			{
				SnapshotError = FString::Printf(TEXT("Duplicate clothing action mapping: %s"), *Path.ToString());
				break;
			}
			ActionMap.Add(Path, Index);
		}
		if (!SnapshotError.IsEmpty()) { break; }
		Tracks.Add(Track);
	}
	if (!SnapshotError.IsEmpty()) { Tracks.Reset(); ActionMap.Reset(); }
}

void FAnimNode_KawaiiAutoClothingProxy::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	GetEvaluateGraphExposedInputs().Execute(Context);
	SourcePose.Initialize(Context);
	TrackingSerial = 0;
	Correction.ResetTemporalState();
}

void FAnimNode_KawaiiAutoClothingProxy::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	SourcePose.CacheBones(Context);
	// The embedded correction never receives its own CacheBones; LOD changes invalidate its compact indices.
	Correction.ResetTemporalState();
}

void FAnimNode_KawaiiAutoClothingProxy::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	GetEvaluateGraphExposedInputs().Execute(Context);
	// The embedded correction is driven through CorrectPose, so it only learns the frame time from here.
	Correction.AccumulateDeltaTime(Context.GetDeltaTime());
	ScopeWeight = Context.GetFinalBlendWeight();
	UE::Anim::TScopedGraphMessage<KawaiiClothingProxy::FActionTrackingScope> Scope(Context, Context, this, ++TrackingSerial);
	SourcePose.Update(Context);
}

void FAnimNode_KawaiiAutoClothingProxy::Evaluate_AnyThread(FPoseContext& Output)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_AutoProxy_Evaluate);
	SourcePose.Evaluate(Output);
	Correction.ExternalCapsules.Reset();
	ActiveActions = CapsuleCount = 0;
	RootReanchorDistance = 0.f;
	// A skipped frame emits the uncorrected input, so the next solve must not continue from older history.
	bool bCorrectionRan = false;
	ON_SCOPE_EXIT
	{
		if (!bCorrectionRan)
		{
			Correction.ResetTemporalState();
		}
	};
	// Exposed pins run after PreUpdate. Never apply the previous garment during that transition.
	if (SnapshotSet != ProxyMotionSet || SnapshotInputMotions != ProxyMotions || Correction.Profile != Profile)
	{
		DebugReason = TEXT("clothing_list_changed_waiting_for_game_thread_snapshot");
		return;
	}
	if (!SnapshotError.IsEmpty()) { DebugReason = SnapshotError; return; }
	if (Tracks.IsEmpty()) { DebugReason = TEXT("no_clothing_proxy_assets"); return; }
	TArray<KawaiiClothingProxy::FTrackedAction> Actions;
	if (!KawaiiClothingProxy::CollectActions(*Output.AnimInstanceProxy, this, TrackingSerial, ScopeWeight, MontageSlot, Actions, DebugReason)) { return; }
	TArray<KawaiiClothingProxy::FWeightedSnapshot> Samples;
	for (const auto& Action : Actions)
	{
		const int32* Index = ActionMap.Find(Action.Animation);
		if (!Index)
		{
			DebugReason = FString::Printf(TEXT("No clothing proxy mapping for %s. Bake this action or upgrade old baked-body playback links."), *Action.Animation.ToString());
			return;
		}
		KawaiiClothingProxy::FWeightedSnapshot& Sample = Samples.AddDefaulted_GetRef();
		Sample.Motion = Tracks[*Index].Get();
		Sample.Time = Action.Time;
		Sample.Weight = Action.Weight;
		Sample.IgnoredTargetRootBones = &IgnoredRoots[*Index];
	}
	const FTransform Space = bUseBakedRelativeTransform ? Samples[0].Motion->RelativeTransform : ProxyToTarget;
	FTransform RecordedRoot;
	if (!KawaiiClothingProxy::SampleSnapshots(Samples, Space, Correction.ExternalCapsules, DebugReason, &RecordedRoot)) { return; }
	if (bFollowLiveRoot && Output.Pose.GetNumBones() > 0)
	{
		// Root motion always lives on the skeleton root. Compare both roots in the same garment-to-target
		// mapping so a non-identity placement is preserved instead of being cancelled out.
		FTransform LiveRoot = Output.Pose[FCompactPoseBoneIndex(0)] * Space;
		LiveRoot.NormalizeRotation();
		if (LiveRoot.ContainsNaN() || RecordedRoot.ContainsNaN())
		{
			DebugReason = TEXT("invalid_root_for_proxy_reanchor");
			return;
		}
		RootReanchorDistance = FVector::Distance(RecordedRoot.GetTranslation(), LiveRoot.GetTranslation());
		KawaiiClothingProxy::ReanchorCapsules(Correction.ExternalCapsules, RecordedRoot, LiveRoot);
	}
	ActiveActions = Samples.Num();
	CapsuleCount = Correction.ExternalCapsules.Num();
	Correction.Alpha = Alpha;
	FPoseContext Baseline(Output);
	Baseline.Pose.CopyBonesFrom(Output.Pose);
	Correction.CorrectPose(Output, Baseline);
	bCorrectionRan = true;
	DebugReason = Correction.GetCorrectionDebugReason();
}

void FAnimNode_KawaiiAutoClothingProxy::GatherDebugData(FNodeDebugData& DebugData)
{
	DebugData.AddDebugItem(FString::Printf(TEXT("%s (%s; actions %d; capsules %d; root reanchor %.1f cm)"), *DebugData.GetNodeName(this), *DebugReason, ActiveActions, CapsuleCount, RootReanchorDistance));
	SourcePose.GatherDebugData(DebugData.BranchFlow(1.f));
}
