#include "KawaiiCalibrationGPU.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "DataDrivenShaderPlatformInfo.h"

class FKawaiiCalibrationCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FKawaiiCalibrationCS);
	SHADER_USE_PARAMETER_STRUCT(FKawaiiCalibrationCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, BoneCount)
		SHADER_PARAMETER(uint32, CapsuleCount)
		SHADER_PARAMETER(uint32, CandidateCount)
		SHADER_PARAMETER(uint32, FirstFrame)
		SHADER_PARAMETER(uint32, FrameCount)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(float, SoftMargin)
		SHADER_PARAMETER(float, SoftResponse)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Bones)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Poses)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Ranges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Capsules)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Frames)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Candidates)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Previous)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Velocities)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Scores)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
};

IMPLEMENT_GLOBAL_SHADER(FKawaiiCalibrationCS, "/Plugin/KawaiiPhysicsBaker/Private/KawaiiCalibration.usf", "Main", SF_Compute);

struct FKawaiiCalibrationGPU::FState
{
	FKawaiiCalibrationGPUInput Input;
	TRefCountPtr<FRDGPooledBuffer> Bones, Poses, Ranges, Capsules, Frames, Candidates, Positions, Previous, Velocities, Scores;
	TUniquePtr<FRHIGPUBufferReadback> Readback;
	TArray<FVector4f> Result;
	FString Error;
	TAtomic<bool> bBusy{false}, bDone{false};
	TAtomic<uint32> NextFrame{0};
};

FKawaiiCalibrationGPU::FKawaiiCalibrationGPU() : State(MakeShared<FState, ESPMode::ThreadSafe>()) {
}

FKawaiiCalibrationGPU::~FKawaiiCalibrationGPU() = default;

TSharedPtr<FKawaiiCalibrationGPU, ESPMode::ThreadSafe> FKawaiiCalibrationGPU::Start(const FKawaiiCalibrationGPUInput& In, FString& OutError) {
	if (!GDynamicRHI || GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) {
		OutError = TEXT("GPU 校准需要支持 SM5 计算着色器的实际图形接口，不支持 NullRHI，也不会自动改用 CPU 筛选。");
		return nullptr;
	}
	if (In.Bones.IsEmpty()
		|| In.Bones.Num() > 512
		|| In.Frames.IsEmpty()
		|| In.Frames.Num() > 20000
		|| In.Candidates.IsEmpty()
		|| In.Candidates.Num() > 1024
		|| In.CapsuleCount > 32
		|| In.Poses.Num() != In.Frames.Num() * In.Bones.Num()
		|| (!In.Ranges.IsEmpty() && In.Ranges.Num() != In.Poses.Num())
		|| In.Capsules.Num() != In.Frames.Num() * In.CapsuleCount * 2
		|| !FMath::IsFinite(In.DeltaTime)
		|| In.DeltaTime <= 0
		|| In.DeltaTime > .1f)
	{
		OutError = TEXT("GPU 校准数据无效或超出大小限制。");
		return nullptr;
	}
	for (int32 I = 0; I < In.Bones.Num(); ++I) {
		if (!FMath::IsFinite(In.Bones[I].X) || In.Bones[I].X < -1 || In.Bones[I].X >= I || In.Bones[I].X != FMath::FloorToFloat(In.Bones[I].X)) {
			OutError = TEXT("GPU 骨骼层级未按父骨骼在前的顺序排列。");
			return nullptr;
		}
	}
	const auto Finite = [](const FVector4f& V) {
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z) && FMath::IsFinite(V.W);
	};
	for (const auto* Values : {&In.Bones, &In.Poses, &In.Ranges, &In.Capsules, &In.Frames, &In.Candidates}) {
		for (const auto& V : *Values) {
			if (!Finite(V)) {
				OutError = TEXT("GPU 校准输入包含非有限数值。");
				return nullptr;
			}
		}
	}
	if (In.Gravity.ContainsNaN()
		|| !FMath::IsFinite(In.SoftMargin)
		|| In.SoftMargin < 0
		|| In.SoftMargin > 5
		|| !FMath::IsFinite(In.SoftResponse)
		|| In.SoftResponse < 0
		|| In.SoftResponse > 100)
	{
		OutError = TEXT("GPU 碰撞参数超出运行时支持范围。");
		return nullptr;
	}
	for (const auto& C : In.Candidates) {
		if (C.X < .1f || C.X > 4 || C.Y < .1f || C.Y > 4 || C.Z < .25f || C.Z > 4 || C.W < 1 || C.W > 8 || C.W != FMath::FloorToFloat(C.W)) {
			OutError = TEXT("GPU 候选参数超出运行时支持范围。");
			return nullptr;
		}
	}
	TSharedPtr<FKawaiiCalibrationGPU, ESPMode::ThreadSafe> Result = MakeShareable(new FKawaiiCalibrationGPU());
	Result->State->Input = In;
	if (In.Ranges.IsEmpty()) {
		Result->State->Input.Ranges.Init(FVector4f(0, 0, 0, -1), In.Poses.Num());
	}
	Result->Tick();
	return Result;
}

