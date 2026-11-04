#include "KawaiiClothingProxyActionTracking.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"

namespace KawaiiClothingProxy
{
namespace
{
class FActionTrackingContext : public UE::Anim::IAnimNotifyEventContextDataInterface
{
	DECLARE_NOTIFY_CONTEXT_INTERFACE(FActionTrackingContext);
public:
	FActionTrackingContext(const void* InOwner, uint32 InSerial) : Owner(InOwner), Serial(InSerial) {}
	const void* Owner;
	uint32 Serial;
};
IMPLEMENT_NOTIFY_CONTEXT_INTERFACE(FActionTrackingContext);

bool BelongsToScope(const FAnimTickRecord& Record, const void* Owner, uint32 Serial)
{
	if (Record.ContextData.IsValid())
	{
		for (const auto& Data : *Record.ContextData)
		{
			if (Data && Data->Is<FActionTrackingContext>())
			{
				const auto& Scope = Data->As<FActionTrackingContext>();
				return Scope.Owner == Owner && Scope.Serial == Serial;
			}
		}
	}
	return false;
}
}

IMPLEMENT_ANIMGRAPH_MESSAGE(FActionTrackingScope);

FActionTrackingScope::FActionTrackingScope(const FAnimationUpdateContext&, const void* InOwner, uint32 InSerial)
	: Owner(InOwner), Serial(InSerial)
{
}

TUniquePtr<const UE::Anim::IAnimNotifyEventContextDataInterface> FActionTrackingScope::MakeUniqueEventContextData() const
{
	return MakeUnique<FActionTrackingContext>(Owner, Serial);
}

void CaptureMontages(const UAnimInstance& Instance, FTrackedMontageSnapshot& OutSnapshot)
{
	OutSnapshot = FTrackedMontageSnapshot();
	OutSnapshot.bCaptured = true;
	if (!IsInGameThread())
	{
		OutSnapshot.Error = TEXT("Montage action snapshots must be captured on the game thread.");
		return;
	}
	if (Instance.IsUsingMainInstanceMontageEvaluationData())
	{
		OutSnapshot.Error = TEXT("Automatic proxy tracking must run in the main AnimGraph; linked main-instance montage data is not supported.");
		return;
	}
	// Same inclusion rule and timing as UAnimInstance::UpdateMontageEvaluationData.
	for (const FAnimMontageInstance* MontageInstance : Instance.MontageInstances)
	{
		if (!MontageInstance || !MontageInstance->Montage || MontageInstance->GetWeight() <= ZERO_ANIMWEIGHT_THRESH)
		{
			continue;
		}
		for (const FSlotAnimationTrack& Slot : MontageInstance->Montage->SlotAnimTracks)
		{
			FTrackedMontageAction& Entry = OutSnapshot.Actions.AddDefaulted_GetRef();
			Entry.Slot = Slot.SlotName;
			Entry.Action.Weight = MontageInstance->GetWeight();
			if (Slot.AnimTrack.IsAdditive() || MontageInstance->GetActiveBlendProfile())
			{
				Entry.Error = TEXT("Automatic montage proxy tracking requires a non-additive full-body slot without blend profiles.");
				continue;
			}
			const float Position = MontageInstance->GetPosition();
			const FAnimSegment* Segment = Slot.AnimTrack.GetSegmentAtTime(Position);
			UAnimSequence* Sequence = Segment ? Cast<UAnimSequence>(Segment->GetAnimationData(Position, Entry.Action.Time)) : nullptr;
			if (!Sequence || Sequence->IsValidAdditive())
			{
				Entry.Error = TEXT("The active montage segment must contain a non-additive AnimSequence.");
				continue;
			}
			Entry.Action.Animation = FSoftObjectPath(Sequence);
		}
	}
}

bool CollectActions(FAnimInstanceProxy& Proxy, const void* Owner, uint32 Serial, float ScopeWeight,
	FName MontageSlot, TArray<FTrackedAction>& OutActions, FString& Error, const FTrackedMontageSnapshot* SnapshotOverride)
{
	OutActions.Reset();
	Error.Reset();
	if (!Owner || !FMath::IsFinite(ScopeWeight) || ScopeWeight <= ZERO_ANIMWEIGHT_THRESH)
	{
		Error = TEXT("The tracked animation branch has no relevant update weight.");
		return false;
	}

	TArray<FTrackedAction> Pending;
	double TotalWeight = 0.;
	const auto AddSequence = [&](UAnimSequence* Sequence, float Time, float Weight) -> bool
	{
		if (!FMath::IsFinite(Weight) || Weight < 0.f || !FMath::IsFinite(Time))
		{
			Error = TEXT("An animation player supplied an invalid time or weight.");
			return false;
		}
		if (Weight <= ZERO_ANIMWEIGHT_THRESH)
		{
			return true;
		}
		if (!Sequence || Sequence->IsValidAdditive())
		{
			Error = TEXT("Automatic proxy tracking requires non-additive AnimSequence players.");
			return false;
		}
		if (Pending.Num() >= 32)
		{
			Error = TEXT("Automatic proxy tracking supports at most 32 active sequence samples.");
			return false;
		}
		FTrackedAction& Action = Pending.AddDefaulted_GetRef();
		Action.Animation = FSoftObjectPath(Sequence);
		Action.Time = Time;
		Action.Weight = Weight / ScopeWeight;
		TotalWeight += Weight;
		return true;
	};

	const auto AddRecord = [&](const FAnimTickRecord& Record) -> bool
	{
		if (!BelongsToScope(Record, Owner, Serial))
		{
			return true;
		}
		if (!FMath::IsFinite(Record.EffectiveBlendWeight) || Record.EffectiveBlendWeight < 0.f)
		{
			Error = TEXT("An animation tick record has an invalid weight.");
			return false;
		}
		if (Record.EffectiveBlendWeight <= ZERO_ANIMWEIGHT_THRESH)
		{
			return true;
		}
		if (Record.MirrorDataTable || Record.bRequestedInertialization)
		{
			Error = TEXT("Mirrored or inertialized input cannot be reproduced by baked proxy tracks. Place tracking before these modifiers.");
			return false;
		}
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(Record.SourceAsset.Get()))
		{
			if (!Record.TimeAccumulator)
			{
				Error = TEXT("A sequence player has no current-frame time accumulator.");
				return false;
			}
			return AddSequence(Sequence, *Record.TimeAccumulator, Record.EffectiveBlendWeight);
		}
		if (Cast<UBlendSpace>(Record.SourceAsset.Get()))
		{
			if (!Record.BlendSpace.BlendSampleDataCache || Record.BlendSpace.BlendSampleDataCache->IsEmpty())
			{
				Error = TEXT("A BlendSpace has no current-frame sequence samples.");
				return false;
			}
			for (const FBlendSampleData& Sample : *Record.BlendSpace.BlendSampleDataCache)
			{
				if (!Sample.PerBoneBlendData.IsEmpty())
				{
					Error = TEXT("BlendSpace per-bone interpolation is not supported by automatic proxy tracking.");
					return false;
				}
				if (!AddSequence(Sample.Animation.Get(), Sample.Time, Record.EffectiveBlendWeight * Sample.TotalWeight))
				{
					return false;
				}
			}
			return true;
		}
		Error = TEXT("The input contains an unsupported asset player. Use non-additive sequences or BlendSpaces.");
		return false;
	};

