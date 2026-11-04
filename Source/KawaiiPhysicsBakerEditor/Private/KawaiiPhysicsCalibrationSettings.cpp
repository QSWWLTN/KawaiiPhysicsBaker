#include "KawaiiPhysicsCalibrationSettings.h"

#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#include "AnimGraphNode_KawaiiPhysics.h"
#include "KawaiiPhysicsLimitsDataAsset.h"

namespace {
	void ImportCapsules(const UKawaiiPhysicsCalibrationSettings& Settings, const FAnimNode_KawaiiPhysics& Node, TArray<FKawaiiClothingCapsuleDefinition>& OutCapsules, FString& OutSource) {
		const auto& Ref = Settings.BodyMesh->GetRefSkeleton();
		const auto AddShape = [&](FName Bone, FVector Center, FRotator Rotation, float Radius, float Length) {
			if (Ref.FindBoneIndex(Bone) == INDEX_NONE || Radius <= 0.f) {
				return;
			}
			FKawaiiClothingCapsuleDefinition Definition;
			Definition.StartBone = Definition.EndBone = Bone;
			const FVector HalfAxis = Rotation.Quaternion().GetAxisZ() * Length * .5;
			Definition.StartOffset = Center - HalfAxis;
			Definition.EndOffset = Center + HalfAxis;
			Definition.Radius = Radius;
			OutCapsules.Add(Definition);
		};
		const auto AddLimits = [&](const auto& Capsules, const auto& Spheres) {
			for (const auto& Capsule : Capsules) {
				AddShape(Capsule.DrivingBone.BoneName, Capsule.OffsetLocation, Capsule.OffsetRotation, Capsule.Radius, Capsule.Length);
			}
			for (const auto& Sphere : Spheres) {
				if (Sphere.LimitType == ESphericalLimitType::Outer) {
					AddShape(Sphere.DrivingBone.BoneName, Sphere.OffsetLocation, Sphere.OffsetRotation, Sphere.Radius, 0.f);
				}
			}
		};
		AddLimits(Node.CapsuleLimits, Node.SphericalLimits);
		if (Node.LimitsDataAsset) {
			AddLimits(Node.LimitsDataAsset->CapsuleLimits, Node.LimitsDataAsset->SphericalLimits);
		}
		OutSource = TEXT("Kawaii 节点及约束资产");
		if (OutCapsules.IsEmpty()) {
			OutSource = TEXT("人物物理资产");
			if (const auto* Physics = Settings.BodyMesh->GetPhysicsAsset()) {
				for (const USkeletalBodySetup* Setup : Physics->SkeletalBodySetups) {
					if (!Setup) {
						continue;
					}
					for (const auto& Shape : Setup->AggGeom.SphylElems) {
						AddShape(Setup->BoneName, Shape.Center, Shape.Rotation, Shape.Radius, Shape.Length);
					}
					for (const auto& Shape : Setup->AggGeom.SphereElems) {
						AddShape(Setup->BoneName, Shape.Center, FRotator::ZeroRotator, Shape.Radius, 0.f);
					}
				}
			}
		}
	}
}

TArray<FString> UKawaiiPhysicsCalibrationSettings::GetPhysicsRootOptions() const {
	TArray<FString> Options;
	if (ClothingBlueprint) {
		TArray<UEdGraph*> Graphs;
		ClothingBlueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs) {
			for (const UEdGraphNode* Node : Graph->Nodes) {
				const auto* PhysicsNode = Cast<UAnimGraphNode_KawaiiPhysics>(Node);
				if (PhysicsNode && !PhysicsNode->Node.RootBone.BoneName.IsNone()) {
					Options.AddUnique(PhysicsNode->Node.RootBone.BoneName.ToString());
				}
			}
		}
	}
	Options.Sort();
	Options.Insert(FName(NAME_None).ToString(), 0);
	return Options;
}

TArray<FString> UKawaiiPhysicsCalibrationSettings::GetBodyBoneOptions() const {
	TArray<FString> Options;
	if (BodyMesh) {
		for (const auto& Bone : BodyMesh->GetRefSkeleton().GetRefBoneInfo()) {
			Options.Add(Bone.Name.ToString());
		}
	}
	return Options;
}

TArray<FString> UKawaiiPhysicsCalibrationSettings::GetChainRootOptions() const {
	TArray<FString> Options;
	if (const auto* Node = FindPhysicsNode()) {
		Options.Add(Node->Node.RootBone.BoneName.ToString());
		for (const auto& Root : Node->Node.AdditionalRootBones) {
			Options.AddUnique(Root.RootBone.BoneName.ToString());
		}
	}
	return Options;
}

