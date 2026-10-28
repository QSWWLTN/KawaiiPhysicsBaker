#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Containers/Ticker.h"

class UAnimBlueprint;
class FKawaiiPhysicsBakeSession;
class IDetailsView;
class SKawaiiPhysicsBakeViewport;
struct FAssetData;

class SKawaiiPhysicsBakePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKawaiiPhysicsBakePanel) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&, UAnimBlueprint* InBlueprint);
	virtual ~SKawaiiPhysicsBakePanel() override;

private:
	bool TickSession(float DeltaTime);
	bool CompileGraphs();
	FReply Play();
	FReply Reset();
	FReply Bake();
	FReply Cancel();
	FReply BrowseResults();
	void SelectAnimation(const FAssetData& Asset);
	void SettingsChanged(const FPropertyChangedEvent& Event);
	FText StatusText() const;
	TWeakObjectPtr<UAnimBlueprint> Blueprint;
	TSharedPtr<FKawaiiPhysicsBakeSession> Session;
	TSharedPtr<IDetailsView> Details;
	TSharedPtr<SKawaiiPhysicsBakeViewport> Preview;
	FTSTicker::FDelegateHandle TickerHandle;
	FText ErrorText;
};