	for (const auto& Group : Proxy.GetSyncGroupMapRead())
	{
		for (const FAnimTickRecord& Record : Group.Value.ActivePlayers)
		{
			if (!AddRecord(Record))
			{
				return false;
			}
		}
	}
	for (const FAnimTickRecord& Record : Proxy.GetUngroupedActivePlayersRead())
	{
		if (!AddRecord(Record))
		{
			return false;
		}
	}

	const FTrackedMontageSnapshot* Snapshot = SnapshotOverride ? SnapshotOverride : GetMontageSnapshot(Proxy);
	if (!Snapshot || !Snapshot->bCaptured || !Snapshot->Error.IsEmpty())
	{
		Error = Snapshot && !Snapshot->Error.IsEmpty() ? Snapshot->Error
			: TEXT("Automatic montage tracking is unavailable. Recompile the AnimBlueprint containing this node.");
		return false;
	}
	if (MontageSlot.IsNone() && !Snapshot->Actions.IsEmpty())
	{
		Error = TEXT("An active montage requires the automatic proxy node's full-body Montage Slot to be configured.");
		return false;
	}
	float SlotTotalWeight = 0.f;
	for (const FTrackedMontageAction& Entry : Snapshot->Actions)
	{
		if (Entry.Slot == MontageSlot)
		{
			SlotTotalWeight += Entry.Action.Weight;
		}
	}
	for (const FTrackedMontageAction& Entry : Snapshot->Actions)
	{
		if (Entry.Slot != MontageSlot)
		{
			continue;
		}
		if (!Entry.Error.IsEmpty())
		{
			Error = Entry.Error;
			return false;
		}
		const float Weight = Entry.Action.Weight / FMath::Max(1.f, SlotTotalWeight);
		if (!FMath::IsFinite(Weight) || Weight < 0.f || !FMath::IsFinite(Entry.Action.Time) || Entry.Action.Animation.IsNull())
		{
			Error = TEXT("An automatic montage snapshot contains invalid action data.");
			return false;
		}
		if (Weight <= ZERO_ANIMWEIGHT_THRESH)
		{
			continue;
		}
		if (Pending.Num() >= 32)
		{
			Error = TEXT("Automatic proxy tracking supports at most 32 active sequence samples.");
			return false;
		}
		FTrackedAction& Action = Pending.Add_GetRef(Entry.Action);
		Action.Weight = Weight;
		TotalWeight += Weight * ScopeWeight;
	}

	// Missing cached-pose paths and non-scalar blends must not be silently renormalized.
	if (Pending.IsEmpty() || !FMath::IsNearlyEqual(TotalWeight / ScopeWeight, 1., 0.005))
	{
		Error = TEXT("The tracked branch does not provide a complete full-body action blend. Place this node before shared cached poses and use a single configured full-body slot.");
		return false;
	}
	OutActions = MoveTemp(Pending);
	return true;
}
}