UAnimGraphNode_KawaiiPhysics* UKawaiiPhysicsCalibrationSettings::FindPhysicsNode() const {
	UAnimGraphNode_KawaiiPhysics* Found = nullptr;
	if (ClothingBlueprint) {
		TArray<UEdGraph*> Graphs;
		ClothingBlueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs) {
			for (UEdGraphNode* Node : Graph->Nodes) {
				auto* PhysicsNode = Cast<UAnimGraphNode_KawaiiPhysics>(Node);
				if (PhysicsNode && (PhysicsRoot.IsNone() || PhysicsNode->Node.RootBone.BoneName == PhysicsRoot)) {
					if (Found) {
						return nullptr;
					}
					Found = PhysicsNode;
				}
			}
		}
	}
	return Found;
}

bool UKawaiiPhysicsCalibrationSettings::ReadCollisionAndChains(FString& OutMessage) {
	const auto* Node = FindPhysicsNode();
	if (!Node || !BodyMesh) {
		OutMessage = TEXT("请先选择人物网格体，并通过物理根骨骼唯一确定 Kawaii 节点。");
		return false;
	}
	TArray<FKawaiiClothingCapsuleDefinition> Imported;
	FString Source;
	ImportCapsules(*this, Node->Node, Imported, Source);
	if (Imported.IsEmpty() || Imported.Num() > 32) {
		OutMessage = TEXT("读取需要 1 至 32 个身体胶囊或外侧球体。不会读取盒体、凸包和内侧球体。");
		return false;
	}
	CollisionShapes.Reset();
	ReplacedCollisionBones.Reset();
	ChainLimits.Reset();
	PhysicsRoot = Node->Node.RootBone.BoneName;
	for (const auto& Capsule : Imported) {
		auto& Shape = CollisionShapes.AddDefaulted_GetRef();
		Shape.Bone = Capsule.StartBone;
		Shape.Center = (Capsule.StartOffset + Capsule.EndOffset) * .5;
		const FVector Axis = Capsule.EndOffset - Capsule.StartOffset;
		Shape.Length = Axis.Size();
		Shape.Rotation = Shape.Length > UE_SMALL_NUMBER ? FQuat::FindBetweenNormals(FVector::UpVector, Axis / Shape.Length).Rotator() : FRotator::ZeroRotator;
		Shape.Radius = Capsule.Radius;
		Shape.Source = Source;
		ReplacedCollisionBones.AddUnique(Shape.Bone);
	}
	for (const FString& Root : GetChainRootOptions()) {
		auto& Limit = ChainLimits.AddDefaulted_GetRef();
		Limit.RootBone = FName(*Root);
		Limit.LimitAngle = Node->Node.PhysicsSettings.LimitAngle;
		Limit.PlanarConstraint = Node->Node.PlanarConstraint;
	}
	bEditCollisionShapes = true;
	OutMessage = FString::Printf(TEXT("已读取 %d 个碰撞形状、%d 条骨链。编辑下方参数后可校准，也可保存手动配置直接试用；手动配置不代表通过穿模验收。源蓝图和约束资产未修改。"), CollisionShapes.Num(), ChainLimits.Num());
	return true;
}

