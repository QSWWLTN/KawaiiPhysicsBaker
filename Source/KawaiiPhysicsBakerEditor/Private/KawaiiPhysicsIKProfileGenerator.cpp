#include "KawaiiPhysicsIKProfileGenerator.h"

#include "AnimGraphNode_KawaiiAutoClothingProxy.h"
#include "AnimGraphNode_KawaiiPhysics.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "IContentBrowserSingleton.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KawaiiClothingProxyAsset.h"
#include "KawaiiClothingProxyMotion.h"
#include "KawaiiClothingProxyPlayback.h"
#include "KawaiiPhysicsLimitsDataAsset.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace KawaiiPhysicsIKProfileGenerator
{
namespace
{
	// Same limits as FAnimNode_KawaiiPhysicsIKCorrection::CorrectPose.
	constexpr int32 MaxChains = 64;
	constexpr int32 MaxCapsules = 64;

	bool IsInSubtree(int32 Bone, int32 Root, const TArray<int32>& Parents)
	{
		for (int32 Index = Bone; Index != INDEX_NONE; Index = Parents[Index]) {
			if (Index == Root) {
				return true;
			}
		}
		return false;
	}

	bool SameCapsule(const FKawaiiPhysicsIKCorrectionCapsule& A, const FKawaiiPhysicsIKCorrectionCapsule& B)
	{
		if (!FMath::IsNearlyEqual(A.Radius, B.Radius, 1.e-3f)) {
			return false;
		}
		const bool bForward = A.StartBone.BoneName == B.StartBone.BoneName && A.EndBone.BoneName == B.EndBone.BoneName
			&& A.StartOffset.Equals(B.StartOffset, 1.e-3) && A.EndOffset.Equals(B.EndOffset, 1.e-3);
		const bool bReversed = A.StartBone.BoneName == B.EndBone.BoneName && A.EndBone.BoneName == B.StartBone.BoneName
			&& A.StartOffset.Equals(B.EndOffset, 1.e-3) && A.EndOffset.Equals(B.StartOffset, 1.e-3);
		return bForward || bReversed;
	}

	/** KawaiiPhysics scales Radius along the chain by RadiusCurveData; keep the largest value so no segment is under-sized. */
	float RadiusScale(const FRuntimeFloatCurve& Curve)
	{
		const FRichCurve* Rich = Curve.GetRichCurveConst();
		if (!Rich || Rich->GetNumKeys() == 0) {
			return 1.f;
		}
		float Min = 0.f;
		float Max = 0.f;
		Rich->GetValueRange(Min, Max);
		return FMath::IsFinite(Max) && Max > 0.f ? Max : 1.f;
	}

	bool SaveAsset(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *Filename, Args);
	}

	void GetSelection(TArray<FAssetData>& Out)
	{
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Out);
	}

	/** The pin default is authoritative while a property pin is visible; hidden properties compile from the node struct. */
	UKawaiiClothingProxyMotionSet* GetMotionSet(const UAnimGraphNode_KawaiiAutoClothingProxy& Node, bool& bLinked)
	{
		bLinked = false;
		if (const UEdGraphPin* Pin = Node.FindPin(GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, ProxyMotionSet), EGPD_Input)) {
			bLinked = !Pin->LinkedTo.IsEmpty();
			return bLinked ? nullptr : Cast<UKawaiiClothingProxyMotionSet>(Pin->DefaultObject.Get());
		}
		return Node.Node.ProxyMotionSet;
	}
}

