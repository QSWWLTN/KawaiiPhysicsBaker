#include "AnimGraphNode_KawaiiPhysicsBakeSource.h"
#include "KawaiiPhysicsBakeAnimInstance.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakeSource"
FText UAnimGraphNode_KawaiiPhysicsBakeSource::GetNodeTitle(ENodeTitleType::Type) const {
	return LOCTEXT("Title", "Bake Source Pose");
}
FText UAnimGraphNode_KawaiiPhysicsBakeSource::GetTooltipText() const {
	return LOCTEXT("Tooltip",
				   "Samples the animation selected in the bake panel at the controlled time. For a part with no animation, copies matching bones from its configured source mesh. Add KawaiiPhysics "
				   "downstream of this node.");
}
FString UAnimGraphNode_KawaiiPhysicsBakeSource::GetNodeCategory() const {
	return TEXT("Kawaii Physics|Baking");
}
FLinearColor UAnimGraphNode_KawaiiPhysicsBakeSource::GetNodeTitleColor() const {
	return FLinearColor(0.08f, 0.5f, 0.42f);
}
void UAnimGraphNode_KawaiiPhysicsBakeSource::GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const {
	if (Registrar.IsOpenForRegistration(GetClass())) {
		Registrar.AddBlueprintAction(GetClass(), UBlueprintNodeSpawner::Create(GetClass()));
	}
}
void UAnimGraphNode_KawaiiPhysicsBakeSource::ValidateAnimNodeDuringCompilation(USkeleton* Skeleton, FCompilerResultsLog& Log) {
	Super::ValidateAnimNodeDuringCompilation(Skeleton, Log);
	const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this);
	if (!Blueprint
		|| !Blueprint->ParentClass
		|| !Blueprint->ParentClass->IsChildOf(UKawaiiPhysicsBakeAnimInstance::StaticClass()))
	{
		Log.Error(TEXT("@@ requires a Kawaii Physics Bake Blueprint (KawaiiPhysicsBakeAnimInstance parent)."), this);
	}
}
#undef LOCTEXT_NAMESPACE
