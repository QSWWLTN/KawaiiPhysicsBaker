#include "KawaiiPhysicsIKMeshEnvelope.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

bool KawaiiPhysicsIKMeshEnvelope::Measure(const USkeletalMesh& Mesh, const TArray<FKawaiiPhysicsIKCorrectionChain>& Chains,
	TArray<float>& OutRadii, FString& Error) {
	const auto* Render = Mesh.GetResourceForRendering();
	if (!Render || Render->LODRenderData.IsEmpty()) { Error = TEXT("Mesh envelope requires LOD 0 render data"); return false; }
	const auto& LOD = Render->LODRenderData[0];
	const auto& Weights = LOD.SkinWeightVertexBuffer;
	if (Weights.GetNumVertices() != LOD.GetNumVertices()) { Error = TEXT("Mesh envelope skin weights are unavailable"); return false; }
	const auto& Skeleton = Mesh.GetRefSkeleton();
	TArray<FTransform> Reference;
	for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone) {
		const int32 Parent = Skeleton.GetParentIndex(Bone);
		Reference.Add(Parent == INDEX_NONE ? Skeleton.GetRefBonePose()[Bone] : Skeleton.GetRefBonePose()[Bone] * Reference[Parent]);
	}
	TArray<int32> BoneToChain; BoneToChain.Init(INDEX_NONE, Skeleton.GetNum());
	TArray<TArray<int32>> Paths;
	OutRadii.Init(0.f, Chains.Num());
	for (int32 C = 0; C < Chains.Num(); ++C) {
		const int32 Root = Skeleton.FindBoneIndex(Chains[C].RootBone.BoneName), Tip = Skeleton.FindBoneIndex(Chains[C].TipBone.BoneName);
		if (Root == INDEX_NONE || Tip == INDEX_NONE) { Error = TEXT("Mesh envelope chain bone is missing"); return false; }
		auto& Path = Paths.AddDefaulted_GetRef();
		for (int32 Bone = Tip; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone)) { Path.Insert(Bone, 0); if (Bone == Root) { break; } }
		if (Path.Num() < 2 || Path[0] != Root) { Error = TEXT("Mesh envelope requires a root-to-descendant chain"); return false; }
		for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone) {
			for (int32 Ancestor = Bone; Ancestor != INDEX_NONE; Ancestor = Skeleton.GetParentIndex(Ancestor)) {
				if (Ancestor == Root) { BoneToChain[Bone] = C; break; }
			}
		}
	}
	for (const auto& Section : LOD.RenderSections) {
		for (uint32 Vertex = Section.BaseVertexIndex; Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex) {
			uint16 Dominant = 0; int32 Chain = INDEX_NONE;
			for (uint32 Influence = 0; Influence < Weights.GetMaxBoneInfluences(); ++Influence) {
				const uint16 Weight = Weights.GetBoneWeight(Vertex, Influence);
				const uint32 LocalBone = Weights.GetBoneIndex(Vertex, Influence);
				if (Weight > Dominant && Section.BoneMap.IsValidIndex(LocalBone)) { Dominant = Weight; Chain = BoneToChain[Section.BoneMap[LocalBone]]; }
			}
			if (Chain == INDEX_NONE) { continue; }
			const FVector P(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
			double Distance = TNumericLimits<double>::Max();
			const auto& Path = Paths[Chain];
			for (int32 I = 1; I < Path.Num(); ++I) {
				const FVector Q = FMath::ClosestPointOnSegment(P, Reference[Path[I-1]].GetTranslation(), Reference[Path[I]].GetTranslation());
				const double D = FVector::Distance(P, Q);
				if (D < Distance) { Distance = D; }
			}
			OutRadii[Chain] = FMath::Max(OutRadii[Chain], float(Distance));
		}
	}
	return true;
}
