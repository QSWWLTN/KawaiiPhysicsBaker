#include "SKawaiiPhysicsBakePanel.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeSession.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "EditorViewportClient.h"
#include "IDetailsView.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PropertyEditorModule.h"
#include "SEditorViewport.h"
#include "ScopedTransaction.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakePanel"

class SKawaiiPhysicsBakeViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SKawaiiPhysicsBakeViewport) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&, TSharedRef<FKawaiiPhysicsBakeSession> InSession) {
		Session = InSession;
		SEditorViewport::Construct(SEditorViewport::FArguments());
	}
	virtual ~SKawaiiPhysicsBakeViewport() override {
		if (ViewportClient) {
			ViewportClient->Viewport = nullptr;
		}
	}
	void FrameMesh() {
		if (USkeletalMeshComponent* Mesh = Session->GetMainComponent()) {
			ViewportClient->FocusViewportOnBox(Mesh->Bounds.GetBox(), true);
			PreviousRoot = RootLocation(Mesh);
			bFollowRoot = true;
		}
	}
	void FollowRoot() {
		if (USkeletalMeshComponent* Mesh = Session->GetMainComponent(); Mesh && bFollowRoot) {
			const FVector Root = RootLocation(Mesh);
			const FVector Delta = Root - PreviousRoot;
			ViewportClient->SetViewLocation(ViewportClient->GetViewLocation() + Delta);
			ViewportClient->SetLookAtLocation(ViewportClient->GetLookAtLocation() + Delta);
			PreviousRoot = Root;
		}
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override {
		ViewportClient = MakeShared<FEditorViewportClient>(nullptr, Session->GetPreviewScene(), SharedThis(this));
		ViewportClient->SetViewMode(VMI_Lit);
		ViewportClient->SetViewportType(LVT_Perspective);
		ViewportClient->SetViewLocation(FVector(250, 250, 140));
		ViewportClient->SetViewRotation(FRotator(-12, -135, 0));
		ViewportClient->SetRealtime(true);
		ViewportClient->EngineShowFlags.SetGrid(false);
		ViewportClient->EngineShowFlags.SetSelectionOutline(false);
		return ViewportClient.ToSharedRef();
	}
	virtual TSharedPtr<SWidget> MakeViewportToolbar() override { return nullptr; }

private:
	static FVector RootLocation(const USkeletalMeshComponent* Mesh) {
		const TArray<FTransform>& Pose = Mesh->GetComponentSpaceTransforms();
		return Pose.IsEmpty() ? Mesh->GetComponentLocation() : Mesh->GetComponentTransform().TransformPosition(Pose[0].GetTranslation());
	}
	FVector PreviousRoot = FVector::ZeroVector;
	bool bFollowRoot = false;
	TSharedPtr<FKawaiiPhysicsBakeSession> Session;
	TSharedPtr<FEditorViewportClient> ViewportClient;
};

