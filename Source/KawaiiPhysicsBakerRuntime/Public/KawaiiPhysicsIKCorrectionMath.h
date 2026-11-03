#pragma once

#include "CoreMinimal.h"

struct FKawaiiClothingProxyCapsule;

/** Stateless, component-space math shared by the runtime node and its tests. */
namespace KawaiiPhysicsIKCorrection {
	struct FCapsule
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		double Radius = 0.0;
	};

	struct FDiagnostics
	{
		/** Remaining capsule overlap, including an immovable root. In component-space units. */
		double MaxPenetration = 0.0;
		double MaxLengthError = 0.0;
		/** False means invalid input or arithmetic; positions are restored to the input. */
		bool bFinite = true;
	};

	/** Append value-only external proxy capsules atomically; invalid input leaves OutCapsules unchanged. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool AppendProxyCapsules(const TArray<FKawaiiClothingProxyCapsule>& Proxies, TArray<FCapsule>& OutCapsules);

	/** Blend anchor-relative and driver-relative mappings of the original baked transform. */
	KAWAIIPHYSICSBAKERRUNTIME_API FTransform WarpTransform(const FTransform& Bone,
														   const FTransform& BaseAnchor,
														   const FTransform& CurrentAnchor,
														   const FTransform& BaseDriver,
														   const FTransform& CurrentDriver,
														   float Weight);

	/**
	 * Point zero stays pinned. RestLengths[i] describes edge i -> i+1.
	 * ReferencePoints supplies deterministic outward directions for degenerate contact.
	 * Alternates length and point/segment capsule constraints, ending with contacts.
	 * Conflicting constraints are reported, not silently described as a complete solve.
	 */
	KAWAIIPHYSICSBAKERRUNTIME_API FDiagnostics SolveChain(TArray<FVector>& Points,
														  const TArray<double>& RestLengths,
														  const TArray<FVector>& ReferencePoints,
														  const TArray<FCapsule>& Capsules,
														  int32 Iterations,
														  double ParticleRadius);
}
