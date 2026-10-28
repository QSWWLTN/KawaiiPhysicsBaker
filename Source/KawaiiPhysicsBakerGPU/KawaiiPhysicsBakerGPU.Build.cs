using UnrealBuildTool;
public class KawaiiPhysicsBakerGPU : ModuleRules
{
    public KawaiiPhysicsBakerGPU(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core" });
        PrivateDependencyModuleNames.AddRange(new[] { "CoreUObject", "Engine", "RHI", "RenderCore", "Projects" });
    }
}
