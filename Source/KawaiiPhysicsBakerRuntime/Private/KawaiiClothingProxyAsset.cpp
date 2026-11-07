#include "KawaiiClothingProxyAsset.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/SecureHash.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ReferenceSkeleton.h"

namespace KawaiiClothingProxy {
	bool ValidateIgnoredTargetRoots(const TArray<FName>& Roots, FString& Error) {
		Error.Reset();
		if (Roots.Num() > MaxIgnoredTargetRoots || Roots.Contains(NAME_None)) {
			Error = FString::Printf(TEXT("衣物代理的忽略目标骨链最多为 %d 项，名称不能为 None。"), MaxIgnoredTargetRoots);
			return false;
		}
		return true;
	}

	bool ValidateShapes(const TArray<FKawaiiClothingProxyShape>& Shapes, FString& Error) {
		Error.Reset();
		if (Shapes.IsEmpty() || Shapes.Num() > MaxShapes) {
			Error = FString::Printf(TEXT("衣物代理需要 1–%d 个球体、胶囊或盒体。"), MaxShapes);
			return false;
		}
		TSet<FName> Ids;
		for (const FKawaiiClothingProxyShape& Shape : Shapes) {
			if (Shape.bBox && (Shape.BoxRotation.ContainsNaN() || Shape.BoxExtent.ContainsNaN() || Shape.BoxExtent.GetMin() <= 0.)) {
				Error = TEXT("盒体旋转和半尺寸必须有效，半尺寸必须大于零。"); return false;
			}
			if (Shape.Id.IsNone() || Ids.Contains(Shape.Id) || Shape.StartBone.IsNone()) {
				Error = TEXT("代理名称必须非空且唯一，起点骨骼不能为空。");
				return false;
			}
			if (Shape.StartOffset.ContainsNaN() || Shape.EndOffset.ContainsNaN() || !FMath::IsFinite(Shape.Radius) || Shape.Radius <= 0.f) {
				Error = FString::Printf(TEXT("代理 %s 的偏移或半径无效。"), *Shape.Id.ToString());
				return false;
			}
			Ids.Add(Shape.Id);
		}
		return true;
	}