void FKawaiiCalibrationGPU::Tick() {
	if (State->bDone.Load() || State->bBusy.Exchange(true)) {
		return;
	}
	auto S = State;
	ENQUEUE_RENDER_COMMAND(KawaiiCalibrationStep)([S](FRHICommandListImmediate& RHICmdList) {
		if (S->Readback) {
			if (S->Readback->IsReady()) {
				S->Result.SetNum(S->Input.Candidates.Num());
				const uint32 Bytes = S->Result.Num() * sizeof(FVector4f);
				const void* Data = S->Readback->Lock(Bytes);
				if (Data) {
					FMemory::Memcpy(S->Result.GetData(), Data, Bytes);
				} else {
					S->Error = TEXT("GPU 评分结果回读失败。");
				}
				S->Readback->Unlock();
				S->bDone.Store(true);
			}
			S->bBusy.Store(false);
			return;
		}
		TShaderMapRef<FKawaiiCalibrationCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		if (!Shader.IsValid()) {
			S->Error = TEXT("校准计算着色器不可用，请重新编译插件后重启编辑器。");
			S->bDone.Store(true);
			S->bBusy.Store(false);
			return;
		}
		FRDGBuilder Graph(RHICmdList);
		const auto InputBuffer = [&](TRefCountPtr<FRDGPooledBuffer>& Saved, const TArray<FVector4f>& Data, const TCHAR* Name) {
			if (Saved) {
				return Graph.RegisterExternalBuffer(Saved);
			}
			const FVector4f Zero(0, 0, 0, 0);
			auto Buffer = CreateStructuredBuffer(Graph, Name, sizeof(FVector4f), FMath::Max(1, Data.Num()), Data.IsEmpty() ? &Zero : Data.GetData(), FMath::Max(1, Data.Num()) * sizeof(FVector4f));
			Graph.QueueBufferExtraction(Buffer, &Saved);
			return Buffer;
		};
		const auto StateBuffer = [&](TRefCountPtr<FRDGPooledBuffer>& Saved, uint32 Count, const TCHAR* Name) {
			if (Saved) {
				return Graph.RegisterExternalBuffer(Saved);
			}
			auto Buffer = Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Count), Name);
			AddClearUAVPass(Graph, Graph.CreateUAV(Buffer), 0u);
			Graph.QueueBufferExtraction(Buffer, &Saved);
			return Buffer;
		};
		auto* P = Graph.AllocParameters<FKawaiiCalibrationCS::FParameters>();
		P->BoneCount = S->Input.Bones.Num();
		P->CapsuleCount = S->Input.CapsuleCount;
		P->CandidateCount = S->Input.Candidates.Num();
		P->FirstFrame = S->NextFrame.Load();
		// Avoid putting eight expensive frames into one dispatch for large search jobs.
		const uint64 WorkPerFrame = uint64(P->CandidateCount) * P->BoneCount * (1 + P->CapsuleCount * 8);
		const uint32 FramesPerDispatch = FMath::Clamp<uint64>(4000000 / FMath::Max<uint64>(1, WorkPerFrame), 1, 8);
		P->FrameCount = FMath::Min(FramesPerDispatch, uint32(S->Input.Frames.Num()) - P->FirstFrame);
		P->DeltaTime = S->Input.DeltaTime;
		P->Gravity = S->Input.Gravity;
		P->SoftMargin = S->Input.SoftMargin;
		P->SoftResponse = S->Input.SoftResponse;
		P->Bones = Graph.CreateSRV(InputBuffer(S->Bones, S->Input.Bones, TEXT("Kawaii.Bones")));
		P->Poses = Graph.CreateSRV(InputBuffer(S->Poses, S->Input.Poses, TEXT("Kawaii.Poses")));
		P->Ranges = Graph.CreateSRV(InputBuffer(S->Ranges, S->Input.Ranges, TEXT("Kawaii.Ranges")));
		P->Capsules = Graph.CreateSRV(InputBuffer(S->Capsules, S->Input.Capsules, TEXT("Kawaii.Capsules")));
		P->Frames = Graph.CreateSRV(InputBuffer(S->Frames, S->Input.Frames, TEXT("Kawaii.Frames")));
		P->Candidates = Graph.CreateSRV(InputBuffer(S->Candidates, S->Input.Candidates, TEXT("Kawaii.Candidates")));
		P->Positions = Graph.CreateUAV(StateBuffer(S->Positions, P->BoneCount * P->CandidateCount, TEXT("Kawaii.Positions")));
		P->Previous = Graph.CreateUAV(StateBuffer(S->Previous, P->BoneCount * P->CandidateCount, TEXT("Kawaii.Previous")));
		P->Velocities = Graph.CreateUAV(StateBuffer(S->Velocities, P->BoneCount * P->CandidateCount, TEXT("Kawaii.Velocities")));
		auto Scores = StateBuffer(S->Scores, P->CandidateCount, TEXT("Kawaii.Scores"));
		P->Scores = Graph.CreateUAV(Scores);
		FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("Kawaii calibration candidates"), Shader, P, FIntVector(FMath::DivideAndRoundUp(P->CandidateCount, 64u), 1, 1));
		const uint32 Next = P->FirstFrame + P->FrameCount;
		if (Next == uint32(S->Input.Frames.Num())) {
			S->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("KawaiiCalibrationScores"));
			AddEnqueueCopyPass(Graph, S->Readback.Get(), Scores, 0u);
		}
		Graph.Execute();
		S->NextFrame.Store(Next);
		S->bBusy.Store(false);
	});
}

bool FKawaiiCalibrationGPU::IsDone() const {
	return State->bDone.Load();
}

float FKawaiiCalibrationGPU::GetProgress() const {
	return float(State->NextFrame.Load()) / FMath::Max(1, State->Input.Frames.Num());
}

const TArray<FVector4f>& FKawaiiCalibrationGPU::GetScores() const {
	check(IsDone());
	return State->Result;
}

const FString& FKawaiiCalibrationGPU::GetError() const {
	check(IsDone());
	return State->Error;
}
