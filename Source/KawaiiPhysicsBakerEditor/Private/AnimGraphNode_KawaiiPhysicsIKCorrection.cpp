#include "AnimGraphNode_KawaiiPhysicsIKCorrection.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsIKCorrection"

FText UAnimGraphNode_KawaiiPhysicsIKCorrection::GetNodeTitle(ENodeTitleType::Type) const {
	return LOCTEXT("Title", "Baked Clothing IK Correction");
}

FText UAnimGraphNode_KawaiiPhysicsIKCorrection::GetTooltipText() const {
	return LOCTEXT(
		"Tooltip",
		"Keeps the live Driver Pose body bones, restores selected baked clothing subtrees, and adapts their chains to body IK with capsule constraints. Both inputs must use the same animation time " "and this mesh's Skeleton. Driver Pose must be evaluated after body IK. Clothing Proxy Capsules accepts current-frame custom proxy-motion samples in this mesh's component space, " "including for baked hair chains. Alpha below one weakens collision correction. This is approximate bone collision, not triangle-mesh collision."
	);
}

FString UAnimGraphNode_KawaiiPhysicsIKCorrection::GetNodeCategory() const {
	return TEXT("Kawaii Physics|Runtime Correction");
}

FLinearColor UAnimGraphNode_KawaiiPhysicsIKCorrection::GetNodeTitleColor() const {
	return FLinearColor(0.15f, 0.45f, 0.65f);
}

void UAnimGraphNode_KawaiiPhysicsIKCorrection::GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const {
	if (Registrar.IsOpenForRegistration(GetClass())) {
		Registrar.AddBlueprintAction(GetClass(), UBlueprintNodeSpawner::Create(GetClass()));
	}
}

#undef LOCTEXT_NAMESPACE
