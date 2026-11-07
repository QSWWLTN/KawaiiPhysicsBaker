#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "KawaiiPhysicsBakeSession.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiClothingProxySnapshot.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "KawaiiPhysicsIKMeshEnvelope.h"
#include "KawaiiPhysicsIKCorrectionMath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace {
// Diagnostic only: intersect the two LOD 0 skinned surfaces, rather than treating the empty
// space inside a fitted capsule as garment. Material opacity and morph deltas are not sampled.
struct FContactSurfaceTriangle {
	FVector P[3];
	FBox Bounds{ForceInit};
	FName Region;
	uint32 Index = 0;
};

bool BuildContactSurface(USkeletalMeshComponent& Component, const FTransform& ToTarget,
	const TArray<FName>* BoneRegions, TArray<FContactSurfaceTriangle>& Triangles, FString& Error) {
	const auto* Mesh = Component.GetSkeletalMeshAsset();
	const auto* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.IsEmpty() || !Component.GetSkinWeightBuffer(0)) {
		Error = TEXT("Contact surface audit requires CPU-readable LOD 0 skinning data"); return false;
	}
	const auto& LOD = RenderData->LODRenderData[0];
	auto& Weights = *Component.GetSkinWeightBuffer(0);
	TArray<FMatrix44f> Matrices; Component.GetCurrentRefToLocalMatrices(Matrices, 0);
	TArray<FVector> Positions; Positions.SetNum(LOD.GetNumVertices());
	TArray<FName> Regions; Regions.SetNum(Positions.Num());
	for (const auto& Section : LOD.RenderSections) {
		if (Section.bDisabled) { continue; }
		for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices; ++V) {
			uint16 DominantWeight = 0; int32 DominantBone = INDEX_NONE;
			for (uint32 I = 0; I < Weights.GetMaxBoneInfluences(); ++I) {
				const uint16 W = Weights.GetBoneWeight(V, I);
				const int32 B = Weights.GetBoneIndex(V, I);
				if (W > DominantWeight && Section.BoneMap.IsValidIndex(B)) { DominantWeight = W; DominantBone = Section.BoneMap[B]; }
			}
			if (DominantBone != INDEX_NONE) {
				Regions[V] = BoneRegions ? (*BoneRegions)[DominantBone] : Mesh->GetRefSkeleton().GetBoneName(DominantBone);
			}
			Positions[V] = ToTarget.TransformPosition(FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(&Component, V, LOD, Weights, Matrices)));
		}
	}
	TArray<uint32> Indices; LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	if (Indices.IsEmpty()) { Error = TEXT("Contact surface audit requires CPU-readable LOD 0 indices"); return false; }
	for (const auto& Section : LOD.RenderSections) {
		if (Section.bDisabled) { continue; }
		for (uint32 I = Section.BaseIndex; I < Section.BaseIndex + Section.NumTriangles * 3; I += 3) {
			const FName A = Regions[Indices[I]], B = Regions[Indices[I + 1]], C = Regions[Indices[I + 2]];
			// Hair boundary triangles are included whenever at least one vertex belongs to a hair chain.
			const FName Region = !A.IsNone() && (A == B || A == C) ? A : !B.IsNone() && B == C ? B : !A.IsNone() ? A : !B.IsNone() ? B : C;
			if (BoneRegions && Region.IsNone()) { continue; }
			auto& T = Triangles.AddDefaulted_GetRef(); T.Region = Region; T.Index = I / 3;
			for (int32 K = 0; K < 3; ++K) { T.P[K] = Positions[Indices[I + K]]; T.Bounds += T.P[K]; }
		}
	}
	return true;
}

