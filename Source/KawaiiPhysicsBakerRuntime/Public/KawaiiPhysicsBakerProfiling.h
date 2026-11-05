#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("Kawaii Physics Baker"), STATGROUP_KawaiiPhysicsBaker, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Proxy: resolve actions"), STAT_KawaiiBakerProxyResolve, STATGROUP_KawaiiPhysicsBaker, KAWAIIPHYSICSBAKERRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Proxy: sample and blend"), STAT_KawaiiBakerProxySample, STATGROUP_KawaiiPhysicsBaker, KAWAIIPHYSICSBAKERRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Proxy: full asset validation"), STAT_KawaiiBakerProxyValidate, STATGROUP_KawaiiPhysicsBaker, KAWAIIPHYSICSBAKERRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(
	TEXT("Correction: game-thread snapshot"),
	STAT_KawaiiBakerCorrectionSnapshot,
	STATGROUP_KawaiiPhysicsBaker,
	KAWAIIPHYSICSBAKERRUNTIME_API
);
DECLARE_CYCLE_STAT_EXTERN(
	TEXT("Correction: evaluate (includes input poses)"),
	STAT_KawaiiBakerCorrectionEvaluate,
	STATGROUP_KawaiiPhysicsBaker,
	KAWAIIPHYSICSBAKERRUNTIME_API
);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Correction: solve chain"), STAT_KawaiiBakerSolveChain, STATGROUP_KawaiiPhysicsBaker, KAWAIIPHYSICSBAKERRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Solver calls / frame"), STAT_KawaiiBakerSolverCalls, STATGROUP_KawaiiPhysicsBaker, KAWAIIPHYSICSBAKERRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Solver points / frame (sum across chains)"),
	STAT_KawaiiBakerSolverPoints,
	STATGROUP_KawaiiPhysicsBaker,
	KAWAIIPHYSICSBAKERRUNTIME_API
);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Solver iterations / frame (sum across chains)"),
	STAT_KawaiiBakerSolverIterations,
	STATGROUP_KawaiiPhysicsBaker,
	KAWAIIPHYSICSBAKERRUNTIME_API
);

namespace KawaiiPhysicsBakerProfiling {
	/** Detailed per-iteration timers are opt-in to avoid distorting small solves. */
	KAWAIIPHYSICSBAKERRUNTIME_API bool IsDetailedSolverTracingEnabled();
}
