#pragma once

#include "AnimGraphNode_Base.h"

#include "AnimNode_KawaiiPhysicsBakeSource.h"

#include "AnimGraphNode_KawaiiPhysicsBakeSource.generated.h"

/** Controlled animation input. Uses the source and time selected in the bake panel. */
UCLASS()
class UAnimGraphNode_KawaiiPhysicsBakeSource : public UAnimGraphNode_Base
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Bake")
		FAnimNode_KawaiiPhysicsBakeSource Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FString GetNodeCategory() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual void ValidateAnimNodeDuringCompilation(USkeleton* ForSkeleton, FCompilerResultsLog& MessageLog) override;
};