bool BuildChains(const TArray<FName>& BoneNames,
				 const TArray<int32>& Parents,
				 const TArray<FSimulatedRoot>& Roots,
				 const TArray<FName>& IgnoredTargetRoots,
				 TArray<FKawaiiPhysicsIKCorrectionChain>& OutChains,
				 TArray<FString>& OutNotes,
				 FString& Error)
{
	OutChains.Reset();
	OutNotes.Reset();
	Error.Reset();
	const int32 Num = BoneNames.Num();
	if (Num == 0 || Parents.Num() != Num) {
		Error = TEXT("骨骼层级为空，或父骨骼索引数量与骨骼数量不一致。");
		return false;
	}
	TMap<FName, int32> IndexOf;
	TArray<TArray<int32>> Children;
	Children.SetNum(Num);
	for (int32 Bone = 0; Bone < Num; ++Bone) {
		if (Parents[Bone] < INDEX_NONE || Parents[Bone] >= Bone) {
			Error = FString::Printf(TEXT("骨骼 %s 的父骨骼索引无效；层级必须按父骨骼在前排列。"), *BoneNames[Bone].ToString());
			return false;
		}
		if (BoneNames[Bone].IsNone() || IndexOf.Contains(BoneNames[Bone])) {
			Error = FString::Printf(TEXT("骨骼名 %s 为空或重复。"), *BoneNames[Bone].ToString());
			return false;
		}
		IndexOf.Add(BoneNames[Bone], Bone);
		if (Parents[Bone] != INDEX_NONE) {
			Children[Parents[Bone]].Add(Bone);
		}
	}

	// Reproduce KawaiiPhysics bone collection: the whole root subtree, stopping at excluded bones.
	TArray<uint8> Simulated;
	Simulated.Init(0, Num);
	TArray<float> Radius;
	Radius.Init(0.f, Num);
	for (const FSimulatedRoot& Root : Roots) {
		const int32* Start = IndexOf.Find(Root.RootBone);
		if (!Start) {
			OutNotes.Add(FString::Printf(TEXT("KawaiiPhysics 根骨骼 %s 不在骨架中，已跳过。"), *Root.RootBone.ToString()));
			continue;
		}
		TSet<int32> Excluded;
		for (const FName Name : Root.ExcludeBones) {
			if (const int32* Found = IndexOf.Find(Name)) {
				Excluded.Add(*Found);
			}
		}
		TArray<int32> Stack{*Start};
		while (!Stack.IsEmpty()) {
			const int32 Bone = Stack.Pop(EAllowShrinking::No);
			if (Excluded.Contains(Bone)) {
				continue;
			}
			Simulated[Bone] = 1;
			Radius[Bone] = FMath::Max(Radius[Bone], Root.CollisionRadius);
			Stack.Append(Children[Bone]);
		}
	}

	// A bone is relevant when it is simulated or leads to a simulated descendant.
	TArray<uint8> Relevant = Simulated;
	for (int32 Bone = Num - 1; Bone >= 0; --Bone) {
		if (Relevant[Bone] && Parents[Bone] != INDEX_NONE) {
			Relevant[Parents[Bone]] = 1;
		}
	}
	TArray<int32> RelevantChildCount;
	RelevantChildCount.Init(0, Num);
	TArray<int32> LastRelevantChild;
	LastRelevantChild.Init(INDEX_NONE, Num);
	for (int32 Bone = 0; Bone < Num; ++Bone) {
		if (Relevant[Bone] && Parents[Bone] != INDEX_NONE) {
			++RelevantChildCount[Parents[Bone]];
			LastRelevantChild[Parents[Bone]] = Bone;
		}
	}
	// PathTip is the deepest bone when the relevant part of the subtree is one path of simulated bones.
	TArray<int32> PathTip;
	PathTip.Init(INDEX_NONE, Num);
	for (int32 Bone = Num - 1; Bone >= 0; --Bone) {
		if (!Simulated[Bone]) {
			continue;
		}
		if (RelevantChildCount[Bone] == 0) {
			PathTip[Bone] = Bone;
		} else if (RelevantChildCount[Bone] == 1) {
			PathTip[Bone] = PathTip[LastRelevantChild[Bone]];
		}
	}

	struct FFoundChain
	{
		int32 Root = INDEX_NONE;
		int32 Tip = INDEX_NONE;
	};
	TArray<FFoundChain> Found;
	TArray<FString> SingleBones;
	TArray<FString> BranchBones;
	TArray<int32> Stack;
	for (int32 Bone = Num - 1; Bone >= 0; --Bone) {
		if (Parents[Bone] == INDEX_NONE && Relevant[Bone]) {
			Stack.Add(Bone);
		}
	}
	while (!Stack.IsEmpty()) {
		const int32 Bone = Stack.Pop(EAllowShrinking::No);
		if (Simulated[Bone] && PathTip[Bone] != INDEX_NONE) {
			if (PathTip[Bone] == Bone) {
				SingleBones.Add(BoneNames[Bone].ToString());
			} else if (Parents[Bone] == INDEX_NONE) {
				OutNotes.Add(FString::Printf(TEXT("%s 是骨架根骨骼，没有可固定的父骨骼，已跳过。"), *BoneNames[Bone].ToString()));
			} else {
				Found.Add({Bone, PathTip[Bone]});
			}
			continue;
		}
		if (Simulated[Bone] && RelevantChildCount[Bone] > 1) {
			BranchBones.Add(BoneNames[Bone].ToString());
		}
		for (const int32 Child : Children[Bone]) {
			if (Relevant[Child]) {
				Stack.Add(Child);
			}
		}
	}
	Found.Sort([](const FFoundChain& A, const FFoundChain& B) { return A.Root < B.Root; });

	TSet<FName> Ignored;
	Ignored.Append(IgnoredTargetRoots);
	Ignored.Remove(NAME_None);
	TSet<FName> MatchedIgnored;
	for (FFoundChain& Chain : Found) {
		if (Ignored.Contains(BoneNames[Chain.Root])) {
			MatchedIgnored.Add(BoneNames[Chain.Root]);
			continue;
		}
		// Single-relevant-child ancestors own no other chain, so lifting cannot create overlaps.
		for (int32 Ancestor = Parents[Chain.Root];
			 Ancestor != INDEX_NONE && Parents[Ancestor] != INDEX_NONE && RelevantChildCount[Ancestor] == 1;
			 Ancestor = Parents[Ancestor])
		{
			if (Ignored.Contains(BoneNames[Ancestor])) {
				OutNotes.Add(FString::Printf(TEXT("骨链 %s 已上移到 %s：衣物代理的“忽略目标骨链”在运行时按骨链根名精确匹配。"),
											 *BoneNames[Chain.Root].ToString(), *BoneNames[Ancestor].ToString()));
				Chain.Root = Ancestor;
				MatchedIgnored.Add(BoneNames[Ancestor]);
				break;
			}
		}
	}
	for (const FName Name : Ignored) {
		const int32* Bone = IndexOf.Find(Name);
		if (!Bone || MatchedIgnored.Contains(Name)) {
			continue;
		}
		TArray<FString> Covered;
		FString Container;
		for (const FFoundChain& Chain : Found) {
			if (IsInSubtree(Chain.Root, *Bone, Parents)) {
				Covered.Add(BoneNames[Chain.Root].ToString());
			} else if (IsInSubtree(*Bone, Chain.Root, Parents)) {
				Container = BoneNames[Chain.Root].ToString();
			}
		}
		if (!Covered.IsEmpty()) {
			OutNotes.Add(FString::Printf(TEXT("忽略规则 %s 覆盖多条骨链（%s），运行时按骨链根名精确匹配，该规则不会生效；如需忽略，请在衣物代理中改填这些骨链根名。"),
										 *Name.ToString(), *FString::Join(Covered, TEXT("、"))));
		} else if (!Container.IsEmpty()) {
			OutNotes.Add(FString::Printf(TEXT("忽略规则 %s 位于骨链 %s 内部，运行时不会匹配；如需忽略，请在衣物代理中改填 %s。"),
										 *Name.ToString(), *Container, *Container));
		}
	}

	if (Found.Num() > MaxChains) {
		Error = FString::Printf(TEXT("推断出 %d 条骨链，超过运行时上限 %d 条。请减少 KawaiiPhysics 节点或手动合并骨链。"), Found.Num(), MaxChains);
		return false;
	}
	for (const FFoundChain& Chain : Found) {
		FKawaiiPhysicsIKCorrectionChain& Out = OutChains.AddDefaulted_GetRef();
		Out.RootBone.BoneName = BoneNames[Chain.Root];
		Out.TipBone.BoneName = BoneNames[Chain.Tip];
		// The live parent lies outside every corrected subtree. Zero weight keeps the anchor-only warp,
		// which is exact for the automatic proxy node because its baseline is the current pose.
		Out.DriverBone.BoneName = BoneNames[Parents[Chain.Root]];
		Out.DriverWeight = 0.f;
		float ChainRadius = 0.f;
		for (int32 Bone = Chain.Tip; Bone != INDEX_NONE; Bone = Parents[Bone]) {
			ChainRadius = FMath::Max(ChainRadius, Radius[Bone]);
			if (Bone == Chain.Root) {
				break;
			}
		}
		Out.CollisionRadius = ChainRadius;
	}
	if (!BranchBones.IsEmpty()) {
		OutNotes.Add(FString::Printf(TEXT("分叉骨骼 %s 本身保持输入姿势，其下每个分支各生成一条骨链。"), *FString::Join(BranchBones, TEXT("、"))));
	}
	if (!SingleBones.IsEmpty()) {
		OutNotes.Add(FString::Printf(TEXT("单骨骼末端 %s 没有可修正的子骨骼，已跳过。"), *FString::Join(SingleBones, TEXT("、"))));
	}
	return true;
}

