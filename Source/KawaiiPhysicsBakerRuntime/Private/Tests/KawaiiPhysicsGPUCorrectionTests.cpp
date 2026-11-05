#include "Misc/AutomationTest.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsGPUShaders.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "KawaiiPhysicsIKCorrectionMath.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiGPUHandoffTest,
	"KawaiiPhysicsBaker.GPU.Handoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiGPUHandoffTest::RunTest(const FString&) {
	KawaiiPhysicsGPU::FState State;
	int32 NodeA = 0, NodeB = 0;
	State.bResourcesReady = true;
	State.Register(&NodeA, 1);
	TestFalse(TEXT("Registration alone does not request GPU correction"), State.bCorrectionRequested);
	State.RequestCorrection(&NodeB);
	TestFalse(TEXT("Another node cannot keep this deformer installed"), State.bCorrectionRequested);
	State.RequestCorrection(&NodeA);
	TestTrue(TEXT("Valid correction requests GPU even before a packet is ready"), State.bCorrectionRequested);
	TestTrue(TEXT("One node may submit"), State.CanSubmit(&NodeA));
	auto Packet = MakeShared<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe>();
	TestTrue(TEXT("Owned packet accepted"), State.Submit(&NodeA, Packet));
	TestTrue(TEXT("Render thread receives packet"), State.Read().IsValid());
	State.Register(&NodeB, 1);
	TestFalse(TEXT("Multiple nodes fall back instead of racing"), State.CanSubmit(&NodeA));
	TestFalse(TEXT("Conflicting packet cleared"), State.Read().IsValid());
	State.Register(&NodeA, 2);
	TestFalse(TEXT("Each update must renew its correction request"), State.bCorrectionRequested);
	TestTrue(TEXT("New evaluation can recover"), State.CanSubmit(&NodeA));
	State.Submit(&NodeA, MakeShared<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe>());
	State.Register(&NodeA, 3);
	TestFalse(TEXT("Skipped evaluation cannot reuse stale packet"), State.Read().IsValid());
	State.Submit(&NodeA, MakeShared<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe>());
	State.Invalidate(&NodeA);
	TestFalse(TEXT("Reset clears packet"), State.Read().IsValid());
	TestFalse(TEXT("Skipped correction releases the GPU request"), State.bCorrectionRequested);
	State.RequestCorrection(&NodeA);
	State.Submit(&NodeA, MakeShared<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe>());
	State.RequireCPU(TEXT("downstream bone edit"));
	TestFalse(TEXT("Rejected pose releases its correction request"), State.bCorrectionRequested);
	TestFalse(TEXT("Rejected pose cannot reach the render thread"), State.Read().IsValid());
	State.bResourcesReady = true;
	State.Invalidate(&NodeA);
	State.Register(&NodeA, 4);
	State.RequestCorrection(&NodeA);
	TestFalse(TEXT("Resource recreation must not clear a CPU fallback"), State.CanSubmit(&NodeA));
	TestFalse(TEXT("Rejected node cannot reinstall a deformer each frame"), State.bCorrectionRequested);
	TestFalse(TEXT("Rejected node cannot submit again"), State.Submit(&NodeA, MakeShared<KawaiiPhysicsGPU::FFrame, ESPMode::ThreadSafe>()));
	TestEqual(TEXT("Rejection reason survives subsequent evaluations"), State.LastReason, FString(TEXT("downstream bone edit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiGPUFinalPoseTest,
	"KawaiiPhysicsBaker.GPU.FinalPoseGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiGPUFinalPoseTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsGPU;
	const FTransform Baseline(FRotator(15.f, 25.f, -30.f), FVector(12.f, -23.f, 145.f), FVector(1.1f, .9f, 1.2f));
	const FQuat Rotation = Baseline.GetRotation();
	KawaiiPhysicsGPU::FFrame Frame;
	Frame.PointBones = {1};
	Frame.Points.SetNumZeroed(PointStride);
	Frame.Points[1] = FVector4f(FVector3f(Baseline.GetTranslation()), 0.f);
	Frame.Points[2] = FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
	Frame.Points[3] = FVector4f(FVector3f(Baseline.GetScale3D()), 0.f);
	TArray<FTransform> Pose = {FTransform::Identity, Baseline};
	int32 MismatchedBone = INDEX_NONE;
	TestTrue(TEXT("Matching final pose accepts float upload precision"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	Pose[0].SetTranslation(FVector(100.f, 50.f, 25.f));
	TestTrue(TEXT("An unrelated component-space bone does not invalidate the chain"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	Pose[1].AddToTranslation(FVector(0.f, 0.f, 1.f));
	TestFalse(TEXT("Downstream translation is rejected"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	TestEqual(TEXT("Rejected bone is reported"), MismatchedBone, 1);
	Pose[1] = Baseline;
	Pose[1].SetRotation(FQuat(FVector::UpVector, .1f) * Rotation);
	TestFalse(TEXT("Downstream rotation is rejected before skinning"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	Pose[1] = Baseline;
	Pose[1].SetScale3D(FVector(1.1f, .9f, 1.3f));
	TestFalse(TEXT("Downstream scale is rejected before skinning"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	Frame.PointBones[0] = 2;
	TestFalse(TEXT("Missing final-pose bone falls back safely"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	Frame.PointBones[0] = 1;
	Frame.Points.Pop();
	TestFalse(TEXT("Incomplete correction packet is rejected"), MatchesComponentPose(Frame, Pose, MismatchedBone));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiGPUSolveTest,
	"KawaiiPhysicsBaker.GPU.SolverParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiGPUSolveTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsGPU;
	if (!GDynamicRHI || GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) {
		AddWarning(TEXT("GPU parity requires a real SM5 RHI; skipped under NullRHI."));
		return true;
	}
	KawaiiPhysicsGPU::FFrame Frame;
	Frame.DeltaTime = 1.f / 60.f;
	Frame.Topology = 123;
	Frame.ResetSerial = 1;
	const TArray<TArray<FVector>> Cases = {
		{{0, 0, 10}, {0, 0, 5}, {0, 0, 0}, {0, 0, -5}},
		{{-5, 0, 0}, {5, 0, 0}}, // Segment crosses a capsule although its endpoints are outside.
		{{0, 0, 0}, {0, 0, -4}, {0, 0, -8}}, // Pinned root overlap must remain finite.
		{{-5, 0, 3}, {-2, 0, 3}, {1, 0, 3}, {4, 0, 3}},
		{{0, 0, 5}, {0, 0, 5}, {0, 0, 0}} // Zero-length segment.
	};
	const TArray<KawaiiPhysicsIKCorrection::FCapsule> Capsules = {{FVector(0, -3, 0), FVector(0, 3, 0), 1.5}, {FVector(2, -2, -3), FVector(2, 2, -3), 1.0}};
	TArray<FVector> Expected;
	TArray<FVector> Reference;
	for (const auto& Input : Cases) {
		FChain Chain;
		Chain.Range = FUintVector4(Expected.Num(), Input.Num(), Frame.Capsules.Num() / 2, Capsules.Num());
		Chain.Parameters = FVector4f(0.25f, 1.f, 16.f, 0.f);
		Chain.AnchorRotation = FVector4f(0, 0, 0, 1);
		Chain.AnchorPosition = FVector4f(0, 0, 0, 0);
		Chain.AnchorScale = FVector4f(1, 1, 1, 0);
		Frame.Chains.Add(Chain);
		TArray<double> Lengths;
		for (int32 Index = 0; Index < Input.Num(); ++Index) {
			const double Length = Index > 0 ? FVector::Distance(Input[Index - 1], Input[Index]) : 0;
			if (Index > 0) {
				Lengths.Add(Length);
			}
			Frame.Points.Append(
				{FVector4f(FVector3f(Input[Index]), Length),
				 FVector4f(FVector3f(Input[Index]), 0),
				 FVector4f(0, 0, 0, 1),
				 FVector4f(1, 1, 1, 0),
				 FVector4f(0, 0, 0, 1),
				 FVector4f(1, 1, 1, 0)}
			);
		}
		for (const auto& Capsule : Capsules) {
			Frame.Capsules.Add(FVector4f(FVector3f(Capsule.Start), Capsule.Radius));
			Frame.Capsules.Add(FVector4f(FVector3f(Capsule.End), 0));
		}
		auto CPU = Input;
		TestTrue(TEXT("CPU reference finite"), KawaiiPhysicsIKCorrection::SolveChain(CPU, Lengths, Input, Capsules, 16, .25).bFinite);
		Expected.Append(CPU);
		Reference.Append(Input);
	}
	TArray<FVector4f> Result;
	// Synchronous readback exists only in this automation test. Runtime has no readback or GPU wait.
	ENQUEUE_RENDER_COMMAND(KawaiiGPUParity)([&Frame, &Result](FRHICommandListImmediate& RHICmdList) {
		FRenderState State;
		FRHIGPUBufferReadback Readback(TEXT("KawaiiGPU.Parity"));
		FRDGBuilder Graph(RHICmdList);
		auto* Deltas = AddSolvePass(Graph, Frame, State, true);
		AddEnqueueCopyPass(Graph, &Readback, Deltas, 0u);
		Graph.Execute();
		RHICmdList.BlockUntilGPUIdle();
		const uint32 Count = Frame.Points.Num() / PointStride * 3;
		const void* Data = Readback.Lock(Count * sizeof(FVector4f));
		if (Data) {
			Result.SetNumUninitialized(Count);
			FMemory::Memcpy(Result.GetData(), Data, Count * sizeof(FVector4f));
			Readback.Unlock();
		}
	});
	FlushRenderingCommands();
	if (!TestEqual(TEXT("GPU delta count"), Result.Num(), Expected.Num() * 3)) {
		return false;
	}
	double MaxError = 0;
	for (int32 Index = 0; Index < Expected.Num(); ++Index) {
		const FVector4f ReferencePosition(FVector3f(Reference[Index]), 1.f);
		const FVector Position(
			Dot4(Result[Index * 3], ReferencePosition),
			Dot4(Result[Index * 3 + 1], ReferencePosition),
			Dot4(Result[Index * 3 + 2], ReferencePosition)
		);
		const double Error = FVector::Distance(Position, Expected[Index]);
		MaxError = FMath::Max(MaxError, Error);
		TestTrue(FString::Printf(TEXT("Point %d finite and within 0.03 cm (error %.6f)"), Index, Error), !Position.ContainsNaN() && Error < .03);
	}
	AddInfo(FString::Printf(TEXT("GPU/CPU solver max position error: %.6f cm across %d points."), MaxError, Expected.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiGPUNearlyParallelTest,
	"KawaiiPhysicsBaker.GPU.NearlyParallelContactParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiGPUNearlyParallelTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsGPU;
	if (!GDynamicRHI || GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) {
		AddWarning(TEXT("Nearly parallel contact parity requires a real SM5 RHI."));
		return true;
	}
	KawaiiPhysicsGPU::FFrame Frame;
	TArray<FVector> Reference, Expected;
	// Both endpoints are outside the capsule, so only the segment-contact pass
	// can move the strand. UE treats these directions as nearly parallel and
	// selects the pinned-root end, whereas the former GPU algorithm selected the
	// interior crossing and moved the tip by twice the capsule radius.
	for (const double Scale : {0.1, 1.0, 10.0}) {
		for (const bool bReverseCapsule : {false, true}) {
			const TArray<FVector> Input = {FVector::ZeroVector, FVector(0, 0, 10 * Scale)};
			FVector Start(.04 * Scale, 0, 0), End(-.04 * Scale, 0, 10 * Scale);
			if (bReverseCapsule) {
				Swap(Start, End);
			}
			const TArray<KawaiiPhysicsIKCorrection::FCapsule> Capsules = {{Start, End, .03 * Scale}};
			const TArray<double> Lengths = {10 * Scale};
			auto CPU = Input;
			TestTrue(TEXT("Nearly parallel CPU solution is finite"), KawaiiPhysicsIKCorrection::SolveChain(CPU, Lengths, Input, Capsules, 1, 0).bFinite);
			TestTrue(TEXT("Fixture isolates the near-parallel contact choice"), CPU[1].Equals(Input[1], 1.e-6));
			FChain Chain;
			Chain.Range = FUintVector4(Reference.Num(), Input.Num(), Frame.Capsules.Num() / 2, 1);
			Chain.Parameters = FVector4f(0, 1, 1, 0);
			Chain.AnchorRotation = FVector4f(0, 0, 0, 1);
			Chain.AnchorPosition = FVector4f(0, 0, 0, 0);
			Chain.AnchorScale = FVector4f(1, 1, 1, 0);
			Frame.Chains.Add(Chain);
			for (int32 Point = 0; Point < Input.Num(); ++Point) {
				Frame.Points.Append(
					{FVector4f(FVector3f(Input[Point]), Point == 0 ? 0 : Lengths[0]),
					 FVector4f(FVector3f(Input[Point]), 0),
					 FVector4f(0, 0, 0, 1),
					 FVector4f(1, 1, 1, 0),
					 FVector4f(0, 0, 0, 1),
					 FVector4f(1, 1, 1, 0)}
				);
			}
			Frame.Capsules.Add(FVector4f(FVector3f(Start), Capsules[0].Radius));
			Frame.Capsules.Add(FVector4f(FVector3f(End), 0));
			Reference.Append(Input);
			Expected.Append(CPU);
		}
	}
	TArray<FVector4f> Result;
	ENQUEUE_RENDER_COMMAND(KawaiiGPUNearlyParallelParity)([&Frame, &Result](FRHICommandListImmediate& RHICmdList) {
		FRenderState State;
		FRHIGPUBufferReadback Readback(TEXT("KawaiiGPU.NearlyParallelParity"));
		FRDGBuilder Graph(RHICmdList);
		auto* Deltas = AddSolvePass(Graph, Frame, State, true);
		AddEnqueueCopyPass(Graph, &Readback, Deltas, 0u);
		Graph.Execute();
		RHICmdList.BlockUntilGPUIdle();
		const uint32 Count = Frame.Points.Num() / PointStride * 3;
		if (const void* Data = Readback.Lock(Count * sizeof(FVector4f))) {
			Result.SetNumUninitialized(Count);
			FMemory::Memcpy(Result.GetData(), Data, Count * sizeof(FVector4f));
			Readback.Unlock();
		}
	});
	FlushRenderingCommands();
	if (!TestEqual(TEXT("Nearly parallel GPU delta count"), Result.Num(), Expected.Num() * 3)) {
		return false;
	}
	double MaxError = 0;
	for (int32 Point = 0; Point < Expected.Num(); ++Point) {
		const FVector4f Position(FVector3f(Reference[Point]), 1);
		const FVector Actual(Dot4(Result[Point * 3], Position), Dot4(Result[Point * 3 + 1], Position), Dot4(Result[Point * 3 + 2], Position));
		const double Error = FVector::Distance(Actual, Expected[Point]);
		MaxError = FMath::Max(MaxError, Error);
		TestTrue(FString::Printf(TEXT("Near-parallel point %d error %.6f cm"), Point, Error), !Actual.ContainsNaN() && Error < .001);
	}
	AddInfo(FString::Printf(TEXT("Near-parallel GPU/CPU max error %.6f cm across six scaled/directed contact cases."), MaxError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiGPUTemporalTest,
	"KawaiiPhysicsBaker.GPU.TemporalParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiGPUTemporalTest::RunTest(const FString&) {
	if (!GDynamicRHI || GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) {
		AddWarning(TEXT("Temporal parity requires a real SM5 RHI."));
		return true;
	}
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>());
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("anchor"), TEXT("anchor"), 0), FTransform(FVector(0, 0, 100)));
		Modifier.Add(FMeshBoneInfo(TEXT("hair0"), TEXT("hair0"), 1), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("hair1"), TEXT("hair1"), 2), FTransform(FVector(0, 0, -10)));
		Modifier.Add(FMeshBoneInfo(TEXT("hair2"), TEXT("hair2"), 3), FTransform(FVector(0, 0, -10)));
	}
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
	Mesh->SetSkeleton(Skeleton.Get());
	Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh.Get());
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	TArray<FBoneIndexType> Indices{0, 1, 2, 3, 4};
	const UE::Anim::FCurveFilterSettings Filter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(Indices, Filter, *Mesh);
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(Indices, Filter, *Mesh);
	TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Profile(NewObject<UKawaiiPhysicsIKCorrectionProfile>());
	FKawaiiPhysicsIKCorrectionChain Chain;
	Chain.RootBone.BoneName = TEXT("hair0");
	Chain.TipBone.BoneName = TEXT("hair2");
	Chain.DriverBone.BoneName = TEXT("root");
	Chain.DriverWeight = 0;
	Profile->Chains = {Chain};
	Profile->Iterations = 32;
	FAnimNode_KawaiiPhysicsIKCorrection CPU;
	CPU.Profile = Profile.Get();
	CPU.PreUpdate(Instance.Get());
	auto Render = MakeShared<KawaiiPhysicsGPU::FRenderState, ESPMode::ThreadSafe>();
	double MaxError = 0;
	for (int32 Step = 0; Step < 48; ++Step) {
		FMemMark Mark(FMemStack::Get());
		const float Delta = Step == 18 ? .5f : 1.f / 60.f;
		if (Step == 25) {
			CPU.ResetTemporalState();
		}
		CPU.Alpha = Step < 24 ? 1.f : .45f;
		if (Step >= 32) {
			// Automatic proxies now fade with their action's original blend weight.
			CPU.Alpha = .45f * (47 - Step) / 16.f;
		}
		FTransform Anchor(FQuat(FVector::ZAxisVector, Step * .04), FVector(Step * .2, Step * -.1, 100));
		FPoseContext Output(&Proxy);
		Output.Pose.ResetToRefPose();
		Output.Pose[FCompactPoseBoneIndex(1)] = Anchor;
		Output.Pose[FCompactPoseBoneIndex(2)].SetRotation(FQuat(FVector::YAxisVector, FMath::DegreesToRadians(FMath::Min(-2.5 + Step, 2.5))));
		FPoseContext Baked(Output);
		Baked.Pose.CopyBonesFrom(Output.Pose);
		TArray<FTransform> Reference;
		for (int32 Index = 0; Index < 5; ++Index) {
			const auto& Local = Output.Pose[FCompactPoseBoneIndex(Index)];
			Reference.Add(Index == 0 ? Local : Local * Reference[Index - 1]);
		}
		FKawaiiClothingProxyCapsule Capsule;
		Capsule.Start = Anchor.TransformPosition(FVector(0, -20, -15));
		Capsule.End = Anchor.TransformPosition(FVector(0, 20, -15));
		Capsule.Radius = 2.f;
		CPU.ExternalCapsules = {Capsule};
		CPU.AccumulateDeltaTime(Delta);
		CPU.CorrectPose(Output, Baked);
		TArray<FTransform> Expected;
		for (int32 Index = 0; Index < 5; ++Index) {
			const auto& Local = Output.Pose[FCompactPoseBoneIndex(Index)];
			Expected.Add(Index == 0 ? Local : Local * Expected[Index - 1]);
		}
		const auto ToGPUQuaternion = [](const FQuat& Rotation) { return FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W); };
		KawaiiPhysicsGPU::FFrame Frame;
		Frame.DeltaTime = Delta;
		Frame.SettleTime = Profile->SettleTime;
		Frame.SmoothingTime = Profile->SmoothingTime;
		Frame.Topology = 5;
		Frame.ResetSerial = Step >= 25 ? 2 : 1;
		KawaiiPhysicsGPU::FChain GPUChain;
		GPUChain.Range = FUintVector4(0, 3, 0, 1);
		GPUChain.Parameters = FVector4f(Chain.CollisionRadius, CPU.Alpha, Profile->Iterations, 0);
		GPUChain.AnchorRotation = ToGPUQuaternion(Anchor.GetRotation());
		GPUChain.AnchorPosition = FVector4f(FVector3f(Anchor.GetTranslation()), 0);
		GPUChain.AnchorScale = FVector4f(1, 1, 1, 0);
		Frame.Chains = {GPUChain};
		Frame.Capsules = {FVector4f(FVector3f(Capsule.Start), Capsule.Radius), FVector4f(FVector3f(Capsule.End), 0)};
		for (int32 Index = 2; Index < 5; ++Index) {
			const auto& Transform = Reference[Index];
			Frame.Points.Append(
				{FVector4f(FVector3f(Transform.GetTranslation()), Index == 2 ? 0.f : 10.f),
				 FVector4f(FVector3f(Transform.GetTranslation()), 0),
				 ToGPUQuaternion(Transform.GetRotation()),
				 FVector4f(1, 1, 1, 0),
				 ToGPUQuaternion(Transform.GetRotation()),
				 FVector4f(1, 1, 1, 0)}
			);
		}
		TArray<FVector4f> Result;
		ENQUEUE_RENDER_COMMAND(KawaiiGPUTemporalParity)([&Frame, &Result, Render, Step](FRHICommandListImmediate& RHICmdList) {
			FRHIGPUBufferReadback Readback(TEXT("KawaiiGPU.TemporalParity"));
			FRDGBuilder Graph(RHICmdList);
			auto* Deltas = KawaiiPhysicsGPU::AddSolvePass(Graph, Frame, *Render, Step == 0 || Frame.DeltaTime > .25f);
			AddEnqueueCopyPass(Graph, &Readback, Deltas, 0u);
			Graph.Execute();
			RHICmdList.BlockUntilGPUIdle();
			const void* Data = Readback.Lock(9 * sizeof(FVector4f));
			if (Data) {
				Result.SetNumUninitialized(9);
				FMemory::Memcpy(Result.GetData(), Data, 9 * sizeof(FVector4f));
				Readback.Unlock();
			}
		});
		FlushRenderingCommands();
		if (TestEqual(TEXT("Temporal GPU delta count"), Result.Num(), 9)) {
			for (int32 Index = 0; Index < 3; ++Index) {
				const FVector4f ReferencePosition(FVector3f(Reference[Index + 2].GetTranslation()), 1.f);
				FVector Position(
					Dot4(Result[Index * 3], ReferencePosition),
					Dot4(Result[Index * 3 + 1], ReferencePosition),
					Dot4(Result[Index * 3 + 2], ReferencePosition)
				);
				const double Error = FVector::Distance(Position, Expected[Index + 2].GetTranslation());
				MaxError = FMath::Max(MaxError, Error);
				TestTrue(FString::Printf(TEXT("Frame %d point %d error %.6f cm"), Step, Index, Error), !Position.ContainsNaN() && Error < .03);

				// Origins alone do not catch a bad rotation/scale matrix that stretches weighted skinning.
				for (const FVector& LocalOffset : {FVector(3, 0, 0), FVector(0, 4, 0), FVector(0, 0, 5)}) {
					const FVector4f BaselineVertex(FVector3f(Reference[Index + 2].TransformPosition(LocalOffset)), 1.f);
					const FVector GPUVertex(
						Dot4(Result[Index * 3], BaselineVertex),
						Dot4(Result[Index * 3 + 1], BaselineVertex),
						Dot4(Result[Index * 3 + 2], BaselineVertex)
					);
					const FVector CPUVertex = Expected[Index + 2].TransformPosition(LocalOffset);
					const double VertexError = FVector::Distance(GPUVertex, CPUVertex);
					MaxError = FMath::Max(MaxError, VertexError);
					TestTrue(
						FString::Printf(TEXT("Frame %d bone %d off-axis skinning error %.6f cm"), Step, Index, VertexError),
						!GPUVertex.ContainsNaN() && VertexError < .03
					);
				}
			}
		}
	}
	ENQUEUE_RENDER_COMMAND(KawaiiGPUTemporalRelease)([Render](FRHICommandListImmediate&) { Render->History.SafeRelease(); });
	FlushRenderingCommands();
	AddInfo(FString::Printf(TEXT("GPU/CPU temporal max error %.6f cm, moving anchor, side history, smoothing, alpha, long gap and reset."), MaxError));
	return true;
}
#endif
