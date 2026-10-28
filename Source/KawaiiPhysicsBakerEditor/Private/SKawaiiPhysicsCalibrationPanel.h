#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsCalibrationSettings.h"

class UAnimBlueprint;
class FKawaiiPhysicsCalibrationSession;
class IDetailsView;
struct FAssetData;

class SKawaiiPhysicsCalibrationPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKawaiiPhysicsCalibrationPanel) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&, UAnimBlueprint* Blueprint = nullptr);
	virtual ~SKawaiiPhysicsCalibrationPanel();

private:
	FReply SaveJob();
	FReply LoadJob();
	bool CanDropAnimations(TArrayView<FAssetData> Assets) const;
	void OnAnimationsDropped(const FDragDropEvent& Event, TArrayView<FAssetData> Assets);

private:
	TStrongObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings;
	TSharedPtr<FKawaiiPhysicsCalibrationSession> Session;
	FTSTicker::FDelegateHandle Ticker;
	FString Message;
	TSharedPtr<IDetailsView> Details;
};