FKawaiiPhysicsIKCorrectionCapsule MakeBoneCapsule(FName Bone, const FVector& Center, const FRotator& Rotation, float Radius, float Length)
{
	FKawaiiPhysicsIKCorrectionCapsule Capsule;
	Capsule.StartBone.BoneName = Bone;
	Capsule.EndBone.BoneName = Bone;
	const FVector HalfAxis = Rotation.Quaternion().GetAxisZ() * (Length * .5);
	Capsule.StartOffset = Center - HalfAxis;
	Capsule.EndOffset = Center + HalfAxis;
	Capsule.Radius = Radius;
	return Capsule;
}

UKawaiiPhysicsIKCorrectionProfile* Generate(UAnimBlueprint* Blueprint, const FOptions& Options, FString& OutReport)
{
	check(IsInGameThread());
	TArray<FString> Lines;
	const auto Finish = [&]() { OutReport = FString::Join(Lines, TEXT("\n")); };
	const auto Fail = [&](const FString& Message) -> UKawaiiPhysicsIKCorrectionProfile* {
		Lines.Add(Message + TEXT(" 未修改任何资产。"));
		Finish();
		return nullptr;
	};
	if (!IsValid(Blueprint)) {
		return Fail(TEXT("未提供动画蓝图。"));
	}
	const USkeleton* Skeleton = Blueprint->TargetSkeleton;
	if (!Skeleton) {
		return Fail(TEXT("动画蓝图没有目标骨架。"));
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	TArray<FName> BoneNames;
	TArray<int32> Parents;
	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone) {
		BoneNames.Add(Ref.GetBoneName(Bone));
		Parents.Add(Ref.GetParentIndex(Bone));
	}

	TArray<UAnimGraphNode_KawaiiPhysics*> PhysicsNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, PhysicsNodes);
	PhysicsNodes.RemoveAll([](const UAnimGraphNode_KawaiiPhysics* Node) { return !IsValid(Node); });
	const int32 DisabledNodes = PhysicsNodes.RemoveAll([](const UAnimGraphNode_KawaiiPhysics* Node) { return !Node->IsNodeEnabled(); });
	TArray<FSimulatedRoot> Roots;
	for (const UAnimGraphNode_KawaiiPhysics* GraphNode : PhysicsNodes) {
		const FAnimNode_KawaiiPhysics& Node = GraphNode->Node;
		const float NodeRadius = FMath::Max(0.f, Node.PhysicsSettings.Radius * RadiusScale(Node.RadiusCurveData));
		const auto AddRoot = [&](FName RootBone, const TArray<FBoneReference>& Excludes) {
			if (RootBone.IsNone()) {
				return;
			}
			FSimulatedRoot& Root = Roots.AddDefaulted_GetRef();
			Root.RootBone = RootBone;
			for (const FBoneReference& Exclude : Excludes) {
				Root.ExcludeBones.Add(Exclude.BoneName);
			}
			Root.CollisionRadius = NodeRadius;
		};
		AddRoot(Node.RootBone.BoneName, Node.ExcludeBones);
		for (const FKawaiiPhysicsRootBoneSetting& Extra : Node.AdditionalRootBones) {
			AddRoot(Extra.RootBone.BoneName, Extra.bUseOverrideExcludeBones ? Extra.OverrideExcludeBones : Node.ExcludeBones);
		}
	}
	if (Roots.IsEmpty()) {
		return Fail(TEXT("动画蓝图中没有设置了根骨骼的启用 KawaiiPhysics 节点，无法推断需要修正的骨链；请手动创建 IK 修正配置。"));
	}

	// Garment ignore rules decide which chain root names must be preserved.
	TArray<UAnimGraphNode_KawaiiAutoClothingProxy*> ProxyNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, ProxyNodes);
	ProxyNodes.RemoveAll([](const UAnimGraphNode_KawaiiAutoClothingProxy* Node) { return !IsValid(Node); });
	TArray<FName> Ignored;
	int32 LinkedSets = 0;
	for (const UAnimGraphNode_KawaiiAutoClothingProxy* ProxyNode : ProxyNodes) {
		bool bLinked = false;
		TArray<UKawaiiClothingProxyMotion*> Motions;
		if (const UKawaiiClothingProxyMotionSet* Set = GetMotionSet(*ProxyNode, bLinked)) {
			Motions.Append(Set->Motions);
		}
		LinkedSets += bLinked ? 1 : 0;
		Motions.Append(ProxyNode->Node.ProxyMotions);
		for (const UKawaiiClothingProxyMotion* Motion : Motions) {
			if (IsValid(Motion) && IsValid(Motion->Definition)) {
				for (const FName Name : Motion->Definition->IgnoredTargetRootBones) {
					Ignored.AddUnique(Name);
				}
			}
		}
	}

	TArray<FKawaiiPhysicsIKCorrectionChain> Chains;
	TArray<FString> Notes;
	FString Error;
	if (!BuildChains(BoneNames, Parents, Roots, Ignored, Chains, Notes, Error)) {
		return Fail(Error);
	}
	if (Chains.IsEmpty()) {
		Lines.Append(Notes);
		return Fail(TEXT("KawaiiPhysics 骨骼中没有至少包含两节骨骼的单路径骨链。"));
	}

	TArray<uint8> InsideChain;
	InsideChain.Init(0, BoneNames.Num());
	for (const FKawaiiPhysicsIKCorrectionChain& Chain : Chains) {
		InsideChain[Ref.FindBoneIndex(Chain.RootBone.BoneName)] = 1;
	}
	for (int32 Bone = 0; Bone < InsideChain.Num(); ++Bone) {
		if (Parents[Bone] != INDEX_NONE && InsideChain[Parents[Bone]]) {
			InsideChain[Bone] = 1;
		}
	}

	TArray<FKawaiiPhysicsIKCorrectionCapsule> Capsules;
	int32 Duplicates = 0, SkippedInside = 0, SkippedMissing = 0, SkippedInner = 0, SkippedInvalid = 0, Truncated = 0;
	int32 BoxOrPlanar = 0;
	TSet<FString> PhysicsAssets;
	if (Options.bImportBodyCapsules) {
		const auto AddShape = [&](FName Bone, const FVector& Center, const FRotator& Rotation, float ShapeRadius, float Length) {
			const int32 Index = Ref.FindBoneIndex(Bone);
			if (Index == INDEX_NONE) {
				++SkippedMissing;
				return;
			}
			if (!FMath::IsFinite(ShapeRadius) || ShapeRadius <= 0.f || !FMath::IsFinite(Length) || Length < 0.f
				|| Center.ContainsNaN() || Rotation.ContainsNaN())
			{
				++SkippedInvalid;
				return;
			}
			// The runtime rejects the whole profile when a body capsule moves with a corrected bone.
			if (InsideChain[Index]) {
				++SkippedInside;
				return;
			}
			const FKawaiiPhysicsIKCorrectionCapsule Capsule = MakeBoneCapsule(Bone, Center, Rotation, ShapeRadius, Length);
			if (Capsules.ContainsByPredicate([&](const FKawaiiPhysicsIKCorrectionCapsule& Existing) { return SameCapsule(Existing, Capsule); })) {
				++Duplicates;
				return;
			}
			if (Capsules.Num() >= MaxCapsules) {
				++Truncated;
				return;
			}
			Capsules.Add(Capsule);
		};
		const auto AddLimits = [&](const TArray<FCapsuleLimit>& CapsuleLimits, const TArray<FSphericalLimit>& SphericalLimits) {
			for (const FCapsuleLimit& Limit : CapsuleLimits) {
				AddShape(Limit.DrivingBone.BoneName, Limit.OffsetLocation, Limit.OffsetRotation, Limit.Radius, Limit.Length);
			}
			for (const FSphericalLimit& Limit : SphericalLimits) {
				if (Limit.LimitType != ESphericalLimitType::Outer) {
					++SkippedInner;
					continue;
				}
				AddShape(Limit.DrivingBone.BoneName, Limit.OffsetLocation, Limit.OffsetRotation, Limit.Radius, 0.f);
			}
		};
		for (const UAnimGraphNode_KawaiiPhysics* GraphNode : PhysicsNodes) {
			const FAnimNode_KawaiiPhysics& Node = GraphNode->Node;
			AddLimits(Node.CapsuleLimits, Node.SphericalLimits);
			BoxOrPlanar += Node.BoxLimits.Num() + Node.PlanarLimits.Num();
			if (const UKawaiiPhysicsLimitsDataAsset* Limits = Node.LimitsDataAsset) {
				AddLimits(Limits->CapsuleLimits, Limits->SphericalLimits);
				BoxOrPlanar += Limits->BoxLimits.Num() + Limits->PlanarLimits.Num();
			}
			if (Node.PhysicsAssetForLimits) {
				PhysicsAssets.Add(Node.PhysicsAssetForLimits->GetPathName());
			}
		}
	}

	const FString PackagePath = Options.PackagePath.IsEmpty()
		? FPackageName::GetLongPackagePath(Blueprint->GetOutermost()->GetName())
		: Options.PackagePath;
	const FString AssetName = FString::Printf(TEXT("DA_%s_IKCorrection"), *Blueprint->GetName());
	const FString PackageName = PackagePath / AssetName;
	FText PathError;
	// Read-only roots include /Temp, which transient tooling and tests use; saving still reports any write failure.
	if (!FPackageName::IsValidLongPackageName(PackageName, true, &PathError)) {
		return Fail(FString::Printf(TEXT("配置资产路径 %s 无效：%s"), *PackageName, *PathError.ToString()));
	}

	Lines.Add(FString::Printf(TEXT("来源：%d 个启用的 KawaiiPhysics 节点（%d 个根骨骼）%s；目标骨架 %s。"),
							  PhysicsNodes.Num(), Roots.Num(),
							  DisabledNodes > 0 ? *FString::Printf(TEXT("，忽略 %d 个已禁用节点"), DisabledNodes) : TEXT(""),
							  *Skeleton->GetName()));
	Lines.Add(FString::Printf(TEXT("骨链 %d 条（驱动骨骼取根骨骼的父骨骼，驱动权重 0）："), Chains.Num()));
	for (const FKawaiiPhysicsIKCorrectionChain& Chain : Chains) {
		Lines.Add(FString::Printf(TEXT("  %s → %s　锚点/驱动 %s　半径 %.2f cm"), *Chain.RootBone.BoneName.ToString(),
								  *Chain.TipBone.BoneName.ToString(), *Chain.DriverBone.BoneName.ToString(), Chain.CollisionRadius));
	}
	for (const FString& Note : Notes) {
		Lines.Add(TEXT("· ") + Note);
	}
	if (!Ignored.IsEmpty()) {
		TArray<FString> Names;
		for (const FName Name : Ignored) {
			Names.Add(Name.ToString());
		}
		Lines.Add(FString::Printf(TEXT("· 已参考衣物代理的忽略目标骨链：%s。"), *FString::Join(Names, TEXT("、"))));
	}
	if (LinkedSets > 0) {
		Lines.Add(FString::Printf(TEXT("· %d 个节点的代理动作集合由连线提供，无法静态读取其忽略规则。"), LinkedSets));
	}
	if (Options.bImportBodyCapsules) {
		Lines.Add(FString::Printf(TEXT("身体碰撞体 %d 个（来自 KawaiiPhysics 外侧球体与胶囊限制；去重 %d 个）。"), Capsules.Num(), Duplicates));
		TArray<FString> Skips;
		if (SkippedInside > 0) { Skips.Add(FString::Printf(TEXT("绑定在被修正骨链内 %d 个"), SkippedInside)); }
		if (SkippedMissing > 0) { Skips.Add(FString::Printf(TEXT("骨骼不存在 %d 个"), SkippedMissing)); }
		if (SkippedInner > 0) { Skips.Add(FString::Printf(TEXT("内侧球体 %d 个"), SkippedInner)); }
		if (SkippedInvalid > 0) { Skips.Add(FString::Printf(TEXT("尺寸无效 %d 个"), SkippedInvalid)); }
		if (Truncated > 0) { Skips.Add(FString::Printf(TEXT("超过 %d 个上限 %d 个"), MaxCapsules, Truncated)); }
		if (BoxOrPlanar > 0) { Skips.Add(FString::Printf(TEXT("Box/Planar 限制 %d 个（无对应形状）"), BoxOrPlanar)); }
		if (!PhysicsAssets.IsEmpty()) { Skips.Add(FString::Printf(TEXT("物理资产限制 %d 个（未导入）"), PhysicsAssets.Num())); }
		if (!Skips.IsEmpty()) {
			Lines.Add(TEXT("· 跳过：") + FString::Join(Skips, TEXT("；")) + TEXT("。"));
		}
	} else {
		Lines.Add(TEXT("未导入身体碰撞体，只与衣物代理碰撞。"));
	}
	if (ProxyNodes.IsEmpty()) {
		Lines.Add(TEXT("· 动画蓝图中没有自动衣物代理修正节点；配置仍可手动指定给其他节点。"));
	}

	if (Options.bDryRun) {
		Lines.Insert(FString::Printf(TEXT("预览（未写入）：%s"), *PackageName), 0);
		Finish();
		return nullptr;
	}

	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UKawaiiPhysicsIKCorrectionProfile* Profile = nullptr;
	if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName)) {
		Profile = Cast<UKawaiiPhysicsIKCorrectionProfile>(StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		if (!Profile) {
			Lines.Reset();
			return Fail(FString::Printf(TEXT("%s 已存在但不是 IK 修正配置，未覆盖。"), *PackageName));
		}
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("生成 Kawaii IK 修正配置")));
	const bool bCreated = Profile == nullptr;
	if (bCreated) {
		UPackage* Package = CreatePackage(*PackageName);
		Package->FullyLoad();
		Profile = NewObject<UKawaiiPhysicsIKCorrectionProfile>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Profile);
	} else {
		Profile->SetFlags(RF_Transactional);
		Profile->Modify();
	}
	Profile->Chains = MoveTemp(Chains);
	Profile->Capsules = MoveTemp(Capsules);
	// Imported capsules are bound to this blueprint's skeleton, not to an attach parent.
	Profile->bUseAttachedParentForCollision = false;
	Profile->PostEditChange();
	Profile->MarkPackageDirty();
	Lines.Insert(FString::Printf(TEXT("%s IK 修正配置：%s"), bCreated ? TEXT("已新建") : TEXT("已重新生成（覆盖原有骨链与碰撞体）"), *ObjectPath), 0);

	const bool bBlueprintWasDirty = Blueprint->GetOutermost()->IsDirty();
	int32 Assigned = 0;
	int32 AlreadyAssigned = 0;
	TArray<FString> Kept;
	if (Options.bAssignToProxyNodes) {
		for (UAnimGraphNode_KawaiiAutoClothingProxy* ProxyNode : ProxyNodes) {
			UEdGraphPin* Pin = ProxyNode->FindPin(GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, Profile), EGPD_Input);
			if (Pin && !Pin->LinkedTo.IsEmpty()) {
				Kept.Add(TEXT("Profile 由连线提供"));
				continue;
			}
			const UObject* Current = Pin ? Pin->DefaultObject.Get() : ProxyNode->Node.Profile.Get();
			if (Current && Current != Profile) {
				Kept.Add(FString::Printf(TEXT("已使用 %s"), *Current->GetPathName()));
				continue;
			}
			if (Current == Profile && ProxyNode->Node.Profile == Profile) {
				++AlreadyAssigned;
				continue;
			}
			ProxyNode->Modify();
			ProxyNode->Node.Profile = Profile;
			if (Pin) {
				Pin->GetSchema()->TrySetDefaultObject(*Pin, Profile);
			}
			++Assigned;
		}
	}
	bool bCompiled = true;
	if (Assigned > 0) {
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bCompiled = Blueprint->Status != BS_Error;
		Lines.Add(FString::Printf(TEXT("已赋给 %d 个自动衣物代理修正节点，蓝图编译%s。"), Assigned, bCompiled ? TEXT("成功") : TEXT("失败，请在编辑器中查看编译结果")));
	}
	if (AlreadyAssigned > 0) {
		Lines.Add(FString::Printf(TEXT("%d 个自动衣物代理修正节点已在使用该配置。"), AlreadyAssigned));
	}
	if (!Kept.IsEmpty()) {
		Lines.Add(TEXT("· 未改动的节点：") + FString::Join(Kept, TEXT("；")) + TEXT("。"));
	}

	if (Options.bSave) {
		Lines.Add(SaveAsset(Profile) ? TEXT("配置已保存。") : TEXT("配置保存失败，修改仍保留在编辑器内，请检查文件权限后手动保存。"));
		if (Assigned > 0) {
			if (!bCompiled) {
				Lines.Add(TEXT("蓝图编译失败，未自动保存蓝图。"));
			} else if (bBlueprintWasDirty) {
				Lines.Add(TEXT("动画蓝图此前已有未保存的修改，未自动保存，请确认后手动保存。"));
			} else {
				Lines.Add(SaveAsset(Blueprint) ? TEXT("动画蓝图已保存。") : TEXT("动画蓝图保存失败，请手动保存。"));
			}
		}
	}
	Lines.Add(TEXT("提示：重新生成会覆盖配置中的骨链和碰撞体；低 LOD 缺少骨链骨骼时运行时会整体回退为输入姿势。"));
	Finish();
	return Profile;
}

