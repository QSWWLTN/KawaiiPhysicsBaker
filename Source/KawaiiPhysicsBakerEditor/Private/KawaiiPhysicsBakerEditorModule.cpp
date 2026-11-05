#include "Modules/ModuleManager.h"
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

#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeFactory.h"
#include "SKawaiiPhysicsBakePanel.h"
#include "SKawaiiPhysicsCalibrationPanel.h"
#include "SKawaiiClothingProxyPanel.h"
#include "SKawaiiClothingReadbackPanel.h"
#include "KawaiiClothingProxyPlaybackUpgrade.h"
#include "KawaiiPhysicsIKProfileGenerator.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakerEditor"

namespace KawaiiPhysicsBakerEditor {
	const FName CalibrationTabId(TEXT("KawaiiPhysicsBaker.Calibration"));
	const FName BakeTabId(TEXT("KawaiiPhysicsBaker.Bake"));
	const FName ClothingProxyTabId(TEXT("KawaiiPhysicsBaker.ClothingProxy"));

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
			})).SetDisplayName(LOCTEXT("Tab", "Kawaii 物理烘焙"));
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
		FGlobalTabmanager::Get()
			->RegisterNomadTabSpawner(KawaiiPhysicsBakerEditor::CalibrationTabId, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
			return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SKawaiiPhysicsCalibrationPanel)];
		}))
			.SetDisplayName(LOCTEXT("CalibrationTab", "Kawaii 物理校准"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);
		FGlobalTabmanager::Get()
			->RegisterNomadTabSpawner(KawaiiPhysicsBakerEditor::ClothingProxyTabId, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
			return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SKawaiiClothingProxyPanel)];
		}))
			.SetDisplayName(LOCTEXT("ClothingProxyTab", "Kawaii 衣物代理"))
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
		if (TSharedPtr<SDockTab> ProxyTab = FGlobalTabmanager::Get()->FindExistingLiveTab(KawaiiPhysicsBakerEditor::ClothingProxyTabId)) {
			ProxyTab->RequestCloseTab();
		}
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(KawaiiPhysicsBakerEditor::ClothingProxyTabId);
		UToolMenus::UnregisterOwner(this);
		if (auto* Module = FModuleManager::GetModulePtr<IAnimationBlueprintEditorModule>("AnimationBlueprintEditor")) {
			Module->GetAllAnimationBlueprintEditorToolbarExtenders().RemoveAll([this](const auto& Delegate) {
				return Delegate.GetHandle() == ToolbarDelegateHandle;
			});
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
			Extender
				->AddToolBarExtension("Asset", EExtensionHook::After, nullptr, FToolBarExtensionDelegate::CreateLambda([WeakEditor](FToolBarBuilder& Builder) {
				Builder.AddToolBarButton(
					FUIAction(FExecuteAction::CreateLambda([WeakEditor] { KawaiiPhysicsBakerEditor::OpenBakePanel(WeakEditor); })),
					NAME_None,
					LOCTEXT("Toolbar", "物理烘焙"),
					LOCTEXT("ToolbarHelp", "打开角色同步预览和物理烘焙设置。"),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Persona.StartRecordAnimation")
				);
			}));
		}
		return Extender;
	}

	void RegisterMenus() {
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("KawaiiPhysicsBaker", LOCTEXT("Section", "Animation"));
		const FUIAction ReadbackAction(FExecuteAction::CreateStatic(&KawaiiClothingReadback::OpenSelected));
		Section.AddMenuEntry("KawaiiClothingReadback", LOCTEXT("Readback", "Kawaii 衣物动画回读"),
			LOCTEXT("ReadbackHelp", "手动修整衣物动画后，重新读取并预览代理轨迹，再更新原动作关联。"), FSlateIcon(), ReadbackAction);
		for (const FName Context : { FName(TEXT("ContentBrowser.AssetContextMenu.AnimSequence")), FName(TEXT("ContentBrowser.AssetContextMenu.KawaiiClothingProxyMotion")) }) {
			UToolMenus::Get()->ExtendMenu(Context)->FindOrAddSection("GetAssetActions").AddMenuEntry("KawaiiClothingReadback",
				LOCTEXT("Readback", "Kawaii 衣物动画回读"), LOCTEXT("ReadbackHelp", "手动修整衣物动画后，重新读取并预览代理轨迹，再更新原动作关联。"), FSlateIcon(), ReadbackAction);
		}
		Section.AddMenuEntry(
			"KawaiiClothingProxy",
			LOCTEXT("ClothingProxyMenu", "Kawaii 衣物代理"),
			LOCTEXT("ClothingProxyMenuHelp", "创建、编辑和预览衣物球体／胶囊代理，供烘焙最小骨骼运动轨道使用。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(KawaiiPhysicsBakerEditor::ClothingProxyTabId); }))
		);
		Section.AddMenuEntry(
			"UpgradeKawaiiClothingProxyPlayback",
			LOCTEXT("UpgradeProxyPlayback", "升级所选衣物代理动作集合"),
			LOCTEXT(
				"UpgradeProxyPlaybackHelp",
				"为内容浏览器中选中的衣物代理动作集合补全可在运行时读取的身体烘焙动画关联。按生成组、源动作和 Main 部件元数据匹配，并保存修改的代理运动。"
			),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateStatic(&KawaiiClothingProxyUpgrade::UpgradeSelectedSets),
				FCanExecuteAction::CreateStatic(&KawaiiClothingProxyUpgrade::HasSelectedSets)
			)
		);
		const FUIAction GenerateProfileAction(
			FExecuteAction::CreateStatic(&KawaiiPhysicsIKProfileGenerator::GenerateForSelectedAnimBlueprints),
			FCanExecuteAction::CreateStatic(&KawaiiPhysicsIKProfileGenerator::HasSelectedAnimBlueprints)
		);
		const FText GenerateProfileLabel = LOCTEXT("GenerateIKProfile", "生成 Kawaii IK 修正配置");
		const FText GenerateProfileHelp = LOCTEXT(
			"GenerateIKProfileHelp",
			"根据所选动画蓝图中的 KawaiiPhysics 节点自动推断骨链（分叉处拆分、遵循衣物代理的忽略目标骨链），导入外侧球体/胶囊限制作为身体碰撞体，生成或更新 DA_<蓝图名>_IKCorrection，并填入 Profile 为空的自动衣物代理修正节点。"
		);
		Section.AddMenuEntry("GenerateKawaiiIKCorrectionProfile", GenerateProfileLabel, GenerateProfileHelp, FSlateIcon(), GenerateProfileAction);
		if (UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.AnimBlueprint")) {
			AssetMenu->FindOrAddSection("GetAssetActions")
				.AddMenuEntry("GenerateKawaiiIKCorrectionProfile", GenerateProfileLabel, GenerateProfileHelp, FSlateIcon(), GenerateProfileAction);
		}
		Section.AddMenuEntry(
			"KawaiiCalibration",
			LOCTEXT("CalibrationMenu", "Kawaii 物理校准"),
			LOCTEXT("CalibrationMenuHelp", "使用多个动作进行离线 GPU 校准，运行时仅由 CPU 执行物理模拟。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(KawaiiPhysicsBakerEditor::CalibrationTabId); }))
		);
		Section.AddMenuEntry(
			"CreateKawaiiBakeBlueprint",
			LOCTEXT("Create", "创建 Kawaii 物理烘焙蓝图"),
			LOCTEXT("CreateHelp", "创建支持多部件同步预览和物理烘焙的动画蓝图。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FKawaiiPhysicsBakerEditorModule::CreateBakeBlueprint))
		);
	}

	void CreateBakeBlueprint() {
		TStrongObjectPtr<UKawaiiPhysicsBakeFactory> Factory(NewObject<UKawaiiPhysicsBakeFactory>());
		if (!Factory->ConfigureProperties()) {
			return;
		}
		UAnimBlueprint* Asset = Cast<UAnimBlueprint>(
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools")
				.Get()
				.CreateAssetWithDialog(Factory->GetDefaultNewAssetName(), TEXT("/Game/KawaiiBakes"), UAnimBlueprint::StaticClass(), Factory.Get())
		);
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
