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
		FVector BoxExtent = FVector::ZeroVector;
		FQuat BoxRotation = FQuat::Identity;
		bool IsBox() const { return !BoxExtent.IsZero(); }
	};
	/** Signed distance to a primitive's surface. Negative is inside. */
	KAWAIIPHYSICSBAKERRUNTIME_API double SurfaceDistance(const FVector& Point, const FCapsule& Shape);
	/** Exact sphere/box depenetration, including an embedded sphere center. */
	KAWAIIPHYSICSBAKERRUNTIME_API FVector BoxPush(const FVector& Point, const FCapsule& Box, double Radius);
	struct FContactBridge {
		int32 A = 0, B = 0;
		bool FixedA = false, FixedB = false;
		double RadiusA = 0., RadiusB = 0., Feedback = 1.;
		int32 Samples = 3;
		TArray<FCapsule> Shapes;
	};
	/** One deterministic collision feedback pass. Caller restores each chain's lengths/contacts afterwards. */
	KAWAIIPHYSICSBAKERRUNTIME_API void ProjectBridges(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FContactBridge>& Bridges);
	struct FDiagnostics
	{
		/** Remaining capsule overlap, including an immovable root. In component-space units. */
		double MaxPenetration = 0.0;
		double MaxLengthError = 0.0;
		double MaxAngleViolationDegrees = 0.0;
		double MaxBendViolationDegrees = 0.0;
		/** False means invalid input or arithmetic; positions are restored to the input. */
		bool bFinite = true;
	};
	/** Direction reference is the driver-warped input before history, contacts or smoothing. */
	struct FAngularConstraints {
		TArray<FVector> Pose;
		/** One entry per point; entry i limits edge i-1 -> i. Zero disables that limit. */
		TArray<double> LimitRadians;
		double MaxBendChangeRadians = 0.;
	};

	/** Append value-only proxies for one exact chain root. None bypasses filtering; invalid batches leave OutCapsules unchanged. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool AppendProxyCapsules(const TArray<FKawaiiClothingProxyCapsule>& Proxies,
														   TArray<FCapsule>& OutCapsules,
														   FName TargetRootBone = NAME_None);

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
	 * Optional SidePoints (same count, usually the previous frame's solution in the current anchor
	 * frame) keep each contact on the side of the capsule the point occupied before, so a strand the
	 * input carries across a capsule axis is not pushed out of the opposite surface. A size mismatch
	 * or non-finite history is ignored.
	 * SidePoints also enables swept joint contacts between samples. PreviousCapsules, when supplied
	 * in the same anchor frame, accounts for collider translation/axis rotation before that sweep.
	 * This is a linear relative-motion approximation, not a swept triangle-mesh collision solver.
	 * Alternates length and point/segment capsule constraints, ending with contacts.
	 * Conflicting constraints are reported, not silently described as a complete solve.
	 */
	KAWAIIPHYSICSBAKERRUNTIME_API FDiagnostics SolveChain(TArray<FVector>& Points,
														  const TArray<double>& RestLengths,
														  const TArray<FVector>& ReferencePoints,
														  const TArray<FCapsule>& Capsules,
														  int32 Iterations,
														  double ParticleRadius,
														  const TArray<FVector>* SidePoints = nullptr,
														  const TArray<FCapsule>* PreviousCapsules = nullptr,
														  const TArray<double>* PointRadii = nullptr,
														  const FAngularConstraints* Angular = nullptr);
}
