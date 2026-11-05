#pragma once

#include "CoreMinimal.h"

#include "KawaiiClothingProxyMotion.h"

struct FCompactPose;

namespace KawaiiClothingProxy {
	/** Current target component-space pose, excluding subtrees modified by the correction itself. */
	struct KAWAIIPHYSICSBAKERRUNTIME_API FLiveBodyPose
	{
		FTransform Root = FTransform::Identity;
		TMap<FName, FTransform> Bones;
	};

	KAWAIIPHYSICSBAKERRUNTIME_API bool BuildLiveBodyPose(const FCompactPose& Pose, const TArray<FName>& ExcludedRoots, FLiveBodyPose& Out, FString& Error);

	/** Owned values only. Build on the game thread, then publish as immutable data to animation workers.
	 * SourceAnimation and BakedAnimation are identities, never instructions to load or decode an asset.
	 * A snapshot remains valid independently of the source UObject's lifetime. Its owner must rebuild it
	 * when the source tracks, definition geometry or skeleton change. */
	struct KAWAIIPHYSICSBAKERRUNTIME_API FSnapshot
	{
		FSoftObjectPath SourceAnimation;
		FSoftObjectPath BakedAnimation;
		FSoftObjectPath BodyBakedAnimation;
		FGuid MotionRevision;
		FSoftObjectPath DefinitionIdentity;
		FSoftObjectPath SourceMesh;
		FString DefinitionSignature;
		FTransform RelativeTransform = FTransform::Identity;
		int32 SampleRate = 0;
		int32 NumFrames = 0;
		float SourceDuration = 0.f;
		TArray<FName> BoneNames;
		TArray<int32> ParentIndices;
		TArray<FTransform> LocalKeys;
		TArray<FKawaiiClothingProxyShape> Shapes;
		TArray<FName> IgnoredTargetRootBones;

		/** Matches the source sequence, this garment's baked sequence or its explicitly recorded Main sequence.
		 * An absent Main alias is never inferred from naming conventions. */
		bool MatchesAnimation(const FSoftObjectPath& Animation) const;
	};

	struct KAWAIIPHYSICSBAKERRUNTIME_API FWeightedSnapshot
	{
		const FSnapshot* Motion = nullptr;
		float Time = 0.f;
		float Weight = 1.f;
		/** Optional game-thread-captured rule values. Null uses the snapshot's original rules.
		 * The owner must keep this array immutable and alive until sampling finishes. All active
		 * samples in a blend must have the same effective rules. Allows rule updates without a track copy. */
		const TArray<FName>* IgnoredTargetRootBones = nullptr;
	};

	/** Game thread only. Performs full asset validation, then copies tracks once. Clears Out on failure. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool BuildSnapshot(const UKawaiiClothingProxyMotion& Motion, FSnapshot& Out, FString& Error);

	/** Any thread. Reads only caller-owned values, never resolves paths or accesses UObjects.
	 * Matches SampleBlended's local interpolation, normalized mixing, hierarchy and target conversion.
	 * Zero-weight samples are ignored; any invalid active sample atomically clears OutCapsules.
	 * OutRecordedRoot optionally receives the blended proxy root bone (BoneNames[0], the garment mesh root)
	 * in target space. Tracks are baked without root motion extraction, so this root still carries the
	 * action's root motion even when the live pose has it extracted.
	 * When LiveBodyPose is supplied, each endpoint follows its nearest shared body ancestor. Baked garment
	 * motion relative to that ancestor is retained, while current body IK and root extraction are applied.
	 * Endpoints without a shared ancestor fall back to LiveBodyPose.Root. Do not reanchor again afterwards. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool SampleSnapshots(const TArray<FWeightedSnapshot>& Samples,
													   const FTransform& ProxyToTarget,
													   TArray<FKawaiiClothingProxyCapsule>& OutCapsules,
													   FString& Error,
													   FTransform* OutRecordedRoot = nullptr,
													   const FLiveBodyPose* LiveBodyPose = nullptr);

	/** Any thread. Rigidly moves capsules sampled relative to RecordedRoot so they follow LiveRoot instead.
	 * Scale is ignored on both roots. Identical roots leave the capsules unchanged. */
	KAWAIIPHYSICSBAKERRUNTIME_API void ReanchorCapsules(TArray<FKawaiiClothingProxyCapsule>& InOutCapsules,
														const FTransform& RecordedRoot,
														const FTransform& LiveRoot);
}
