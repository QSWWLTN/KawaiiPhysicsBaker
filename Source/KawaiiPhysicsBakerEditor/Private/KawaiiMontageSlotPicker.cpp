#include "KawaiiMontageSlotPicker.h"

#include "AnimGraphNode_Slot.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "DetailLayoutBuilder.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "PersonaTabs.h"
#include "PropertyHandle.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "KawaiiMontageSlotPicker"

namespace KawaiiMontageSlotPicker
{
	namespace
	{
		FText NoneLabel()
		{
			return LOCTEXT("NoneSlot", "None（不追踪蒙太奇）");
		}

		FText UnregisteredLabel(FName Slot)
		{
			return FText::Format(LOCTEXT("UnregisteredSlot", "{0}（骨架中未注册）"), FText::FromName(Slot));
		}
	}

	FText DescribeSlot(const USkeleton* Skeleton, FName Slot)
	{
		if (Slot.IsNone()) {
			return NoneLabel();
		}
		if (Skeleton) {
			for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups()) {
				if (Group.SlotNames.Contains(Slot)) {
					return FText::FromString(FString::Printf(TEXT("%s.%s"), *Group.GroupName.ToString(), *Slot.ToString()));
				}
			}
		}
		return UnregisteredLabel(Slot);
	}

	void BuildOptions(const USkeleton* Skeleton, const TSet<FName>& UsedSlots, FName Current, bool bHasCurrent, TArray<FOption>& OutOptions)
	{
		OutOptions.Reset();
		OutOptions.Add({NAME_None, NoneLabel()});
		bool bCurrentListed = !bHasCurrent || Current.IsNone();
		if (Skeleton) {
			for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups()) {
				for (const FName Slot : Group.SlotNames) {
					FText Label = FText::FromString(FString::Printf(TEXT("%s.%s"), *Group.GroupName.ToString(), *Slot.ToString()));
					if (UsedSlots.Contains(Slot)) {
						Label = FText::Format(LOCTEXT("UsedSlot", "{0}　（本蓝图已使用）"), Label);
					}
					OutOptions.Add({Slot, Label});
					bCurrentListed |= Slot == Current;
				}
			}
		}
		// Never silently drop a saved value; unlike the engine Slot node this does not register it on the skeleton.
		if (!bCurrentListed) {
			OutOptions.Add({Current, UnregisteredLabel(Current)});
		}
	}

	TSet<FName> GetUsedSlots(const UAnimBlueprint* Blueprint)
	{
		TSet<FName> Used;
		if (Blueprint) {
			TArray<UAnimGraphNode_Slot*> Nodes;
			FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);
			for (const UAnimGraphNode_Slot* Node : Nodes) {
				if (Node && !Node->Node.SlotName.IsNone()) {
					Used.Add(Node->Node.SlotName);
				}
			}
		}
		return Used;
	}
}

void SKawaiiMontageSlotPicker::Construct(const FArguments&, TSharedRef<IPropertyHandle> InSlotHandle, UAnimBlueprint* InBlueprint)
{
	SlotHandle = InSlotHandle;
	Blueprint = InBlueprint;
	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			SAssignNew(Combo, SComboBox<FOptionPtr>)
			.OptionsSource(&Options)
			.OnGenerateWidget(this, &SKawaiiMontageSlotPicker::MakeOptionWidget)
			.OnSelectionChanged(this, &SKawaiiMontageSlotPicker::OnSelectionChanged)
			// Refresh on every opening so Anim Slot Manager and graph edits appear without reselecting the node.
			.OnComboBoxOpening(this, &SKawaiiMontageSlotPicker::RefreshOptions)
			.ContentPadding(2.f)
			.ToolTipText(InSlotHandle->GetToolTipText())
			[
				SNew(STextBlock)
				.Text(this, &SKawaiiMontageSlotPicker::GetCurrentText)
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ToolTipText(LOCTEXT("OpenSlotManagerTooltip", "打开动画插槽管理器（Anim Slot Manager）编辑插槽与插槽组。"))
			.OnClicked(this, &SKawaiiMontageSlotPicker::OnOpenSlotManager)
			.ContentPadding(2.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("MeshPaint.FindInCB"))
			]
		]
	];
	RefreshOptions();
}

const USkeleton* SKawaiiMontageSlotPicker::GetSkeleton() const
{
	const UAnimBlueprint* Owner = Blueprint.Get();
	return Owner ? Owner->TargetSkeleton.Get() : nullptr;
}

void SKawaiiMontageSlotPicker::RefreshOptions()
{
	FName Current = NAME_None;
	const bool bHasCurrent = SlotHandle.IsValid() && SlotHandle->GetValue(Current) == FPropertyAccess::Success;
	TArray<KawaiiMontageSlotPicker::FOption> Built;
	KawaiiMontageSlotPicker::BuildOptions(GetSkeleton(), KawaiiMontageSlotPicker::GetUsedSlots(Blueprint.Get()), Current, bHasCurrent, Built);
	Options.Reset();
	FOptionPtr Selected;
	for (KawaiiMontageSlotPicker::FOption& Option : Built) {
		FOptionPtr& Added = Options.Add_GetRef(MakeShared<KawaiiMontageSlotPicker::FOption>(MoveTemp(Option)));
		if (bHasCurrent && Added->Slot == Current && !Selected.IsValid()) {
			Selected = Added;
		}
	}
	if (Combo.IsValid()) {
		Combo->RefreshOptions();
		// Direct selection only highlights the current value; OnSelectionChanged ignores it.
		if (Selected.IsValid()) {
			Combo->SetSelectedItem(Selected);
		} else {
			Combo->ClearSelection();
		}
	}
}

TSharedRef<SWidget> SKawaiiMontageSlotPicker::MakeOptionWidget(FOptionPtr Option)
{
	return SNew(STextBlock)
		.Text(Option.IsValid() ? Option->Label : FText::GetEmpty())
		.Font(IDetailLayoutBuilder::GetDetailFont());
}

void SKawaiiMontageSlotPicker::OnSelectionChanged(FOptionPtr Option, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct || !Option.IsValid() || !SlotHandle.IsValid()) {
		return;
	}
	// The handle owns the transaction, undo and node/blueprint change notifications.
	SlotHandle->SetValue(Option->Slot);
}

FText SKawaiiMontageSlotPicker::GetCurrentText() const
{
	FName Current = NAME_None;
	switch (SlotHandle.IsValid() ? SlotHandle->GetValue(Current) : FPropertyAccess::Fail) {
	case FPropertyAccess::Success:
		return KawaiiMontageSlotPicker::DescribeSlot(GetSkeleton(), Current);
	case FPropertyAccess::MultipleValues:
		return LOCTEXT("MultipleValues", "多个值");
	default:
		return FText::GetEmpty();
	}
}

FReply SKawaiiMontageSlotPicker::OnOpenSlotManager()
{
	UAnimBlueprint* Owner = Blueprint.Get();
	if (Owner && GEditor) {
		if (IAssetEditorInstance* Editor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Owner, false)) {
			if (const TSharedPtr<FTabManager> Tabs = Editor->GetAssociatedTabManager()) {
				Tabs->TryInvokeTab(FPersonaTabs::SkeletonSlotNamesID);
			}
		}
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
