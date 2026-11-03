using UnrealBuildTool;

public class KawaiiPhysicsBaker : ModuleRules
{
    public KawaiiPhysicsBaker(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AnimGraphRuntime", "KawaiiPhysics", "KawaiiPhysicsBakerRuntime" });
    }
}
