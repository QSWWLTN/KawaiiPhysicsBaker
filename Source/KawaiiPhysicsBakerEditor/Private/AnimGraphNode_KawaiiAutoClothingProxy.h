#pragma once

#include "AnimGraphNode_Base.h"

#include "AnimNode_KawaiiAutoClothingProxy.h"

#include "AnimGraphNode_KawaiiAutoClothingProxy.generated.h"

UCLASS()
class UAnimGraphNode_KawaiiAutoClothingProxy : public UAnimGraphNode_Base
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Clothing Proxy")
		FAnimNode_KawaiiAutoClothingProxy Node;

	virtual void Serialize(FArchive& Ar) override;
	virtual ERedirectType DoPinsMatchForReconstruction(const UEdGraphPin* NewPin,
													   int32 NewPinIndex,
													   const UEdGraphPin* OldPin,
													   int32 OldPinIndex) const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FString GetNodeCategory() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
	virtual void GetRequiredExtensions(TArray<TSubclassOf<UAnimBlueprintExtension>>& OutExtensions) const override;
	/** Replaces the Montage Slot text field with the same skeleton slot dropdown the engine Slot node uses. */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
