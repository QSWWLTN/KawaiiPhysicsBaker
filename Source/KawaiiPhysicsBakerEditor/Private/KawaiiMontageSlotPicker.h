#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class IPropertyHandle;
class UAnimBlueprint;
class USkeleton;

namespace KawaiiMontageSlotPicker {
	struct FOption
	{
		FName Slot;
		FText Label;
	};

	/** None first, then every skeleton slot as Group.Slot in Anim Slot Manager order. Slots used by Slot nodes in the
	 * blueprint are marked; a current value missing from the skeleton is kept as a final, marked entry. */
	void BuildOptions(const USkeleton* Skeleton, const TSet<FName>& UsedSlots, FName Current, bool bHasCurrent, TArray<FOption>& OutOptions);
	FText DescribeSlot(const USkeleton* Skeleton, FName Slot);
	TSet<FName> GetUsedSlots(const UAnimBlueprint* Blueprint);
}

/** Montage Slot editor matching the engine Slot node: a skeleton slot dropdown plus the Anim Slot Manager button. */
class SKawaiiMontageSlotPicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKawaiiMontageSlotPicker) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedRef<IPropertyHandle> InSlotHandle, UAnimBlueprint* InBlueprint);

private:
	using FOptionPtr = TSharedPtr<KawaiiMontageSlotPicker::FOption>;

	void RefreshOptions();
	TSharedRef<SWidget> MakeOptionWidget(FOptionPtr Option);
	void OnSelectionChanged(FOptionPtr Option, ESelectInfo::Type SelectInfo);
	FText GetCurrentText() const;
	FReply OnOpenSlotManager();
	const USkeleton* GetSkeleton() const;

	TSharedPtr<IPropertyHandle> SlotHandle;
	TWeakObjectPtr<UAnimBlueprint> Blueprint;
	TArray<FOptionPtr> Options;
	TSharedPtr<SComboBox<FOptionPtr>> Combo;
};
