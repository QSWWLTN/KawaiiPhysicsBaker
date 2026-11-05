#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "DataDrivenShaderPlatformInfo.h"

#include "KawaiiPhysicsGPUCorrection.h"

class FKawaiiCorrectionSolveCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FKawaiiCorrectionSolveCS);
	SHADER_USE_PARAMETER_STRUCT(FKawaiiCorrectionSolveCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ResetHistory)
		SHADER_PARAMETER(uint32, AdvanceHistory)
		SHADER_PARAMETER(uint32, UseSideHistory)
		SHADER_PARAMETER(uint32, UseSmoothing)
		SHADER_PARAMETER(float, SettleAlpha)
		SHADER_PARAMETER(float, SmoothingAlpha)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiChain>, Chains)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PointInput)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Capsules)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, History)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Deltas)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FKawaiiCorrectionSkinCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FKawaiiCorrectionSkinCS);
	SHADER_USE_PARAMETER_STRUCT(FKawaiiCorrectionSkinCS, FGlobalShader);
	class FUnlimited : SHADER_PERMUTATION_BOOL("GPUSKIN_UNLIMITED_BONE_INFLUENCE");
	class FIndex16 : SHADER_PERMUTATION_BOOL("GPUSKIN_BONE_INDEX_UINT16");
	class FWeight16 : SHADER_PERMUTATION_BOOL("GPUSKIN_BONE_WEIGHTS_UINT16");
	class FApplyCorrection : SHADER_PERMUTATION_BOOL("KAWAII_APPLY_CORRECTION");
	using FPermutationDomain = TShaderPermutationDomain<FUnlimited, FIndex16, FWeight16, FApplyCorrection>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumVertices)
		SHADER_PARAMETER(uint32, BaseVertex)
		SHADER_PARAMETER(uint32, NumInfluences)
		SHADER_PARAMETER(uint32, WeightStride)
		SHADER_PARAMETER(uint32, WeightIndexSize)
		SHADER_PARAMETER(uint32, BoneMapOffset)
		SHADER_PARAMETER(uint32, HasMorph)
		SHADER_PARAMETER_SRV(Buffer<float4>, BoneMatrices)
		SHADER_PARAMETER_SRV(Buffer<uint>, WeightStream)
		SHADER_PARAMETER_SRV(Buffer<uint>, WeightLookup)
		SHADER_PARAMETER_SRV(Buffer<float>, MorphBuffer)
		SHADER_PARAMETER_SRV(Buffer<float>, StaticPositions)
		SHADER_PARAMETER_SRV(Buffer<float4>, StaticTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, BoneToPoint)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Deltas)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, OutputTangents)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

namespace KawaiiPhysicsGPU {
	/** Render-thread-owned persistent history, retired on the render thread. */
	struct FRenderState
	{
		// Set false on resource release before a queued scene task may touch its mesh object.
		bool bActive = true;
		TRefCountPtr<FRDGPooledBuffer> History;
		TRefCountPtr<FRDGPooledBuffer> CorrectionDeltas;
		uint32 PointCount = 0;
		uint32 Topology = 0;
		uint32 ResetSerial = 0;
		uint64 Sequence = 0;
		uint64 LastRenderFrame = 0;
		int32 LOD = INDEX_NONE;
	};

	FRDGBufferRef AddSolvePass(FRDGBuilder& Graph, const FFrame& Frame, FRenderState& State, bool bReset);
}