bool AuditSurfaceIntersections(USkeletalMeshComponent& Main, USkeletalMeshComponent& Garment,
	const TArray<FName>& HairRegions, int32 Frame, FString& Rows, FString& Error) {
	TArray<FContactSurfaceTriangle> Hair, Clothing;
	if (!BuildContactSurface(Main, FTransform::Identity, &HairRegions, Hair, Error)
		|| !BuildContactSurface(Garment, Garment.GetComponentTransform().GetRelativeTransform(Main.GetComponentTransform()), nullptr, Clothing, Error)) { return false; }
	const auto Cell = [](const FVector& P) { return FIntVector(FMath::FloorToInt(P.X / 8.), FMath::FloorToInt(P.Y / 8.), FMath::FloorToInt(P.Z / 8.)); };
	TMap<FIntVector, TArray<int32>> Grid;
	for (int32 I = 0; I < Clothing.Num(); ++I) {
		const FIntVector Low = Cell(Clothing[I].Bounds.Min), High = Cell(Clothing[I].Bounds.Max);
		for (int32 X = Low.X; X <= High.X; ++X) for (int32 Y = Low.Y; Y <= High.Y; ++Y) for (int32 Z = Low.Z; Z <= High.Z; ++Z) {
			Grid.FindOrAdd(FIntVector(X, Y, Z)).Add(I);
		}
	}
	for (const auto& A : Hair) {
		const FIntVector Low = Cell(A.Bounds.Min), High = Cell(A.Bounds.Max);
		TSet<int32> Candidates;
		for (int32 X = Low.X; X <= High.X; ++X) for (int32 Y = Low.Y; Y <= High.Y; ++Y) for (int32 Z = Low.Z; Z <= High.Z; ++Z) {
			if (const auto* Found = Grid.Find(FIntVector(X, Y, Z))) { Candidates.Append(*Found); }
		}
		for (const int32 I : Candidates) {
			const auto& B = Clothing[I]; if (!A.Bounds.Intersect(B.Bounds)) { continue; }
			FVector Hit = FVector::ZeroVector, Normal = FVector::ZeroVector; bool bCrossing = false;
			for (int32 K = 0; K < 3 && !bCrossing; ++K) {
				bCrossing = FMath::SegmentTriangleIntersection(A.P[K], A.P[(K + 1) % 3], B.P[0], B.P[1], B.P[2], Hit, Normal)
					|| FMath::SegmentTriangleIntersection(B.P[K], B.P[(K + 1) % 3], A.P[0], A.P[1], A.P[2], Hit, Normal);
			}
			if (bCrossing) { Rows += FString::Printf(TEXT("%d,%s,%u,%s,%u,%.6f,%.6f,%.6f\n"), Frame,
				*A.Region.ToString(), A.Index, *B.Region.ToString(), B.Index, Hit.X, Hit.Y, Hit.Z); }
		}
	}
	return true;
}

