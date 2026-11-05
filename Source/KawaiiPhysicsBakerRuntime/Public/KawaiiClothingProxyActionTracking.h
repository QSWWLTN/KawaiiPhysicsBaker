#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeMessages.h"
#include "UObject/SoftObjectPath.h"

struct FAnimationUpdateContext;
struct FAnimInstanceProxy;
class UAnimInstance;

namespace KawaiiClothingProxy {
	/** Identity and sequence-local time read from the current frame's actual asset players. */
	struct FTrackedAction
	{
		FSoftObjectPath Animation;
		float Time = 0.f;
		float Weight = 0.f;
	};

	/** Game-thread snapshot of one montage's contribution to one slot. Contains no object pointers. */
	struct FTrackedMontageAction
	{
		FName Slot;
		FTrackedAction Action;
		FString Error;
	};

	struct FTrackedMontageSnapshot
	{
		TArray<FTrackedMontageAction> Actions;
		FString Error;
		bool bCaptured = false;
	};

	/** Called by the automatically installed anim subsystem immediately after UE has advanced montages. */
	KAWAIIPHYSICSBAKERRUNTIME_API void CaptureMontages(const UAnimInstance& Instance, FTrackedMontageSnapshot& OutSnapshot);
	/** Worker-thread read of the subsystem's owned value snapshot. */
	KAWAIIPHYSICSBAKERRUNTIME_API const FTrackedMontageSnapshot* GetMontageSnapshot(FAnimInstanceProxy& Proxy);

	/** Tag only the input branch whose pose will be matched. Standard asset players preserve
 * this message in their tick records, including delayed Save Cached Pose updates.
 * Use a unique owner plus an incrementing serial on each node update. */
	class KAWAIIPHYSICSBAKERRUNTIME_API FActionTrackingScope : public UE::Anim::IGraphMessage
	{
		DECLARE_ANIMGRAPH_MESSAGE(FActionTrackingScope);

	public:
		FActionTrackingScope(const FAnimationUpdateContext& Context, const void* InOwner, uint32 InSerial);
		virtual TUniquePtr<const UE::Anim::IAnimNotifyEventContextDataInterface> MakeUniqueEventContextData() const override;

	private:
		const void* Owner;
		uint32 Serial;
	};

	/** Call during Evaluate, after the input's update and engine asset-player synchronization.
 * ScopeWeight is the update context's final blend weight. MontageSlot, when set, must be
 * the single full-body Slot in the tracked branch. No montage, asset, or time is inferred
 * from pose similarity. Unsupported/missing contributions fail atomically.
 * Supports standard non-additive sequence players/evaluators, scalar full-body blends and
 * BlendSpaces without per-bone interpolation, plus a configured non-additive montage Slot.
 * Body IK may precede this node: it does not change action identities or clocks. Mirroring,
 * inertialized clothing playback and per-bone action blends are not reproduced. Place the
 * tracking node before a shared Save Cached Pose node so all source contributions retain its tag.
 */
	KAWAIIPHYSICSBAKERRUNTIME_API bool CollectActions(FAnimInstanceProxy& Proxy,
													  const void* Owner,
													  uint32 Serial,
													  float ScopeWeight,
													  FName MontageSlot,
													  TArray<FTrackedAction>& OutActions,
													  FString& Error,
													  const FTrackedMontageSnapshot* SnapshotOverride = nullptr);

}
