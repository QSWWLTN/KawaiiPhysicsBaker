#include "KawaiiPhysicsBakePreviewCollision.h"

#include "KawaiiPhysicsClothingCollisionProfile.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiClothingProxyAsset.h"

namespace KawaiiPhysicsBakePreviewCollision {
	bool BuildProfile(const UKawaiiClothingProxyAsset& Definition, const TArray<FName>& AvailableRoots, const TArray<FName>& RequestedRoots, UKawaiiPhysicsClothingCollisionProfile& OutProfile, FString& OutError) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_PreviewProxy_BuildProfile);
		OutError.Reset();
		OutProfile.StaticCapsules.Reset();
		OutProfile.DynamicCapsules.Reset();
		OutProfile.ComponentCapsules.Reset();
		OutProfile.TargetRootBones.Reset();
		if (!Definition.Validate(OutError)) {
			return false;
		}

		TArray<FName> TargetRoots;
		if (RequestedRoots.IsEmpty()) {
			for (const FName Root : AvailableRoots) {
				if (!Root.IsNone()) {
					TargetRoots.AddUnique(Root);
				}
			}
		} else {
			for (const FName Root : RequestedRoots) {
				if (Root.IsNone() || !AvailableRoots.Contains(Root)) {
					OutError = FString::Printf(TEXT("预览碰撞目标 %s 不是 Main 中已编译 KawaiiPhysics 节点的根骨骼。"), *Root.ToString());
					return false;
				}
				TargetRoots.AddUnique(Root);
			}
		}
		if (TargetRoots.IsEmpty()) {
			OutError = TEXT("Main 中没有可接收衣物代理碰撞的 KawaiiPhysics 根骨骼。");
			return false;
		}
		TSet<FName> IgnoredRootSet;
		for (const FName Root : Definition.IgnoredTargetRootBones) {
			IgnoredRootSet.Add(Root);
		}
		// A reusable garment may exclude roots absent on this character, or all current consumers.
		TargetRoots.RemoveAll([&IgnoredRootSet](FName Root) { return IgnoredRootSet.Contains(Root); });

		OutProfile.DynamicCapsules.Reserve(Definition.Shapes.Num());
		for (const FKawaiiClothingProxyShape& Shape : Definition.Shapes) {
			FKawaiiClothingCapsuleDefinition& Capsule = OutProfile.DynamicCapsules.AddDefaulted_GetRef();
			Capsule.bUseSockets = false;
			Capsule.StartBone = Shape.StartBone;
			Capsule.EndBone = Shape.EndBone;
			Capsule.StartOffset = Shape.StartOffset;
			Capsule.EndOffset = Shape.EndOffset;
			Capsule.Radius = Shape.Radius;
		}
		OutProfile.TargetRootBones = MoveTemp(TargetRoots);
		// Offline preview advances fixed simulation steps independently of wall-clock time.
		OutProfile.MaxSnapshotAgeSeconds = MAX_flt;
		OutProfile.MaxLOD = -1;
		OutProfile.MaxDistance = 0.f;
		return true;
	}
}
