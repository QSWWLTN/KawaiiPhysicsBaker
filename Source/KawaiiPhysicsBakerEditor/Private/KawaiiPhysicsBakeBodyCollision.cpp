#include "KawaiiPhysicsBakeBodyCollision.h"
#include "AnimNode_KawaiiPhysics.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "KawaiiClothingProxyAsset.h"
#include "UObject/UnrealType.h"

bool KawaiiPhysicsBakeBodyCollision::Update(const UKawaiiClothingProxyAsset& Definition, const USkeletalMeshComponent& Body, USkeletalMeshComponent& Garment, FString& Error) {
	const auto* Mesh = Body.GetSkeletalMeshAsset();
	auto* Instance = Garment.GetAnimInstance();
	const auto* Interface = Instance ? IAnimClassInterface::GetFromClass(Instance->GetClass()) : nullptr;
	if (!Mesh || !Interface) { Error = TEXT("身体代理缺少 Main 网格体或衣物动画实例。"); return false; }
	const auto& Reference = Mesh->GetRefSkeleton();
	const auto& Pose = Body.GetComponentSpaceTransforms();
	TArray<KawaiiContact::FCapsule> Contacts;
	for (const auto& Shape : Definition.Shapes) {
		const int32 A = Reference.FindBoneIndex(Shape.StartBone), B = Reference.FindBoneIndex(Shape.EndBone.IsNone() ? Shape.StartBone : Shape.EndBone);
		if (!Pose.IsValidIndex(A) || !Pose.IsValidIndex(B)) { Error = TEXT("身体代理端点骨骼不在 Main 当前姿态中。"); return false; }
		FKawaiiClothingCapsuleDefinition CapsuleDefinition;
		CapsuleDefinition.StartBone = Shape.StartBone; CapsuleDefinition.EndBone = Shape.EndBone;
		CapsuleDefinition.StartOffset = Shape.StartOffset; CapsuleDefinition.EndOffset = Shape.EndOffset; CapsuleDefinition.Radius = Shape.Radius;
		FKawaiiClothingCapsule Capsule;
		if (!KawaiiClothingCollision::BuildCapsuleFromComponentSpace(CapsuleDefinition, Pose[A], Pose[B], Body.GetComponentTransform(), Garment.GetComponentTransform(), Capsule)) {
			Error = TEXT("身体代理转换至衣物组件空间失败。"); return false;
		}
		Contacts.Add({Capsule.Start, Capsule.End, Capsule.Radius});
	}
	int32 Nodes = 0;
	for (const FStructProperty* Property : Interface->GetAnimNodeProperties()) {
		if (Property->Struct->IsChildOf(FAnimNode_KawaiiPhysics::StaticStruct())) {
			auto* Node = Property->ContainerPtrToValuePtr<FAnimNode_KawaiiPhysics>(Instance);
			Node->SetExternalContactCapsules(Definition.IgnoredTargetRootBones.Contains(Node->RootBone.BoneName) ? TArray<KawaiiContact::FCapsule>() : Contacts);
			++Nodes;
		}
	}
	if (!Nodes) { Error = TEXT("已开启身体代理碰撞，但衣物图中没有 KawaiiPhysics 节点。"); return false; }
	return true;
}
