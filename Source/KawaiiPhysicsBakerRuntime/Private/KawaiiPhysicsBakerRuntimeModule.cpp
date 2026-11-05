#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#include "KawaiiPhysicsGPUCompute.h"

class FKawaiiPhysicsBakerRuntimeModule : public IModuleInterface
{
	virtual void StartupModule() override {
		const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("KawaiiPhysicsBaker"));
		if (Plugin) {
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/KawaiiPhysicsBakerRuntime"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
		KawaiiPhysicsGPU::InitializeComputeSystem();
	}

	virtual void ShutdownModule() override { KawaiiPhysicsGPU::ShutdownComputeSystem(); }
};
IMPLEMENT_MODULE(FKawaiiPhysicsBakerRuntimeModule, KawaiiPhysicsBakerRuntime)
