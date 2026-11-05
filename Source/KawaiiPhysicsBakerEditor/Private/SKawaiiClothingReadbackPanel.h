#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "KawaiiClothingMotionReadback.h"

class IDetailsView;
class SKawaiiClothingReadbackViewport;
namespace KawaiiClothingReadback {
	void OpenPanel(UKawaiiClothingProxyMotion* Motion = nullptr, USkeletalMesh* BodyMesh = nullptr, UKawaiiPhysicsIKCorrectionProfile* Profile = nullptr);
	void OpenSelected();
}

class SKawaiiClothingReadbackPanel : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SKawaiiClothingReadbackPanel) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&, UKawaiiClothingProxyMotion* Motion, USkeletalMesh* BodyMesh, UKawaiiPhysicsIKCorrectionProfile* Profile);
	virtual ~SKawaiiClothingReadbackPanel();
	virtual void Tick(const FGeometry&, double CurrentTime, float DeltaTime) override;
private:
	void FillFromMotion();
	void InvalidateRead();
	void SettingsChanged(const FPropertyChangedEvent& Event);
	void ObjectModified(UObject* Object);
	TSharedRef<SWidget> MakePreviewControls();
	float TimelineDuration() const;
	float TimelinePosition() const;
	FText TimelineText() const;
	void SeekFrame(int32 Frame);
	void ScrubTime(float Time);
	FReply StepFrame(int32 Direction);
	FReply TogglePlayback();
	FReply Read();
	FReply Save();
	FReply Edit();
	FReply DuplicateForEditing();
	FReply UseSelected();
	TStrongObjectPtr<UKawaiiClothingReadbackSettings> Settings;
	TSharedPtr<FKawaiiClothingReadbackPreview> Preview;
	TSharedPtr<SKawaiiClothingReadbackViewport> Viewport;
	TSharedPtr<IDetailsView> Details;
	FDelegateHandle ModifiedHandle;
	FText Status;
	bool bReady = false, bPlaying = false, bPublishing = false, bLoopPreview = true;
	double PlayAccumulator = 0., LastValidation = 0.;
};
