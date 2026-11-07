#pragma once

#include "CoreMinimal.h"

class FPrimitiveDrawInterface;
struct FReferenceSkeleton;
struct FKawaiiClothingProxyShape;

namespace KawaiiClothingProxyEditor {

enum class EEditTarget : uint8 { Shape, Start, End };

/** Keeps the two bone spaces separate, including when a capsule spans different bones. */
struct FGeometry
{
	FTransform StartBone;
	FTransform EndBone;
	FTransform ComponentToWorld;
	FVector Start;
	FVector End;
	double Radius = 0.0;
	FVector BoxExtent = FVector::ZeroVector;
	FQuat BoxRotation = FQuat::Identity;
};

bool BuildGeometry(const FKawaiiClothingProxyShape& Shape, const FTransform& StartBone, const FTransform& EndBone,
	const FTransform& ComponentToWorld, FGeometry& Out);
bool ResolveGeometry(const FKawaiiClothingProxyShape& Shape, const FReferenceSkeleton& Skeleton,
	TConstArrayView<FTransform> Pose, const FTransform& ComponentToWorld, FGeometry& Out);
bool Translate(FKawaiiClothingProxyShape& Shape, const FGeometry& Geometry, const FVector& Delta, EEditTarget Target);
/** Local X/Y change radius, local Z changes capsule length; spheres scale their radius on any axis. */
bool Scale(FKawaiiClothingProxyShape& Shape, const FGeometry& Geometry, const FVector& Delta);
void Draw(FPrimitiveDrawInterface* PDI, const FGeometry& Geometry, const FLinearColor& Color, float Thickness = 1.5f);

}