bool UKawaiiPhysicsCalibrationSettings::ResolveConstraints(FString& OutSource, FString& OutError) {
	const auto* Node = FindPhysicsNode();
	if (!Node || !BodyMesh || !ClothingMesh || ClothingBlueprint->TargetSkeleton != ClothingMesh->GetSkeleton()) {
		OutError = TEXT("请选择人物、衣物网格体和匹配衣物骨架的蓝图，并唯一确定物理节点。");
		return false;
	}
	TArray<FKawaiiClothingCapsuleDefinition> Resolved = BodyCapsules;
	OutSource = TEXT("显式身体胶囊");
	if (bEditCollisionShapes) {
		Resolved.Reset();
		OutSource = TEXT("手动编辑的身体碰撞形状");
		for (const auto& Shape : CollisionShapes) {
			if (!Shape.bEnabled) {
				continue;
			}
			if (Shape.Center.ContainsNaN() || Shape.Rotation.ContainsNaN() || !FMath::IsFinite(Shape.Length) || Shape.Length < 0.f) {
				OutError = TEXT("碰撞形状的位置、旋转和长度必须有效，长度不能为负数。");
				return false;
			}
			FKawaiiClothingCapsuleDefinition Capsule;
			Capsule.StartBone = Capsule.EndBone = Shape.Bone;
			const FVector HalfAxis = Shape.Rotation.Quaternion().GetAxisZ() * Shape.Length * .5;
			Capsule.StartOffset = Shape.Center - HalfAxis;
			Capsule.EndOffset = Shape.Center + HalfAxis;
			Capsule.Radius = Shape.Radius;
			Resolved.Add(Capsule);
		}
	} else if (Resolved.IsEmpty()) {
		ImportCapsules(*this, Node->Node, Resolved, OutSource);
	}
	if (Resolved.IsEmpty() || Resolved.Num() > 32) {
		OutError = TEXT("需要 1 至 32 个启用的身体碰撞形状。全部禁用不会自动恢复旧碰撞。");
		return false;
	}
	const auto& Ref = BodyMesh->GetRefSkeleton();
	for (const auto& Capsule : Resolved) {
		if (Capsule.bUseSockets || Ref.FindBoneIndex(Capsule.StartBone) == INDEX_NONE
			|| (!Capsule.EndBone.IsNone() && Ref.FindBoneIndex(Capsule.EndBone) == INDEX_NONE)
			|| !FMath::IsFinite(Capsule.Radius) || Capsule.Radius <= 0.f
			|| Capsule.StartOffset.ContainsNaN() || Capsule.EndOffset.ContainsNaN())
		{
			OutError = TEXT("身体碰撞参数无效或人物骨架中缺少绑定骨骼；半径必须大于 0，不支持插槽。");
			return false;
		}
	}
	for (FName Bone : ReplacedCollisionBones) {
		if (Ref.FindBoneIndex(Bone) == INDEX_NONE) {
			OutError = TEXT("替换原碰撞的骨骼已不属于当前人物，请重新读取碰撞与骨链。");
			return false;
		}
	}
	if (bSearchNodeSettings && !bUseAttachedParentForContacts) {
		const auto& ClothingRef = ClothingMesh->GetRefSkeleton();
		for (const auto& Capsule : Resolved) {
			for (FName Bone : {Capsule.StartBone, Capsule.EndBone.IsNone() ? Capsule.StartBone : Capsule.EndBone}) {
				if (ClothingRef.FindBoneIndex(Bone) == INDEX_NONE) {
					OutError = FString::Printf(TEXT("衣物输入缺少身体碰撞骨骼 %s。请补齐衣物输入骨骼，或启用“从挂接人物读取身体碰撞”并确保运行时挂接关系正确。"), *Bone.ToString());
					return false;
				}
			}
		}
	}
	const TArray<FString> Roots = GetChainRootOptions();
	TSet<FName> Seen;
	for (const auto& Limit : ChainLimits) {
		if (!Roots.Contains(Limit.RootBone.ToString()) || Seen.Contains(Limit.RootBone)
			|| ClothingMesh->GetRefSkeleton().FindBoneIndex(Limit.RootBone) == INDEX_NONE
			|| !FMath::IsFinite(Limit.LimitAngle) || Limit.LimitAngle < 0.f || Limit.LimitAngle > 180.f
			|| static_cast<uint8>(Limit.PlanarConstraint) > static_cast<uint8>(EPlanarConstraint::Z))
		{
			OutError = TEXT("骨链必须是所选节点的主根或附加根，不能重复；角度须在 0 至 180 度内。更换节点后请重新读取。");
			return false;
		}
		Seen.Add(Limit.RootBone);
	}
	if (!FMath::IsFinite(SoftMargin) || SoftMargin < 0.f || SoftMargin > 5.f || !FMath::IsFinite(SoftResponse) || SoftResponse < 0.f || SoftResponse > 100.f) {
		OutError = TEXT("柔性缓冲距离须在 0 至 5 厘米内，柔性响应速度须在 0 至 100 内。");
		return false;
	}
	BodyCapsules = MoveTemp(Resolved);
	return true;
}

void UKawaiiPhysicsCalibrationSettings::CopyConstraintsToProfile(UKawaiiPhysicsCalibrationProfile& Profile) const {
	Profile.bUseAttachedParent = !bSearchNodeSettings || bUseAttachedParentForContacts;
	Profile.BodyCapsules = BodyCapsules;
	Profile.ReplacedCollisionBones = ReplacedCollisionBones;
	Profile.ChainLimits = ChainLimits;
	Profile.SoftMargin = SoftMargin;
	Profile.SoftResponse = SoftResponse;
}