void SKawaiiPhysicsBakePanel::Construct(const FArguments&, UAnimBlueprint* InBlueprint) {
	Blueprint = InBlueprint;
	Session = MakeShared<FKawaiiPhysicsBakeSession>();
	FString Error;
	if (!Session->Initialize(InBlueprint, Error)) {
		ErrorText = FText::FromString(Error);
	}
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = PropertyEditor.CreateDetailView(DetailsArgs);
	Details->SetObject(KawaiiPhysicsBake::GetSettings(InBlueprint));
	Details->OnFinishedChangingProperties().AddSP(this, &SKawaiiPhysicsBakePanel::SettingsChanged);

	FAssetPickerConfig Picker;
	Picker.Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Picker.InitialAssetViewType = EAssetViewType::List;
	Picker.SelectionMode = ESelectionMode::Single;
	Picker.OnAssetDoubleClicked = FOnAssetDoubleClicked::CreateSP(this, &SKawaiiPhysicsBakePanel::SelectAnimation);
	Picker.OnShouldFilterAsset = FOnShouldFilterAsset::CreateLambda([WeakBP = Blueprint](const FAssetData& Asset) {
		if (!WeakBP.IsValid() || !WeakBP->TargetSkeleton) {
			return false;
		}
		FString SkeletonTag;
		return Asset.GetTagValue(TEXT("Skeleton"), SkeletonTag) && SkeletonTag != FAssetData(WeakBP->TargetSkeleton).GetExportTextName();
	});
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	ChildSlot
		[SNew(SVerticalBox)
		 + SVerticalBox::Slot().AutoHeight().Padding(
			 8)[SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("Help",
								  "Edit the native AnimGraph: Bake Source Pose → KawaiiPhysics → Output Pose (use the usual local/component space conversion nodes). Each clothing part has its own "
								  "mesh and bake graph. Double-click an animation below to preview the complete character."))]
		 + SVerticalBox::Slot().AutoHeight().Padding(
			 8, 0)[SNew(SHorizontalBox)
				   + SHorizontalBox::Slot().AutoWidth().Padding(
					   0, 0, 6, 0)[SNew(SButton).Text(LOCTEXT("Play", "Compile & Play")).IsEnabled_Lambda([this] { return !Session->IsBaking(); }).OnClicked(this, &SKawaiiPhysicsBakePanel::Play)]
				   + SHorizontalBox::Slot().AutoWidth().Padding(
					   0, 0, 6, 0)[SNew(SButton).Text(LOCTEXT("Reset", "Reset")).IsEnabled_Lambda([this] { return !Session->IsBaking(); }).OnClicked(this, &SKawaiiPhysicsBakePanel::Reset)]
				   + SHorizontalBox::Slot().AutoWidth().Padding(
					   0, 0, 6, 0)[SNew(SButton).Text(LOCTEXT("Bake", "Bake New Animations")).IsEnabled_Lambda([this] { return !Session->IsBaking(); }).OnClicked(this, &SKawaiiPhysicsBakePanel::Bake)]
				   + SHorizontalBox::Slot().AutoWidth().Padding(
					   0, 0, 6, 0)[SNew(SButton).Text(LOCTEXT("Cancel", "Stop / Cancel")).IsEnabled_Lambda([this] { return Session->IsRunning(); }).OnClicked(this, &SKawaiiPhysicsBakePanel::Cancel)]
				   + SHorizontalBox::Slot().AutoWidth()[SNew(SButton)
															.Text(LOCTEXT("Results", "Find Results"))
															.IsEnabled_Lambda([this] { return !Session->GetOutputAssets().IsEmpty(); })
															.OnClicked(this, &SKawaiiPhysicsBakePanel::BrowseResults)]]
		 + SVerticalBox::Slot().AutoHeight().Padding(8)[SNew(STextBlock).AutoWrapText(true).Text(this, &SKawaiiPhysicsBakePanel::StatusText)]
		 + SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 6)[SNew(SProgressBar).Percent_Lambda([this]() -> TOptional<float> { return Session->GetProgress(); })]
		 + SVerticalBox::Slot().FillHeight(
			 1)[SNew(SSplitter) + SSplitter::Slot().Value(0.56f)[SAssignNew(Preview, SKawaiiPhysicsBakeViewport, Session.ToSharedRef())]
				+ SSplitter::Slot().Value(0.44f)[SNew(SSplitter).Orientation(Orient_Vertical) + SSplitter::Slot().Value(0.68f)[SNew(SBox).IsEnabled_Lambda([this] {
					  return !Session->IsBaking();
				  })[Details.ToSharedRef()]] + SSplitter::Slot().Value(0.32f)[SNew(SBox).IsEnabled_Lambda([this] { return !Session->IsBaking(); })[ContentBrowser.Get().CreateAssetPicker(Picker)]]]]];

	// An editor ticker keeps a bake progressing while its graph document is selected.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &SKawaiiPhysicsBakePanel::TickSession));
	Preview->FrameMesh();
}

