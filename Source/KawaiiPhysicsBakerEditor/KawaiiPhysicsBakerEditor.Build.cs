using UnrealBuildTool;

public class KawaiiPhysicsBakerEditor : ModuleRules
{
    public KawaiiPhysicsBakerEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "KawaiiPhysicsBaker" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "KawaiiPhysics", "KawaiiPhysicsEd", "KawaiiPhysicsBakerRuntime", "AnimGraphRuntime", "AnimGraph", "BlueprintGraph",
            "AnimationBlueprintEditor", "Kismet", "KismetCompiler", "KismetWidgets", "UnrealEd", "EditorFramework",
            "Slate", "SlateCore", "InputCore", "ToolMenus", "PropertyEditor", "ContentBrowser", "EditorWidgets",
            "AssetTools", "AssetRegistry", "AdvancedPreviewScene", "Persona", "AnimationDataController",
            "SequenceRecorder", "Projects", "RenderCore", "KawaiiPhysicsBakerGPU", "RHI", "Json", "JsonUtilities"
        });
    }
}