	FString ComputeDefinitionSignature(const FSoftObjectPath& Mesh, const TArray<FKawaiiClothingProxyShape>& Shapes) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_DefinitionSignature);
		// Length-prefixed UTF-8 strings avoid ambiguous concatenation and preserve non-ASCII asset paths.
		FString Canonical(TEXT("KawaiiClothingProxyDefinition:1;"));
		const auto AppendString = [&Canonical](const FString& Value) {
			Canonical += FString::Printf(TEXT("%d:"), Value.Len());
			Canonical += Value;
			Canonical += TEXT(";");
		};
		AppendString(Mesh.ToString());
		Canonical += FString::Printf(TEXT("%d;"), Shapes.Num());
		for (const FKawaiiClothingProxyShape& Shape : Shapes) {
			AppendString(Shape.Id.ToString().ToLower());
			AppendString(Shape.StartBone.ToString().ToLower());
			AppendString(Shape.EndBone.ToString().ToLower());
			Canonical += FString::Printf(
				TEXT("%.17g,%.17g,%.17g;%.17g,%.17g,%.17g;%.17g;"),
				Shape.StartOffset.X,
				Shape.StartOffset.Y,
				Shape.StartOffset.Z,
				Shape.EndOffset.X,
				Shape.EndOffset.Y,
				Shape.EndOffset.Z,
				static_cast<double>(Shape.Radius)
			);
			// Keep existing capsule-only signatures stable when upgrading the plugin.
			if (Shape.bBox) {
				Canonical += FString::Printf(TEXT("Box;%.17g,%.17g,%.17g;%.17g,%.17g,%.17g;"),
					Shape.BoxExtent.X, Shape.BoxExtent.Y, Shape.BoxExtent.Z, Shape.BoxRotation.Pitch, Shape.BoxRotation.Yaw, Shape.BoxRotation.Roll);
			}
		}
		FTCHARToUTF8 UTF8(*Canonical);
		FMD5 Hash;
		Hash.Update(reinterpret_cast<const uint8*>(UTF8.Get()), UTF8.Length());
		uint8 Digest[16];
		Hash.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	FString ComputeSkeletonSignature(const FReferenceSkeleton& Skeleton) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_SkeletonSignature);
		FString Canonical(TEXT("KawaiiClothingProxySkeleton:1;"));
		Canonical += FString::Printf(TEXT("%d;"), Skeleton.GetRawBoneNum());
		for (int32 Bone = 0; Bone < Skeleton.GetRawBoneNum(); ++Bone) {
			const FString Name = Skeleton.GetBoneName(Bone).ToString().ToLower();
			const FTransform& Pose = Skeleton.GetRawRefBonePose()[Bone];
			const FVector P = Pose.GetTranslation(), S = Pose.GetScale3D();
			const FQuat Q = Pose.GetRotation();
			Canonical += FString::Printf(
				TEXT("%d:%s;%d;%.17g,%.17g,%.17g;%.17g,%.17g,%.17g,%.17g;%.17g,%.17g,%.17g;"),
				Name.Len(),
				*Name,
				Skeleton.GetParentIndex(Bone),
				P.X,
				P.Y,
				P.Z,
				Q.X,
				Q.Y,
				Q.Z,
				Q.W,
				S.X,
				S.Y,
				S.Z
			);
		}
		FTCHARToUTF8 UTF8(*Canonical);
		FMD5 Hash;
		Hash.Update(reinterpret_cast<const uint8*>(UTF8.Get()), UTF8.Length());
		uint8 Digest[16];
		Hash.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	bool BuildMinimalHierarchy(const FReferenceSkeleton& Skeleton, const TArray<FKawaiiClothingProxyShape>& Shapes, TArray<FName>& OutNames, TArray<int32>& OutParents, TArray<int32>& OutMeshIndices, FString& Error) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_BuildMinimalHierarchy);
		OutNames.Reset();
		OutParents.Reset();
		OutMeshIndices.Reset();
		if (!ValidateShapes(Shapes, Error)) {
			return false;
		}
		const int32 Count = Skeleton.GetRawBoneNum();
		if (Count <= 0 || Count > MaxBones) {
			Error = TEXT("衣物参考骨架为空或超出支持的骨骼数量。");
			return false;
		}
		TSet<int32> Required;
		for (const FKawaiiClothingProxyShape& Shape : Shapes) {
			for (const FName Endpoint : {Shape.StartBone, Shape.bBox || Shape.EndBone.IsNone() ? Shape.StartBone : Shape.EndBone}) {
				int32 Bone = Skeleton.FindBoneIndex(Endpoint);
				if (Bone < 0 || Bone >= Count) {
					Error = FString::Printf(TEXT("代理 %s 引用了不存在或非真实骨骼的 %s。"), *Shape.Id.ToString(), *Endpoint.ToString());
					return false;
				}
				while (Bone != INDEX_NONE) {
					Required.Add(Bone);
					const int32 Parent = Skeleton.GetParentIndex(Bone);
					if (Parent < INDEX_NONE || Parent >= Bone) {
						Error = TEXT("衣物骨架的父子层级无效。");
						return false;
					}
					Bone = Parent;
				}
			}
		}
		TMap<int32, int32> Indices;
		for (int32 Bone = 0; Bone < Count; ++Bone) {
			if (!Required.Contains(Bone)) {
				continue;
			}
			const int32 Parent = Skeleton.GetParentIndex(Bone);
			const int32* MinimalParent = Indices.Find(Parent);
			if (Parent != INDEX_NONE && !MinimalParent) {
				OutNames.Reset();
				OutParents.Reset();
				OutMeshIndices.Reset();
				Error = TEXT("代理层级缺少祖先骨骼。");
				return false;
			}
			const int32 ParentIndex = Parent == INDEX_NONE ? INDEX_NONE : *MinimalParent;
			Indices.Add(Bone, OutNames.Num());
			OutNames.Add(Skeleton.GetBoneName(Bone));
			OutParents.Add(ParentIndex);
			OutMeshIndices.Add(Bone);
		}
		return true;
	}
}

FString UKawaiiClothingProxyAsset::ComputeSignature() const {
	// Collision routing is read from the definition each sample and does not change baked geometry or tracks.
	return KawaiiClothingProxy::ComputeDefinitionSignature(PreviewMesh.ToSoftObjectPath(), Shapes);
}

bool UKawaiiClothingProxyAsset::Validate(FString& Error) const {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_ValidateDefinition);
	if (PreviewMesh.IsNull()) {
		Error = TEXT("请为衣物代理选择衣物骨骼网格体。");
		return false;
	}
	return KawaiiClothingProxy::ValidateShapes(Shapes, Error) && KawaiiClothingProxy::ValidateIgnoredTargetRoots(IgnoredTargetRootBones, Error);
}
