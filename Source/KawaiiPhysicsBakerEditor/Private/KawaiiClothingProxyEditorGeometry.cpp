#include "KawaiiClothingProxyEditorGeometry.h"

#include "KawaiiClothingProxyAsset.h"
#include "ReferenceSkeleton.h"
#include "SceneManagement.h"

namespace KawaiiClothingProxyEditor {

bool BuildGeometry(const FKawaiiClothingProxyShape& Shape, const FTransform& StartBone, const FTransform& EndBone,
	const FTransform& ComponentToWorld, FGeometry& Out) {
	if (Shape.StartOffset.ContainsNaN() || Shape.EndOffset.ContainsNaN() || !FMath::IsFinite(Shape.Radius) || Shape.Radius <= 0.f
		|| !StartBone.IsValid() || !EndBone.IsValid() || !ComponentToWorld.IsValid()) {
		return false;
	}
	Out.StartBone = StartBone;
	Out.EndBone = EndBone;
	Out.ComponentToWorld = ComponentToWorld;
	Out.Start = ComponentToWorld.TransformPosition(StartBone.TransformPosition(Shape.StartOffset));
	Out.End = ComponentToWorld.TransformPosition(EndBone.TransformPosition(Shape.EndOffset));
	// Match the runtime proxy sampler's conservative radius under nonuniform scale.
	Out.Radius = Shape.Radius * FMath::Max(StartBone.GetMaximumAxisScale(), EndBone.GetMaximumAxisScale())
		* ComponentToWorld.GetMaximumAxisScale();
	Out.BoxExtent = FVector::ZeroVector;
	Out.BoxRotation = FQuat::Identity;
	if (Shape.bBox) {
		if (Shape.BoxExtent.ContainsNaN() || Shape.BoxExtent.GetMin() <= 0. || Shape.BoxRotation.ContainsNaN()) { return false; }
		Out.End = Out.Start;
		Out.BoxExtent = Shape.BoxExtent * StartBone.GetMaximumAxisScale() * ComponentToWorld.GetMaximumAxisScale();
		Out.BoxRotation = (ComponentToWorld.GetRotation() * StartBone.GetRotation() * Shape.BoxRotation.Quaternion()).GetNormalized();
	}
	return !Out.Start.ContainsNaN() && !Out.End.ContainsNaN() && FMath::IsFinite(Out.Radius) && Out.Radius > 0.0;
}

bool ResolveGeometry(const FKawaiiClothingProxyShape& Shape, const FReferenceSkeleton& Skeleton,
	TConstArrayView<FTransform> Pose, const FTransform& ComponentToWorld, FGeometry& Out) {
	const int32 Start = Skeleton.FindBoneIndex(Shape.StartBone);
	const int32 End = Skeleton.FindBoneIndex(Shape.bBox || Shape.EndBone.IsNone() ? Shape.StartBone : Shape.EndBone);
	return Pose.IsValidIndex(Start) && Pose.IsValidIndex(End)
		&& BuildGeometry(Shape, Pose[Start], Pose[End], ComponentToWorld, Out);
}

static bool CanInvert(const FTransform& Transform) {
	return Transform.GetScale3D().GetAbsMin() > UE_SMALL_NUMBER;
}

bool Translate(FKawaiiClothingProxyShape& Shape, const FGeometry& Geometry, const FVector& Delta, EEditTarget Target) {
	if (Delta.ContainsNaN() || Delta.IsNearlyZero() || !CanInvert(Geometry.StartBone)
		|| !CanInvert(Geometry.EndBone) || !CanInvert(Geometry.ComponentToWorld)) {
		return false;
	}
	if (Shape.bBox || Target != EEditTarget::End) {
		Shape.StartOffset = Geometry.StartBone.InverseTransformPosition(Geometry.ComponentToWorld.InverseTransformPosition(Geometry.Start + Delta));
	}
	if (!Shape.bBox && Target != EEditTarget::Start) {
		Shape.EndOffset = Geometry.EndBone.InverseTransformPosition(Geometry.ComponentToWorld.InverseTransformPosition(Geometry.End + Delta));
	}
	return true;
}

bool Scale(FKawaiiClothingProxyShape& Shape, const FGeometry& Geometry, const FVector& Delta) {
	if (Delta.ContainsNaN() || Delta.IsNearlyZero() || !CanInvert(Geometry.StartBone)
		|| !CanInvert(Geometry.EndBone) || !CanInvert(Geometry.ComponentToWorld)) {
		return false;
	}
	if (Shape.bBox) {
		const FVector Before = Shape.BoxExtent;
		Shape.BoxExtent = (Before * (FVector::OneVector + Delta).ComponentMax(FVector(.01))).ComponentMax(FVector(.01));
		return !Shape.BoxExtent.Equals(Before);
	}
	const bool bSphere = Geometry.Start.Equals(Geometry.End);
	double RadiusDelta = FMath::Abs(Delta.X) >= FMath::Abs(Delta.Y) ? Delta.X : Delta.Y;
	if (bSphere && FMath::Abs(Delta.Z) > FMath::Abs(RadiusDelta)) {
		RadiusDelta = Delta.Z;
	}
	const FKawaiiClothingProxyShape Before = Shape;
	Shape.Radius = FMath::Max(0.01f, static_cast<float>(Shape.Radius * FMath::Max(0.01, 1.0 + RadiusDelta)));
	if (!bSphere && !FMath::IsNearlyZero(Delta.Z)) {
		const FVector Center = (Geometry.Start + Geometry.End) * 0.5;
		const FVector HalfAxis = (Geometry.End - Geometry.Start) * (0.5 * FMath::Max(0.01, 1.0 + Delta.Z));
		Shape.StartOffset = Geometry.StartBone.InverseTransformPosition(Geometry.ComponentToWorld.InverseTransformPosition(Center - HalfAxis));
		Shape.EndOffset = Geometry.EndBone.InverseTransformPosition(Geometry.ComponentToWorld.InverseTransformPosition(Center + HalfAxis));
	}
	return Shape.Radius != Before.Radius || !Shape.StartOffset.Equals(Before.StartOffset) || !Shape.EndOffset.Equals(Before.EndOffset);
}

void Draw(FPrimitiveDrawInterface* PDI, const FGeometry& Geometry, const FLinearColor& Color, float Thickness) {
	if (!Geometry.BoxExtent.IsZero()) {
		DrawWireBox(PDI, FTransform(Geometry.BoxRotation, Geometry.Start).ToMatrixNoScale(),
			FBox(-Geometry.BoxExtent, Geometry.BoxExtent), Color, SDPG_Foreground, Thickness);
		return;
	}
	const FVector Axis = Geometry.End - Geometry.Start;
	if (Axis.IsNearlyZero()) {
		DrawWireSphere(PDI, Geometry.Start, Color, Geometry.Radius, 24, SDPG_Foreground, Thickness, 0.f, true);
	} else {
		const FVector Z = Axis.GetSafeNormal();
		FVector X, Y;
		Z.FindBestAxisVectors(X, Y);
		DrawWireCapsule(PDI, (Geometry.Start + Geometry.End) * 0.5, X, Y, Z, Color,
			Geometry.Radius, Axis.Size() * 0.5 + Geometry.Radius, 24, SDPG_Foreground, Thickness, 0.f, true);
	}
}

}
