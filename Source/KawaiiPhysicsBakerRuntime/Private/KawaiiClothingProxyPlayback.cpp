#include "KawaiiClothingProxyPlayback.h"

#include "Animation/AnimSequence.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiPhysicsBakerProfiling.h"

bool UKawaiiClothingProxyPlaybackLibrary::ResolveProxyMotionSamples(const UKawaiiClothingProxyMotionSet* MotionSet, const TArray<FKawaiiClothingProxyActionSample>& Actions, TArray<FKawaiiClothingProxyMotionSample>& OutSamples, FString& Error) {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_ResolveActions);
	SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerProxyResolve);
	OutSamples.Reset();
	Error.Reset();
	if (!IsInGameThread() || !IsValid(MotionSet) || Actions.Num() > 32) {
		Error = TEXT("Resolve Clothing Proxy Actions requires a valid motion set, at most 32 samples, and the game thread.");
		return false;
	}
	TArray<FKawaiiClothingProxyMotionSample> Resolved;
	for (const FKawaiiClothingProxyActionSample& Action : Actions) {
		if (!FMath::IsFinite(Action.Weight) || Action.Weight < 0.f || !FMath::IsFinite(Action.Time)) {
			Error = TEXT("Action time/weight must be finite and weight cannot be negative.");
			return false;
		}
		if (Action.Weight == 0.f) {
			continue;
		}
		if (!IsValid(Action.SourceAnimation)) {
			Error = TEXT("A positive-weight action is missing its source animation.");
			return false;
		}
		UKawaiiClothingProxyMotion* Match = nullptr;
		const FSoftObjectPath SourcePath(Action.SourceAnimation.Get());
		for (UKawaiiClothingProxyMotion* Motion : MotionSet->Motions) {
			if (IsValid(Motion) && Motion->SourceAnimation.ToSoftObjectPath() == SourcePath) {
				if (Match) {
					Error = FString::Printf(TEXT("Duplicate clothing proxy mapping for %s."), *Action.SourceAnimation->GetName());
					return false;
				}
				Match = Motion;
			}
		}
		if (!Match) {
			Error = FString::Printf(TEXT("No clothing proxy motion was baked for %s."), *Action.SourceAnimation->GetName());
			return false;
		}
		FKawaiiClothingProxyMotionSample& Sample = Resolved.AddDefaulted_GetRef();
		Sample.Motion = Match;
		Sample.Time = Action.Time;
		Sample.Weight = Action.Weight;
	}
	OutSamples = MoveTemp(Resolved);
	return true;
}
