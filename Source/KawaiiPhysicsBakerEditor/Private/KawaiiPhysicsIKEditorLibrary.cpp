#include "KawaiiPhysicsIKEditorLibrary.h"

#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

bool UKawaiiPhysicsIKEditorLibrary::BindUseCachedPose(UEdGraphNode* UseNode, UEdGraphNode* SaveNode) {
	UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(UseNode);
	UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(SaveNode);
	if (!IsValid(Use)
		|| !IsValid(Save)
		|| !IsValid(Use->GetGraph())
		|| Use->GetGraph() != Save->GetGraph())
	{
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

FString UKawaiiPhysicsIKEditorLibrary::GetBlueprintStatus(const UBlueprint* Blueprint) {
	return IsValid(Blueprint) ? UEnum::GetValueAsString(Blueprint->Status.GetValue()) : TEXT("InvalidBlueprint");
}

bool UKawaiiPhysicsIKEditorLibrary::IsBlueprintUpToDate(const UBlueprint* Blueprint) {
	return IsValid(Blueprint) && Blueprint->IsUpToDate();
}
