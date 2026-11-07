#include "KawaiiPhysicsIKEditorLibrary.h"

#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_ModifyBone.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "KawaiiPhysicsIKProfileGenerator.h"
#include "KawaiiPhysicsIKMeshEnvelope.h"
#include "Engine/SkeletalMesh.h"

namespace {
	UEdGraphPin* FindComponentPosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction) {
		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin->Direction == Direction && Pin->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct()) {
				return Pin;
			}
		}
		return nullptr;
	}
}

bool UKawaiiPhysicsIKEditorLibrary::RefreshIKMeshEnvelopes(UKawaiiPhysicsIKCorrectionProfile* Profile, USkeletalMesh* Mesh, FString& Report) {
	if (!Profile || !Mesh) { Report = TEXT("Profile and mesh are required"); return false; }
	TArray<float> Radii;
	if (!KawaiiPhysicsIKMeshEnvelope::Measure(*Mesh, Profile->Chains, Radii, Report)) { return false; }
	Profile->Modify();
	TArray<FString> Lines;
	for (int32 C = 0; C < Radii.Num(); ++C) {
		auto& Chain = Profile->Chains[C];
		Chain.MeshEnvelopeRadius = Radii[C];
		Lines.Add(FString::Printf(TEXT("%s: authored %.3f, mesh envelope %.3f cm"), *Chain.RootBone.BoneName.ToString(), Chain.CollisionRadius, Radii[C]));
	}
	Profile->MeshEnvelopeSource = Mesh;
	Profile->PostEditChange();
	Profile->MarkPackageDirty();
	Report = FString::Join(Lines, TEXT("\n"));
	return true;
}

bool UKawaiiPhysicsIKEditorLibrary::BindUseCachedPose(UEdGraphNode* UseNode, UEdGraphNode* SaveNode) {
	UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(UseNode);
	UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(SaveNode);
	if (!IsValid(Use) || !IsValid(Save) || !IsValid(Use->GetGraph()) || Use->GetGraph() != Save->GetGraph()) {
		return false;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Use->GetGraph());
	if (!IsValid(Blueprint) || Save->CacheName.IsEmpty()) {
		return false;
	}

	Use->Modify();
	Use->SaveCachedPoseNode = Save;
	// The title updates the engine's serialized cache-name fallback as well.
	Use->GetNodeTitle(ENodeTitleType::FullTitle);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Use->SaveCachedPoseNode.Get() == Save;
}

UEdGraphNode* UKawaiiPhysicsIKEditorLibrary::InsertBoneRotationBeforeNode(UEdGraphNode* TargetNode, FName NodeName, FName BoneName, FRotator Rotation, const FString& Comment) {
	UEdGraph* Graph = IsValid(TargetNode) ? TargetNode->GetGraph() : nullptr;
	UAnimBlueprint* Blueprint = Graph ? Cast<UAnimBlueprint>(FBlueprintEditorUtils::FindBlueprintForGraph(Graph)) : nullptr;
	if (!Blueprint || !Blueprint->TargetSkeleton || NodeName.IsNone() || Rotation.ContainsNaN()
		|| Blueprint->TargetSkeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
	{
		return nullptr;
	}
	UEdGraphPin* Input = FindComponentPosePin(TargetNode, EGPD_Input);
	if (!Input || Input->LinkedTo.Num() != 1) {
		return nullptr;
	}

	UObject* Existing = StaticFindObjectFast(UObject::StaticClass(), Graph, NodeName);
	UAnimGraphNode_ModifyBone* Guide = Cast<UAnimGraphNode_ModifyBone>(Existing);
	if (Existing && !Guide) {
		return nullptr;
	}
	Blueprint->Modify();
	Graph->Modify();
	if (!Guide) {
		FGraphNodeCreator<UAnimGraphNode_ModifyBone> Creator(*Graph);
		Guide = Creator.CreateNode();
		Creator.Finalize();
		Guide->Rename(*NodeName.ToString(), Graph, REN_DontCreateRedirectors);
		Guide->NodePosX = TargetNode->NodePosX - 260;
		Guide->NodePosY = TargetNode->NodePosY + 180;
		UEdGraphNode* Previous = Input->LinkedTo[0]->GetOwningNode();
		while (UAnimGraphNode_ModifyBone* PreviousGuide = Cast<UAnimGraphNode_ModifyBone>(Previous)) {
			PreviousGuide->Modify();
			PreviousGuide->NodePosX -= 260;
			UEdGraphPin* PreviousInput = FindComponentPosePin(PreviousGuide, EGPD_Input);
			if (!PreviousInput || PreviousInput->LinkedTo.Num() != 1) {
				break;
			}
			Previous = PreviousInput->LinkedTo[0]->GetOwningNode();
		}
		UEdGraphPin* GuideInput = FindComponentPosePin(Guide, EGPD_Input);
		UEdGraphPin* GuideOutput = FindComponentPosePin(Guide, EGPD_Output);
		if (!GuideInput || !GuideOutput) {
			Guide->DestroyNode();
			return nullptr;
		}
		UEdGraphPin* Source = Input->LinkedTo[0];
		TargetNode->Modify();
		Source->GetOwningNode()->Modify();
		Input->BreakLinkTo(Source);
		Source->MakeLinkTo(GuideInput);
		GuideOutput->MakeLinkTo(Input);
	}
	Guide->Modify();
	for (int32 Index = 0; Index < Guide->ShowPinForProperties.Num(); ++Index) {
		const FName PropertyName = Guide->ShowPinForProperties[Index].PropertyName;
		if (PropertyName == TEXT("Rotation") || PropertyName == TEXT("Translation") || PropertyName == TEXT("Scale")) {
			Guide->SetPinVisibility(false, Index);
		}
	}
	Guide->Node.BoneToModify.BoneName = BoneName;
	Guide->Node.RotationMode = BMM_Additive;
	Guide->Node.RotationSpace = BCS_ParentBoneSpace;
	Guide->Node.Rotation = Rotation;
	Guide->NodeComment = Comment;
	Guide->bCommentBubbleVisible = true;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Guide;
}

FString UKawaiiPhysicsIKEditorLibrary::GetBlueprintStatus(const UBlueprint* Blueprint) {
	return IsValid(Blueprint) ? UEnum::GetValueAsString(Blueprint->Status.GetValue()) : TEXT("InvalidBlueprint");
}

bool UKawaiiPhysicsIKEditorLibrary::IsBlueprintUpToDate(const UBlueprint* Blueprint) {
	return IsValid(Blueprint) && Blueprint->IsUpToDate();
}

UKawaiiPhysicsIKCorrectionProfile* UKawaiiPhysicsIKEditorLibrary::GenerateIKCorrectionProfile(UAnimBlueprint* AnimBlueprint, bool bImportBodyCapsules, bool bAssignToProxyNodes, bool bSave, bool bDryRun, FString& Report) {
	KawaiiPhysicsIKProfileGenerator::FOptions Options;
	Options.bImportBodyCapsules = bImportBodyCapsules;
	Options.bAssignToProxyNodes = bAssignToProxyNodes;
	Options.bSave = bSave;
	Options.bDryRun = bDryRun;
	return KawaiiPhysicsIKProfileGenerator::Generate(AnimBlueprint, Options, Report);
}