bool HasSelectedAnimBlueprints()
{
	TArray<FAssetData> Selected;
	GetSelection(Selected);
	return Selected.ContainsByPredicate([](const FAssetData& Asset) { return Asset.IsInstanceOf(UAnimBlueprint::StaticClass()); });
}

void GenerateForSelectedAnimBlueprints()
{
	check(IsInGameThread());
	TArray<FAssetData> Selected;
	GetSelection(Selected);
	TArray<FString> Reports;
	TArray<UObject*> Generated;
	for (const FAssetData& Asset : Selected) {
		if (!Asset.IsInstanceOf(UAnimBlueprint::StaticClass())) {
			continue;
		}
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Asset.GetAsset());
		if (!Blueprint) {
			Reports.Add(FString::Printf(TEXT("【%s】加载失败。"), *Asset.AssetName.ToString()));
			continue;
		}
		FString Report;
		if (UKawaiiPhysicsIKCorrectionProfile* Profile = Generate(Blueprint, FOptions(), Report)) {
			Generated.Add(Profile);
		}
		Reports.Add(FString::Printf(TEXT("【%s】\n%s"), *Blueprint->GetName(), *Report));
	}
	if (Reports.IsEmpty()) {
		Reports.Add(TEXT("请先在内容浏览器选择动画蓝图。"));
	}
	const FString Message = FString::Join(Reports, TEXT("\n\n"));
	UE_LOG(LogTemp, Display, TEXT("Kawaii IK correction profile generation:\n%s"), *Message);
	if (!Generated.IsEmpty()) {
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Generated);
	}
	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
}
}
