#include "KawaiiPhysicsBakerProfiling.h"

#include "HAL/IConsoleManager.h"

DEFINE_STAT(STAT_KawaiiBakerProxyResolve);
DEFINE_STAT(STAT_KawaiiBakerProxySample);
DEFINE_STAT(STAT_KawaiiBakerProxyValidate);
DEFINE_STAT(STAT_KawaiiBakerCorrectionSnapshot);
DEFINE_STAT(STAT_KawaiiBakerCorrectionEvaluate);
DEFINE_STAT(STAT_KawaiiBakerSolveChain);
DEFINE_STAT(STAT_KawaiiBakerSolverCalls);
DEFINE_STAT(STAT_KawaiiBakerSolverPoints);
DEFINE_STAT(STAT_KawaiiBakerSolverIterations);

namespace {
	TAutoConsoleVariable<int32> CVarKawaiiBakerProfileSolverDetails(
		TEXT("a.KawaiiPhysicsBaker.ProfileSolverDetails"),
		0,
		TEXT(
			"Emit per-iteration length/point/segment constraint CPU trace scopes. Default 0. Requires cpu trace channel; detailed tracing adds measurement overhead."),
		ECVF_Default
	);
}

bool KawaiiPhysicsBakerProfiling::IsDetailedSolverTracingEnabled() {
	return CVarKawaiiBakerProfileSolverDetails.GetValueOnAnyThread() != 0;
}
