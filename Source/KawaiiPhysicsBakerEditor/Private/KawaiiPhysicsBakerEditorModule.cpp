#include "Modules/ModuleManager.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeFactory.h"
#include "SKawaiiPhysicsBakePanel.h"
#include "SKawaiiPhysicsCalibrationPanel.h"
#include "Framework/Docking/TabManager.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IAnimationBlueprintEditorModule.h"
#include "IAssetTools.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakerEditor"

namespace KawaiiPhysicsBakerEditor
{
	const FName CalibrationTabId(TEXT("KawaiiPhysicsBaker.Calibration"));
	const FName BakeTabId(TEXT("KawaiiPhysicsBaker.Bake"));

	void OpenBakePanel(TWeakPtr<IAnimationBlueprintEditor> WeakEditor) {
		TSharedPtr<IAnimationBlueprintEditor> Editor = WeakEditor.Pin();
		if (!Editor) {
			return;
		}
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Editor->GetBlueprintObj());
		if (!KawaiiPhysicsBake::IsBakeBlueprint(Blueprint)) {
			return;
		}
		KawaiiPhysicsBake::FindOrAddSettings(Blueprint);
		const TSharedPtr<FTabManager> Manager = Editor->GetTabManager();
		if (!Manager.IsValid()) {
			return;
		}
		if (!Manager->HasTabSpawner(BakeTabId)) {
			Manager
				->RegisterTabSpawner(BakeTabId, FOnSpawnTab::CreateLambda([WeakBP = TWeakObjectPtr<UAnimBlueprint>(Blueprint)](const FSpawnTabArgs&) {
										 TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::PanelTab).Label(LOCTEXT("Tab", "Kawaii 物理烘焙"));
										 if (WeakBP.IsValid()) {
											 Tab->SetContent(SNew(SKawaiiPhysicsBakePanel, WeakBP.Get()));
										 }
										 return Tab;
									 }))
				.SetDisplayName(LOCTEXT("Tab", "Kawaii 物理烘焙"));
		}
		Manager->TryInvokeTab(BakeTabId);
	}
}

class FKawaiiPhysicsBakerEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {
		if (IsRunningCommandlet()) {
			return;
		}
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			KawaiiPhysicsBakerEditor::CalibrationTabId,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
				return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SKawaiiPhysicsCalibrationPanel)];
			})
		)
		.SetDisplayName(LOCTEXT("CalibrationTab", "Kawaii 物理校准"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
		auto& EditorModule = FModuleManager::LoadModuleChecked<IAnimationBlueprintEditorModule>("AnimationBlueprintEditor");
		IAnimationBlueprintEditorModule::FAnimationBlueprintEditorToolbarExtender Extender;
		Extender.BindRaw(this, &FKawaiiPhysicsBakerEditorModule::ExtendToolbar);
		ToolbarDelegateHandle = Extender.GetHandle();
		EditorModule.GetAllAnimationBlueprintEditorToolbarExtenders().Add(Extender);
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FKawaiiPhysicsBakerEditorModule::RegisterMenus));
	}
	virtual void ShutdownModule() override {
		if (IsRunningCommandlet()) {
			return;
		}
		UToolMenus::UnRegisterStartupCallback(this);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(KawaiiPhysicsBakerEditor::CalibrationTabId);
		UToolMenus::UnregisterOwner(this);
		if (auto* Module = FModuleManager::GetModulePtr<IAnimationBlueprintEditorModule>("AnimationBlueprintEditor")) {
			Module->GetAllAnimationBlueprintEditorToolbarExtenders().RemoveAll([this](const auto& Delegate) { return Delegate.GetHandle() == ToolbarDelegateHandle; });
		}
		for (const auto& Pair : Editors) {
			if (TSharedPtr<IAnimationBlueprintEditor> Editor = Pair.Value.Pin()) {
				if (TSharedPtr<FTabManager> Manager = Editor->GetTabManager()) {
					if (TSharedPtr<SDockTab> Tab = Manager->FindExistingLiveTab(KawaiiPhysicsBakerEditor::BakeTabId)) {
						Tab->RequestCloseTab();
					}
					Manager->UnregisterTabSpawner(KawaiiPhysicsBakerEditor::BakeTabId);
				}
			}
		}
		Editors.Reset();
	}
	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TSharedRef<FExtender> ExtendToolbar(const TSharedRef<FUICommandList>, TSharedRef<IAnimationBlueprintEditor> Editor) {
		TSharedRef<FExtender> Extender = MakeShared<FExtender>();
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Editor->GetBlueprintObj());
		Extender->AddToolBarExtension(
			"Asset",
			EExtensionHook::After,
			nullptr,
			FToolBarExtensionDelegate::CreateLambda([Weak = TWeakObjectPtr<UAnimBlueprint>(Blueprint)](FToolBarBuilder& Builder) {
				Builder.AddToolBarButton(
					FUIAction(FExecuteAction::CreateLambda([Weak] {
						auto Tab = FGlobalTabmanager::Get()->TryInvokeTab(KawaiiPhysicsBakerEditor::CalibrationTabId);
						if (Tab && Weak.IsValid()) {
							Tab->SetContent(SNew(SKawaiiPhysicsCalibrationPanel, Weak.Get()));
						}
					})),
					NAME_None,
					LOCTEXT("Calibrate", "物理校准"),
					LOCTEXT("CalibrateHelp", "通过离线 GPU 筛选和原始 CPU 模拟验证，寻找适用于多个动作的可复用物理配置。"),
					FSlateIcon()
				);
			})
		);
		if (KawaiiPhysicsBake::IsBakeBlueprint(Blueprint)) {
			// Store the editor interface supplied by the native AnimBP module rather
			// than casting an arbitrary IAssetEditorInstance returned by the subsystem.
			for (auto Iterator = Editors.CreateIterator(); Iterator; ++Iterator) {
				if (!Iterator.Key().IsValid() || !Iterator.Value().IsValid()) {
					Iterator.RemoveCurrent();
				}
			}
			Editors.Add(Blueprint, Editor);
			TWeakPtr<IAnimationBlueprintEditor> WeakEditor = Editor;
			Extender->AddToolBarExtension("Asset", EExtensionHook::After, nullptr, FToolBarExtensionDelegate::CreateLambda([WeakEditor](FToolBarBuilder& Builder) {
											  Builder.AddToolBarButton(FUIAction(FExecuteAction::CreateLambda([WeakEditor] { KawaiiPhysicsBakerEditor::OpenBakePanel(WeakEditor); })),
																	   NAME_None,
																	   LOCTEXT("Toolbar", "物理烘焙"),
																	   LOCTEXT("ToolbarHelp", "打开角色同步预览和物理烘焙设置。"),
																	   FSlateIcon(FAppStyle::GetAppStyleSetName(), "Persona.StartRecordAnimation"));
										  }));
		}
		return Extender;
	}
	void RegisterMenus() {
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("KawaiiPhysicsBaker", LOCTEXT("Section", "Animation"));
		Section.AddMenuEntry(
			"KawaiiCalibration",
			LOCTEXT("CalibrationMenu", "Kawaii 物理校准"),
			LOCTEXT("CalibrationMenuHelp", "使用多个动作进行离线 GPU 校准，运行时仅由 CPU 执行物理模拟。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([] {
				FGlobalTabmanager::Get()->TryInvokeTab(KawaiiPhysicsBakerEditor::CalibrationTabId);
			}))
		);
		Section.AddMenuEntry("CreateKawaiiBakeBlueprint",
							 LOCTEXT("Create", "创建 Kawaii 物理烘焙蓝图"),
							 LOCTEXT("CreateHelp", "创建支持多部件同步预览和物理烘焙的动画蓝图。"),
							 FSlateIcon(),
							 FUIAction(FExecuteAction::CreateRaw(this, &FKawaiiPhysicsBakerEditorModule::CreateBakeBlueprint)));
	}
	void CreateBakeBlueprint() {
		TStrongObjectPtr<UKawaiiPhysicsBakeFactory> Factory(NewObject<UKawaiiPhysicsBakeFactory>());
		if (!Factory->ConfigureProperties()) {
			return;
		}
		UAnimBlueprint* Asset = Cast<UAnimBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools")
														 .Get()
														 .CreateAssetWithDialog(Factory->GetDefaultNewAssetName(), TEXT("/Game/KawaiiBakes"), UAnimBlueprint::StaticClass(), Factory.Get()));
		if (!Asset) {
			return;
		}
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
		if (const TWeakPtr<IAnimationBlueprintEditor>* Editor = Editors.Find(Asset)) {
			KawaiiPhysicsBakerEditor::OpenBakePanel(*Editor);
		}
	}
	FDelegateHandle ToolbarDelegateHandle;
	TMap<TWeakObjectPtr<UAnimBlueprint>, TWeakPtr<IAnimationBlueprintEditor>> Editors;
};

IMPLEMENT_MODULE(FKawaiiPhysicsBakerEditorModule, KawaiiPhysicsBakerEditor)
#undef LOCTEXT_NAMESPACE
