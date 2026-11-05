#pragma once

#include "AnimGraphNode_Base.h"

#include "AnimNode_KawaiiPhysicsIKCorrection.h"

#include "AnimGraphNode_KawaiiPhysicsIKCorrection.generated.h"

UCLASS()
class UAnimGraphNode_KawaiiPhysicsIKCorrection : public UAnimGraphNode_Base
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Correction")
		FAnimNode_KawaiiPhysicsIKCorrection Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FString GetNodeCategory() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
};
