#include "Modules/ModuleManager.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

class FKawaiiPhysicsBakerGPUModule : public IModuleInterface
{
	virtual void StartupModule() override {
		const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("KawaiiPhysicsBaker"));
		if (Plugin) {
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/KawaiiPhysicsBaker"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};
IMPLEMENT_MODULE(FKawaiiPhysicsBakerGPUModule, KawaiiPhysicsBakerGPU)