class FContactAuditCommand : public IAutomationLatentCommand {
public:
	FContactAuditCommand(FAutomationTestBase* T, UAnimBlueprint* BP, const FString& InLabel) : Test(T), Blueprint(BP), Session(MakeShared<FKawaiiPhysicsBakeSession>()), Label(InLabel) {}
	bool Update() override {
		FString Error;
		if (!Started) {
			Started = true;
			if (!Session->Initialize(Blueprint.Get(), Error) || !Session->StartPreview(Error)) { Test->AddError(Error); return true; }
			SavedSettleTime = KawaiiPhysicsBake::GetSettings(Blueprint.Get())->PreviewCorrectionProfile->SettleTime;
		}
		// Only the transient diagnostic profile is changed. The native simulation, source pose and
		// garment sampling continue uninterrupted, isolating correction history from input motion.
		if (Label.StartsWith(TEXT("history-reset-"))) {
			const int32 ResetFrame = FCString::Atoi(*Label.RightChop(14));
			KawaiiPhysicsBake::GetSettings(Blueprint.Get())->PreviewCorrectionProfile->SettleTime =
				Session->GetCachedFrameCount() == ResetFrame ? 0.f : SavedSettleTime;
		}
		Session->Tick(1.f / 60.f);
		if (!Session->IsRunning()) { Test->AddError(Session->GetStatus().ToString()); return true; }
		if (Session->GetCachedFrameCount() > Frames) {
			if (!Audit(Session->GetCachedFrameCount() - 1, Error)) { Test->AddError(Error); Session->Cancel(); return true; }
			Frames = Session->GetCachedFrameCount();
		}
		if (Frames <= Session->GetLastFrame() && !(Label.StartsWith(TEXT("idle-")) && Frames >= 1)) { return false; }
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("KawaiiContactAudit");
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveStringToFile(Rows, *(Directory / (TEXT("landing-") + Label + TEXT(".csv"))));
		FFileHelper::SaveStringToFile(PoseRows, *(Directory / (TEXT("pose-") + Label + TEXT(".csv"))));
		FFileHelper::SaveStringToFile(SurfaceRows, *(Directory / (TEXT("surface-") + Label + TEXT(".csv"))));
		const double Width = HairMaxX - HairMinX;
		FFileHelper::SaveStringToFile(FString::SanitizeFloat(Width), *(Directory / (TEXT("width-") + Label + TEXT(".txt"))));
		Test->AddInfo(FString::Printf(TEXT("Skinned hair width %.6f cm (%s)"), Width, *Label));
		if (Label == TEXT("idle-default")) {
			FString OriginalWidth;
			if (FFileHelper::LoadFileToString(OriginalWidth, *(Directory / TEXT("width-idle-legacy.txt")))) {
				Test->TestTrue(TEXT("The default configuration preserves the original idle hairstyle"), FMath::IsNearlyEqual(Width, FCString::Atod(*OriginalWidth), .001));
			}
		}
		Test->AddInfo(FString::Printf(TEXT("%d landing frames: maximum proxy alignment shift %.6f cm, movable bone overlap %.6f cm, skinned hair vertex overlap %.6f cm"), Frames, MaxShift, MaxPointDepth, MaxVertexDepth));
		Test->AddInfo(FString::Printf(TEXT("Frames 5-48 backpack/skin overlap %.6f cm"), BackpackSkinDepth));
		if (!Label.StartsWith(TEXT("idle-"))) { Test->TestEqual(TEXT("Every source frame is audited"), Frames, Session->GetLastFrame() + 1); }
		if (Label == TEXT("mesh")) { Test->TestTrue(TEXT("The measured skin envelope clears the reported sustained backpack overlap"), BackpackSkinDepth < .1); }
		Session->Cancel();
		return true;
	}
private:
	bool Audit(int32 Frame, FString& Error) {
		auto* Settings = KawaiiPhysicsBake::GetSettings(Blueprint.Get());
		auto* Main = Session->GetMainComponent();
		USkeletalMeshComponent* Garment = nullptr;
		TInlineComponentArray<USkeletalMeshComponent*> Components(Main->GetOwner());
		for (auto* Component : Components) { if (Component->GetFName() == Settings->CollisionSourcePart) { Garment = Component; } }
		if (!Garment) { Error = TEXT("Missing garment component"); return false; }
		const auto& Definition = *Settings->Parts[0].ProxyDefinition;
		const auto* Profile = Settings->PreviewCorrectionProfile.Get();
		if (Snapshot.BoneNames.IsEmpty()) {
			Snapshot.DefinitionIdentity = FSoftObjectPath(&Definition);
			Snapshot.DefinitionSignature = Definition.ComputeSignature();
			Snapshot.SourceMesh = FSoftObjectPath(Garment->GetSkeletalMeshAsset());
			if (!KawaiiClothingProxy::BuildMinimalHierarchy(Garment->GetSkeletalMeshAsset()->GetRefSkeleton(), Definition.Shapes, Snapshot.BoneNames, Snapshot.ParentIndices, SourceBones, Error)) { return false; }
			Snapshot.Shapes = Definition.Shapes; Snapshot.IgnoredTargetRootBones = Definition.IgnoredTargetRootBones;
			Snapshot.SampleRate = 60; Snapshot.SourceDuration = 1.f / 60.f; Snapshot.NumFrames = 2;
			Snapshot.LocalKeys.SetNum(SourceBones.Num() * 2);
		}
		const auto& SourcePose = Garment->GetComponentSpaceTransforms();
		for (int32 Bone = 0; Bone < SourceBones.Num(); ++Bone) {
			const int32 Parent = Snapshot.ParentIndices[Bone];
			FTransform Local = Parent == INDEX_NONE ? SourcePose[SourceBones[Bone]] : SourcePose[SourceBones[Bone]].GetRelativeTransform(SourcePose[SourceBones[Parent]]);
			Local.NormalizeRotation(); Snapshot.LocalKeys[Bone] = Snapshot.LocalKeys[Bone + SourceBones.Num()] = Local;
		}
		const auto Space = Garment->GetComponentTransform().GetRelativeTransform(Main->GetComponentTransform());
		FMemMark Mark(FMemStack::Get());
		FAnimInstanceProxy Proxy(Main->GetAnimInstance()); Proxy.GetRequiredBones() = Main->GetAnimInstance()->GetRequiredBones();
		FPoseContext Native(&Proxy);
		const auto& Local = Main->GetBoneSpaceTransforms();
		const auto& Skeleton = Main->GetSkeletalMeshAsset()->GetRefSkeleton();
		for (int32 Bone = 0; Bone < Native.Pose.GetNumBones(); ++Bone) {
			const FCompactPoseBoneIndex Index(Bone); Native.Pose[Index] = Local[Native.Pose.GetBoneContainer().MakeMeshPoseIndex(Index).GetInt()];
		}
		TArray<FName> Excluded;
		for (const auto& Chain : Profile->Chains) { Excluded.Add(Chain.RootBone.BoneName); }
		KawaiiClothingProxy::FLiveBodyPose Live;
		TArray<FKawaiiClothingProxyCapsule> RawCapsules, Capsules;
		if (!KawaiiClothingProxy::BuildLiveBodyPose(Native.Pose, Excluded, Live, Error)
			|| !KawaiiClothingProxy::SampleSnapshots({{&Snapshot, 0.f, 1.f}}, Space, RawCapsules, Error)
			|| !KawaiiClothingProxy::SampleSnapshots({{&Snapshot, 0.f, 1.f}}, Space, Capsules, Error, nullptr, &Live)) { return false; }
		for (int32 C = 0; C < Capsules.Num(); ++C) {
			const double Shift = FMath::Max(FVector::Distance(Capsules[C].Start, RawCapsules[C].Start), FVector::Distance(Capsules[C].End, RawCapsules[C].End));
			MaxShift = FMath::Max(MaxShift, Shift);
			Rows += FString::Printf(TEXT("%d,proxy_shift,,,%s,%.6f\n"), Frame, *Capsules[C].Id.ToString(), Shift);
			PoseRows += FString::Printf(TEXT("%d,proxy,%s,start,%.9f,%.9f,%.9f\n%d,proxy,%s,end,%.9f,%.9f,%.9f\n"),
				Frame, *Capsules[C].Id.ToString(), Capsules[C].Start.X, Capsules[C].Start.Y, Capsules[C].Start.Z,
				Frame, *Capsules[C].Id.ToString(), Capsules[C].End.X, Capsules[C].End.Y, Capsules[C].End.Z);
		}
		const auto& Pose = Main->GetComponentSpaceTransforms();
		TArray<int32> BoneToChain; BoneToChain.Init(INDEX_NONE, Skeleton.GetNum());
		for (int32 C = 0; C < Profile->Chains.Num(); ++C) {
			const auto& Chain = Profile->Chains[C]; const int32 Root = Skeleton.FindBoneIndex(Chain.RootBone.BoneName), Tip = Skeleton.FindBoneIndex(Chain.TipBone.BoneName);
			for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone) {
				for (int32 Ancestor = Bone; Ancestor != INDEX_NONE; Ancestor = Skeleton.GetParentIndex(Ancestor)) {
					if (Ancestor == Root) { BoneToChain[Bone] = C; break; }
				}
			}
			for (int32 Bone = Tip; Bone != INDEX_NONE && Bone != Root; Bone = Skeleton.GetParentIndex(Bone)) {
				const FVector P = Pose[Bone].GetTranslation();
				PoseRows += FString::Printf(TEXT("%d,bone,%s,%s,%.9f,%.9f,%.9f\n"), Frame,
					*Chain.RootBone.BoneName.ToString(), *Skeleton.GetBoneName(Bone).ToString(), P.X, P.Y, P.Z);
				const FVector Parent = Pose[Skeleton.GetParentIndex(Bone)].GetTranslation();
				for (const auto& Capsule : Capsules) {
					if (Capsule.IgnoredTargetRootBones.Contains(Chain.RootBone.BoneName)) { continue; }
					const KawaiiPhysicsIKCorrection::FCapsule Primitive{Capsule.Start, Capsule.End, Capsule.Radius, Capsule.BoxExtent, Capsule.BoxRotation};
					const double Depth = Chain.CollisionRadius - KawaiiPhysicsIKCorrection::SurfaceDistance(P, Primitive);
					MaxPointDepth = FMath::Max(MaxPointDepth, Depth);
					if (Depth > .01) { Rows += FString::Printf(TEXT("%d,bone,%s,%s,%s,%.6f\n"), Frame, *Chain.RootBone.BoneName.ToString(), *Skeleton.GetBoneName(Bone).ToString(), *Capsule.Id.ToString(), Depth); }
					FVector OnChain, OnCapsule;
					FMath::SegmentDistToSegmentSafe(Parent, P, Capsule.Start, Capsule.End, OnChain, OnCapsule);
					double SegmentDepth = Capsule.Radius + Chain.CollisionRadius - FVector::Distance(OnChain, OnCapsule);
					if (Primitive.IsBox()) {
						SegmentDepth = 0.;
						for (int32 S = 0; S <= 64; ++S) { SegmentDepth = FMath::Max(SegmentDepth, Chain.CollisionRadius - KawaiiPhysicsIKCorrection::SurfaceDistance(FMath::Lerp(Parent, P, double(S) / 64.), Primitive)); }
					}
					const bool bPinnedInside = Skeleton.GetParentIndex(Bone) == Root
						&& KawaiiPhysicsIKCorrection::SurfaceDistance(Parent, Primitive) < Chain.CollisionRadius;
					if (SegmentDepth > .01) { Rows += FString::Printf(TEXT("%d,%s,%s,%s,%s,%.6f\n"), Frame,
						bPinnedInside ? TEXT("pinned_segment") : TEXT("segment"), *Chain.RootBone.BoneName.ToString(), *Skeleton.GetBoneName(Bone).ToString(), *Capsule.Id.ToString(), SegmentDepth); }
				}
			}
		}
		const auto* RenderData = Main->GetSkeletalMeshAsset()->GetResourceForRendering();
		if (RenderData && RenderData->LODRenderData.Num() > 0) {
			const auto& LOD = RenderData->LODRenderData[0]; auto* Weights = Main->GetSkinWeightBuffer(0);
			if (Weights) {
				TArray<FMatrix44f> Matrices; Main->GetCurrentRefToLocalMatrices(Matrices, 0);
				for (const auto& Section : LOD.RenderSections) {
					for (uint32 Vertex = Section.BaseVertexIndex; Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex) {
						int32 ChainIndex = INDEX_NONE; uint16 Dominant = 0;
						for (uint32 I = 0; I < Weights->GetMaxBoneInfluences(); ++I) {
							const auto Weight = Weights->GetBoneWeight(Vertex, I); if (Weight <= Dominant) { continue; }
							const auto LocalBone = Weights->GetBoneIndex(Vertex, I);
							if (Section.BoneMap.IsValidIndex(LocalBone)) { Dominant = Weight; ChainIndex = BoneToChain[Section.BoneMap[LocalBone]]; }
						}
						if (ChainIndex == INDEX_NONE) { continue; }
						const FVector P(USkinnedMeshComponent::GetSkinnedVertexPosition(Main, Vertex, LOD, *Weights, Matrices));
						const auto Root = Profile->Chains[ChainIndex].RootBone.BoneName;
						if (Root.ToString().StartsWith(TEXT("Hair"))) { HairMinX = FMath::Min(HairMinX, P.X); HairMaxX = FMath::Max(HairMaxX, P.X); }
						for (const auto& Capsule : Capsules) {
							if (Capsule.IgnoredTargetRootBones.Contains(Root)) { continue; }
							const double Depth = -KawaiiPhysicsIKCorrection::SurfaceDistance(P, {Capsule.Start, Capsule.End, Capsule.Radius, Capsule.BoxExtent, Capsule.BoxRotation});
							if (Frame >= 5 && Frame <= 48 && Capsule.Id == TEXT("Belt_root_1")) { BackpackSkinDepth = FMath::Max(BackpackSkinDepth, Depth); }
							MaxVertexDepth = FMath::Max(MaxVertexDepth, Depth);
							if (Depth > .1) { Rows += FString::Printf(TEXT("%d,vertex,%s,%u,%s,%.6f\n"), Frame, *Root.ToString(), Vertex, *Capsule.Id.ToString(), Depth); }
						}
					}
				}
			}
		}
		if (Label.StartsWith(TEXT("history-")) || (Label == TEXT("idle-geometry") || Label == TEXT("idle-box-trial") || Label == TEXT("idle-bridge-trial"))) {
			TArray<FName> HairRegions; HairRegions.SetNum(BoneToChain.Num());
			for (int32 Bone = 0; Bone < BoneToChain.Num(); ++Bone) {
				if (BoneToChain[Bone] == INDEX_NONE) { continue; }
				const FName Root = Profile->Chains[BoneToChain[Bone]].RootBone.BoneName;
				if (Root.ToString().StartsWith(TEXT("Hair"))) { HairRegions[Bone] = Root; }
			}
			if (!AuditSurfaceIntersections(*Main, *Garment, HairRegions, Frame, SurfaceRows, Error)) { return false; }
		}
		return true;
	}
	FAutomationTestBase* Test;
	TStrongObjectPtr<UAnimBlueprint> Blueprint;
	TSharedRef<FKawaiiPhysicsBakeSession> Session;
	KawaiiClothingProxy::FSnapshot Snapshot;
	TArray<int32> SourceBones;
	FString Rows = TEXT("frame,kind,root,item,collider,value\n");
	FString PoseRows = TEXT("frame,kind,root,item,x,y,z\n");
	FString SurfaceRows = TEXT("frame,hair_root,hair_triangle,garment_bone,garment_triangle,x,y,z\n");
	FString Label;
	float SavedSettleTime = 0.f;
	int32 Frames = 0;
	bool Started = false;
	double MaxShift = 0., MaxPointDepth = 0., MaxVertexDepth = 0.;
	double BackpackSkinDepth = 0.;
	double HairMinX = TNumericLimits<double>::Max(), HairMaxX = -TNumericLimits<double>::Max();
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FKawaiiLandingContactAuditTest, "KawaiiPhysicsBaker.ContactAudit.ProjectLanding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FKawaiiLandingContactAuditTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const {
	Names = {TEXT("Current"), TEXT("MeshEnvelope"), TEXT("IdleLegacy"), TEXT("IdleInflated"), TEXT("IdleDefault"),
		TEXT("History.Continuous"), TEXT("History.Disabled"), TEXT("History.Reset6"), TEXT("History.Reset24"), TEXT("History.Reset48"), TEXT("History.MoreIterations"), TEXT("History.Source"), TEXT("IdleGeometry"), TEXT("OfficialPrimitives.LandingBoxes"), TEXT("OfficialPrimitives.IdleBoxes"), TEXT("OfficialPrimitives.LandingBridges"), TEXT("OfficialPrimitives.IdleBridges")};
	Commands = {TEXT("current"), TEXT("mesh"), TEXT("idle-legacy"), TEXT("idle-inflated"), TEXT("idle-default"),
		TEXT("history-continuous"), TEXT("history-off"), TEXT("history-reset-6"), TEXT("history-reset-24"), TEXT("history-reset-48"), TEXT("history-iterations"), TEXT("history-source"), TEXT("idle-geometry"), TEXT("history-box-trial"), TEXT("idle-box-trial"), TEXT("history-bridge-trial"), TEXT("idle-bridge-trial")};
}
bool FKawaiiLandingContactAuditTest::RunTest(const FString& Parameters) {
	// Generate a fresh control in the same test, rather than relying on another test's output/order.
	if (Parameters == TEXT("idle-default")) { if (!RunTest(TEXT("idle-legacy"))) { return false; } }
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/KawaiiBakes/ABP_KawaiiBake"));
	if (!TestNotNull(TEXT("Bake fixture"), Source)) { return false; }
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/KawaiiContactAudit_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	auto* Copy = DuplicateObject<UAnimBlueprint>(Source, Package, TEXT("ABP_ContactAudit"));
	auto* Settings = KawaiiPhysicsBake::GetSettings(Copy);
	Settings->SourceAnimation = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/Jump/Land_3h"));
	if (Parameters.StartsWith(TEXT("idle-"))) { Settings->SourceAnimation = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/Idle")); }
	Settings->bPreviewClothingProxyCollision = true; Settings->bEnableClothingCollision = false;
	Settings->PreviewCorrectionProfile = DuplicateObject<UKawaiiPhysicsIKCorrectionProfile>(Settings->PreviewCorrectionProfile, Package, TEXT("AuditProfile"));
	Settings->PreviewCorrectionProfile->SmoothingTime = 0.f;
	if (Parameters == TEXT("history-source")) { Settings->PreviewCorrectionAlpha = 0.f; }
	if (Parameters == TEXT("history-off")) { Settings->PreviewCorrectionProfile->SettleTime = 0.f; }
	if (Parameters == TEXT("history-iterations")) { Settings->PreviewCorrectionProfile->Iterations = 64; }
	Settings->PreviewCorrectionProfile->bUseMeshEnvelope = Parameters == TEXT("mesh");
	Settings->WarmUpFrames = 60; Settings->SampleRate = 60; Settings->CollisionSourcePart = Settings->Parts[0].Name;
	Settings->Parts[0].ProxyMotionSet = nullptr;
	if (Parameters.EndsWith(TEXT("bridge-trial"))) {
		auto* Profile = Settings->PreviewCorrectionProfile.Get(); Profile->ContactBridges.Reset(); Profile->BridgeIterations = 8;
		const TArray<FName> Order{TEXT("Hair_back_1_L"), TEXT("Hair_back_2_L"), TEXT("Hair_back_3_L"), TEXT("Hair_back_4"), TEXT("Hair_back_3_R"), TEXT("Hair_back_2_R"), TEXT("Hair_back_1_R")};
		const auto& Ref = Settings->MainMesh->GetRefSkeleton();
		TArray<TArray<int32>> Paths;
		for (FName Root : Order) {
			const auto* Chain = Profile->Chains.FindByPredicate([&](const auto& C) { return C.RootBone.BoneName == Root; });
			if (!Chain) { AddError(TEXT("Bridge fixture missing hair chain")); return false; }
			auto& Path = Paths.AddDefaulted_GetRef();
			for (int32 B = Ref.FindBoneIndex(Chain->TipBone.BoneName); B != INDEX_NONE; B = Ref.GetParentIndex(B)) {
				Path.Insert(B, 0); if (Ref.GetBoneName(B) == Root) { break; }
			}
		}
		for (int32 C = 1; C < Paths.Num(); ++C) {
			for (int32 I = 1; I < Paths[C - 1].Num(); ++I) {
				const int32 J = FMath::RoundToInt(double(I) * (Paths[C].Num() - 1) / (Paths[C - 1].Num() - 1));
				auto& Bridge = Profile->ContactBridges.AddDefaulted_GetRef();
				Bridge.BoneA.BoneName = Ref.GetBoneName(Paths[C - 1][I]); Bridge.BoneB.BoneName = Ref.GetBoneName(Paths[C][J]);
				Bridge.Samples = 3; Bridge.Feedback = 1.f;
			}
		}
		AddInfo(FString::Printf(TEXT("Trial has %d explicitly selected hair-curtain bridges; no saved asset changes"), Profile->ContactBridges.Num()));
	}
	if (Parameters.EndsWith(TEXT("box-trial"))) {
		Settings->Parts[0].ProxyDefinition = DuplicateObject<UKawaiiClothingProxyAsset>(Settings->Parts[0].ProxyDefinition, Package, TEXT("BoxTrialDefinition"));
		for (auto& Shape : Settings->Parts[0].ProxyDefinition->Shapes) {
			if (Shape.Id != TEXT("Belt_root_1") && Shape.Id != TEXT("Chest_1")) { continue; }
			const FVector Axis = Shape.EndOffset - Shape.StartOffset;
			Shape.bBox = true;
			Shape.BoxRotation = FRotationMatrix::MakeFromZ(Axis).Rotator();
			Shape.BoxExtent = FVector(Shape.Radius, Shape.Radius, Axis.Size() * .5 + Shape.Radius);
			Shape.StartOffset = Shape.EndOffset = (Shape.StartOffset + Shape.EndOffset) * .5;
		}
	}
	if (Parameters == TEXT("mesh") || Parameters == TEXT("idle-default") || Parameters == TEXT("idle-inflated")) {
		TArray<float> Radii; FString Error;
		if (!KawaiiPhysicsIKMeshEnvelope::Measure(*Settings->MainMesh, Settings->PreviewCorrectionProfile->Chains, Radii, Error)) { AddError(Error); return false; }
		for (int32 C = 0; C < Radii.Num(); ++C) {
			auto& Chain = Settings->PreviewCorrectionProfile->Chains[C];
			AddInfo(FString::Printf(TEXT("%s: configured %.3f, mesh %.3f cm"), *Chain.RootBone.BoneName.ToString(), Chain.CollisionRadius, Radii[C]));
			Chain.MeshEnvelopeRadius = Radii[C];
			if (Parameters == TEXT("idle-inflated")) { Chain.CollisionRadius = FMath::Max(Chain.CollisionRadius, Radii[C]); }
		}
	}
	ADD_LATENT_AUTOMATION_COMMAND(FContactAuditCommand(this, Copy, Parameters));
	return true;
}
#endif
