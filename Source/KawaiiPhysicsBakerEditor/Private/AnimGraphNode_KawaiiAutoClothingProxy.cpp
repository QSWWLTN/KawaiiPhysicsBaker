#include "AnimGraphNode_KawaiiAutoClothingProxy.h"

#include "Animation/AnimBlueprint.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"

#include "KawaiiClothingProxyAnimBlueprintExtension.h"
#include "KawaiiMontageSlotPicker.h"

#define LOCTEXT_NAMESPACE "KawaiiAutoClothingProxy"

void UAnimGraphNode_KawaiiAutoClothingProxy::Serialize(FArchive& Ar) {
	Super::Serialize(Ar);
	if (Ar.IsLoading()) {
		// Migrate before compile-on-load, preserving pin IDs and all existing links.
		// Only the input changed; the generated output must continue to be Pose.
		for (UEdGraphPin* Pin : Pins) {
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == TEXT("Pose")) {
				Pin->PinName = GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, SourcePose);
			}
		}
		for (FOptionalPinFromProperty& OptionalPin : ShowPinForProperties) {
			if (OptionalPin.PropertyName == TEXT("Pose")) {
				OptionalPin.PropertyName = GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, SourcePose);
			}
		}
	}
}

UK2Node::ERedirectType UAnimGraphNode_KawaiiAutoClothingProxy::DoPinsMatchForReconstruction(const UEdGraphPin* NewPin, int32 NewPinIndex, const UEdGraphPin* OldPin, int32 OldPinIndex) const {
	if (NewPin->Direction == EGPD_Input && OldPin->Direction == EGPD_Input
		&& NewPin->PinName == GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, SourcePose) && OldPin->PinName == TEXT("Pose"))
	{
		return ERedirectType_Name;
	}
	return Super::DoPinsMatchForReconstruction(NewPin, NewPinIndex, OldPin, OldPinIndex);
}

FText UAnimGraphNode_KawaiiAutoClothingProxy::GetNodeTitle(ENodeTitleType::Type) const {
	return LOCTEXT("Title", "Automatic Clothing Proxy Correction");
}

FText UAnimGraphNode_KawaiiAutoClothingProxy::GetTooltipText() const {
	return LOCTEXT(
		"Tooltip",
		"Automatically follows the current input branch's sequence times and full-body blend weights, looks up the equipped garment's baked proxies, and corrects Profile bone chains. Set Proxy Motion Set OR Proxy Motions; either pin can change at runtime. Configure Montage Slot once for a single full-body montage slot. Place before shared Save Cached Pose. Requires synchronized baked clothing playback; layered blends, additive poses, mirror/inertialization and procedural clothing changes are not reproduced. Missing mappings disable proxy correction and appear in animation debug data."
	);
}

FString UAnimGraphNode_KawaiiAutoClothingProxy::GetNodeCategory() const {
	return TEXT("Kawaii Physics|Runtime Correction");
}

FLinearColor UAnimGraphNode_KawaiiAutoClothingProxy::GetNodeTitleColor() const {
	return FLinearColor(.15f, .5f, .65f);
}

void UAnimGraphNode_KawaiiAutoClothingProxy::GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const {
	if (Registrar.IsOpenForRegistration(GetClass())) {
		Registrar.AddBlueprintAction(GetClass(), UBlueprintNodeSpawner::Create(GetClass()));
	}
}

void UAnimGraphNode_KawaiiAutoClothingProxy::GetRequiredExtensions(TArray<TSubclassOf<UAnimBlueprintExtension>>& OutExtensions) const {
	Super::GetRequiredExtensions(OutExtensions);
	OutExtensions.AddUnique(UKawaiiClothingProxyAnimBlueprintExtension::StaticClass());
}

void UAnimGraphNode_KawaiiAutoClothingProxy::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) {
	Super::CustomizeDetails(DetailBuilder);
	const FName SlotProperty = GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, MontageSlot);
	TSharedRef<IPropertyHandle> SlotHandle = DetailBuilder.GetProperty(
		*FString::Printf(TEXT("%s.%s"), *GET_MEMBER_NAME_CHECKED(UAnimGraphNode_KawaiiAutoClothingProxy, Node).ToString(), *SlotProperty.ToString()),
		GetClass()
	);
	if (!SlotHandle->IsValidHandle()) {
		return;
	}
	// Slot names belong to the target skeleton. Mixed or missing skeletons keep the default text field.
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	UAnimBlueprint* Blueprint = nullptr;
	for (const TWeakObjectPtr<UObject>& Object : Objects) {
		const UAnimGraphNode_KawaiiAutoClothingProxy* ProxyNode = Cast<UAnimGraphNode_KawaiiAutoClothingProxy>(Object.Get());
		UAnimBlueprint* NodeBlueprint = ProxyNode && ProxyNode->HasValidBlueprint() ? ProxyNode->GetAnimBlueprint() : nullptr;
		if (!NodeBlueprint || !NodeBlueprint->TargetSkeleton || (Blueprint && Blueprint->TargetSkeleton != NodeBlueprint->TargetSkeleton)) {
			return;
		}
		Blueprint = Blueprint ? Blueprint : NodeBlueprint;
	}
	if (!Blueprint) {
		return;
	}
	// Touch earlier categories first so customizing this row does not move Action Tracking to the top.
	DetailBuilder.EditCategory(TEXT("Clothing Proxy"));
	DetailBuilder.EditCategory(TEXT("Correction"));
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Action Tracking"));
	TSharedRef<SKawaiiMontageSlotPicker> Picker = SNew(SKawaiiMontageSlotPicker, SlotHandle, Blueprint);
	const int32 PinIndex =
		ShowPinForProperties.IndexOfByPredicate([SlotProperty](const FOptionalPinFromProperty& Pin) { return Pin.PropertyName == SlotProperty; });
	if (PinIndex != INDEX_NONE) {
		const TSharedRef<IPropertyHandle> ShowPinHandle = DetailBuilder.GetProperty(*FString::Printf(TEXT("ShowPinForProperties[%d].bShowPin"), PinIndex));
		if (ShowPinHandle->IsValidHandle()) {
			ShowPinHandle->MarkHiddenByCustomization();
			// Same rule as the engine anim node details: an exposed pin owns the value.
			Picker->SetVisibility(TAttribute<EVisibility>::CreateLambda([ShowPinHandle]() {
				bool bShowPin = false;
				return ShowPinHandle->GetValue(bShowPin) == FPropertyAccess::Success && bShowPin ? EVisibility::Hidden : EVisibility::Visible;
			}));
		}
	}
	Category.AddProperty(SlotHandle)
		.CustomWidget()
		.NameContent()[SlotHandle->CreatePropertyNameWidget()]
		.ValueContent()
		.MinDesiredWidth(250.f)
		.MaxDesiredWidth(400.f)[Picker];
}

#undef LOCTEXT_NAMESPACE
