using UnrealBuildTool;

public class KawaiiPhysicsBakerRuntime : ModuleRules
{
    public KawaiiPhysicsBakerRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AnimGraphRuntime" });
    }
}