SKawaiiPhysicsBakePanel::~SKawaiiPhysicsBakePanel() {
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	if (Session) {
		Session->Cancel();
	}
}
bool SKawaiiPhysicsBakePanel::TickSession(float) {
	if (!Blueprint.IsValid()) {
		Session->Cancel();
		return false;
	}
	Session->Tick();
	Preview->FollowRoot();
	return true;
}
bool SKawaiiPhysicsBakePanel::CompileGraphs() {
	Session->Cancel();
	ErrorText = FText::GetEmpty();
	if (!Blueprint.IsValid() || !KawaiiPhysicsBake::GetSettings(Blueprint.Get())) {
		return false;
	}
	TSet<UAnimBlueprint*> Graphs;
	Graphs.Add(Blueprint.Get());
	for (const FKawaiiPhysicsBakePart& Part : KawaiiPhysicsBake::GetSettings(Blueprint.Get())->Parts) {
		if (Part.Graph) {
			Graphs.Add(Part.Graph);
		}
	}
	for (UAnimBlueprint* Graph : Graphs) {
		FKismetEditorUtilities::CompileBlueprint(Graph);
		if (Graph->Status == BS_Error || !Graph->GeneratedClass) {
			ErrorText = FText::Format(LOCTEXT("CompileError", "Fix compiler errors in {0} before baking."), FText::FromString(Graph->GetName()));
			return false;
		}
	}
	return true;
}
FReply SKawaiiPhysicsBakePanel::Play() {
	FString Error;
	if (CompileGraphs()) {
		if (!Session->StartPreview(Error)) {
			ErrorText = FText::FromString(Error);
		} else {
			Preview->FrameMesh();
		}
	}
	return FReply::Handled();
}
FReply SKawaiiPhysicsBakePanel::Reset() {
	FString Error;
	ErrorText = FText::GetEmpty();
	if (!Session->ResetPreview(Error)) {
		ErrorText = FText::FromString(Error);
	} else {
		Preview->FrameMesh();
	}
	return FReply::Handled();
}
FReply SKawaiiPhysicsBakePanel::Bake() {
	FString Error;
	if (CompileGraphs()) {
		if (!Session->StartBake(Error)) {
			ErrorText = FText::FromString(Error);
		} else {
			Preview->FrameMesh();
		}
	}
	return FReply::Handled();
}
FReply SKawaiiPhysicsBakePanel::Cancel() {
	Session->Cancel();
	ErrorText = FText::GetEmpty();
	return FReply::Handled();
}
FReply SKawaiiPhysicsBakePanel::BrowseResults() {
	TArray<UObject*> Assets;
	for (const TWeakObjectPtr<UAnimSequence>& Asset : Session->GetOutputAssets()) {
		if (Asset.IsValid()) {
			Assets.Add(Asset.Get());
		}
	}
	FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
	return FReply::Handled();
}
void SKawaiiPhysicsBakePanel::SelectAnimation(const FAssetData& Asset) {
	if (!Blueprint.IsValid() || Session->IsBaking()) {
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("Select", "Select Bake Animation"));
	KawaiiPhysicsBake::GetSettings(Blueprint.Get())->Modify();
	KawaiiPhysicsBake::GetSettings(Blueprint.Get())->SourceAnimation = Cast<UAnimSequence>(Asset.GetAsset());
	Blueprint->MarkPackageDirty();
	Details->ForceRefresh();
	Play();
}
void SKawaiiPhysicsBakePanel::SettingsChanged(const FPropertyChangedEvent&) {
	Session->Cancel();
	ErrorText = FText::GetEmpty();
	if (Blueprint.IsValid()) {
		Blueprint->MarkPackageDirty();
	}
}
FText SKawaiiPhysicsBakePanel::StatusText() const {
	return ErrorText.IsEmpty() ? Session->GetStatus() : ErrorText;
}
#undef LOCTEXT_NAMESPACE
