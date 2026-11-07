#pragma once

#include "CoreMinimal.h"

#include "KawaiiPhysicsIKCorrectionProfile.h"

class UAnimBlueprint;

/** Builds a UKawaiiPhysicsIKCorrectionProfile from the KawaiiPhysics nodes already authored in an AnimBlueprint. */
namespace KawaiiPhysicsIKProfileGenerator {
	/** One KawaiiPhysics root and the bones its node excludes together with their descendants. */
	struct FSimulatedRoot
	{
		FName RootBone;
		TArray<FName> ExcludeBones;
		float CollisionRadius = 0.f;
		FRuntimeFloatCurve RadiusCurve;
		float DummyLength = 0.f;
		float LimitAngle = 0.f;
		FRuntimeFloatCurve LimitAngleCurve;
	};

	/** Hierarchy-only decomposition. Parents[i] < i; INDEX_NONE marks a skeleton root.
	 * Each emitted chain owns a skeleton subtree whose simulated bones form exactly one path, so the
	 * runtime non-overlap, descendant-tip and driver rules hold by construction. Branching bones stay
	 * uncorrected anchors and every branch below them becomes its own chain. A chain is lifted to an
	 * unsimulated single-path ancestor only when that ancestor is an ignored target root (e.g. Tail_Root),
	 * because the runtime matches garment ignore rules against the chain root name exactly. */
	bool BuildChains(const TArray<FName>& BoneNames,
					 const TArray<int32>& Parents,
					 const TArray<FSimulatedRoot>& Roots,
					 const TArray<FName>& IgnoredTargetRoots,
					 TArray<FKawaiiPhysicsIKCorrectionChain>& OutChains,
					 TArray<FString>& OutNotes,
					 FString& Error);

	/** KawaiiPhysics limits are offsets in the driving bone's space with the capsule axis along the offset rotation's Z. */
	FKawaiiPhysicsIKCorrectionCapsule MakeBoneCapsule(FName Bone, const FVector& Center, const FRotator& Rotation, float Radius, float Length);

	struct FOptions
	{
		/** Convert the nodes' outer spherical and capsule limits into body capsules. */
		bool bImportBodyCapsules = true;
		/** Fill empty Profile settings on Automatic Clothing Proxy Correction nodes in the same blueprint. */
		bool bAssignToProxyNodes = true;
		/** Save the profile, and the blueprint when it had no unrelated unsaved edits. */
		bool bSave = true;
		/** Report the result without creating or modifying any asset. */
		bool bDryRun = false;
		/** Long package path for the profile; empty places it beside the AnimBlueprint. */
		FString PackagePath;
	};

	/** Creates DA_<Blueprint>_IKCorrection, or regenerates it in place. Returns nullptr on failure or dry run. */
	UKawaiiPhysicsIKCorrectionProfile* Generate(UAnimBlueprint* Blueprint, const FOptions& Options, FString& OutReport);

	bool HasSelectedAnimBlueprints();
	void GenerateForSelectedAnimBlueprints();
}
