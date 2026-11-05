#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class UAnimInstance;

namespace KawaiiPhysicsGPU {
	constexpr uint32 MaxChainPoints = 64;
	constexpr uint32 PointStride = 6;

	/** GPU layout: one group per chain; chain-local projections retain their CPU ordering. */
	struct FChain
	{
		FUintVector4 Range; // first point, point count, first capsule, capsule count
		FVector4f Parameters; // particle radius, alpha, iterations, reserved
		FVector4f AnchorRotation;
		FVector4f AnchorPosition;
		FVector4f AnchorScale;
	};

	/** Immutable worker-to-render snapshot. No UObjects or borrowed pose storage. */
	struct FFrame
	{
		TArray<FChain> Chains;
		// target/rest length, reference, baseline quaternion/scale, warped quaternion/scale
		TArray<FVector4f> Points;
		TArray<FVector4f> Capsules; // start/radius, end
		TArray<int32> BoneToPoint; // mesh reference skeleton index -> nearest corrected ancestor
		TArray<int32> PointBones; // exact mesh bone for each solver point, used to validate the final pose
		uint32 Topology = 0;
		uint32 ResetSerial = 0;
		uint64 Sequence = 0;
		uint64 GameFrame = 0;
		float DeltaTime = 0.f;
		float SettleTime = 0.f;
		float SmoothingTime = 0.f;
	};

	/** Animation workers and the render submission thread exchange owned, immutable packets here. */
	struct FState
	{
		void Register(const void* Node, uint64 FrameNumber);
		bool CanSubmit(const void* Node);
		void RequestCorrection(const void* Node);
		void Invalidate(const void* Node);
		void RequireCPU(const FString& Reason);
		bool Submit(const void* Node, TSharedPtr<FFrame, ESPMode::ThreadSafe> Packet);
		TSharedPtr<const FFrame, ESPMode::ThreadSafe> Read();

		FCriticalSection Mutex;
		TSharedPtr<const FFrame, ESPMode::ThreadSafe> Frame;
		const void* Owner = nullptr;
		uint64 RegistrationFrame = MAX_uint64;
		uint64 Sequence = 0;
		uint32 ResetSerial = 1;
		bool bConflict = false;
		bool bResourcesReady = false;
		bool bCorrectionRequested = false;
		bool bRequiresCPU = false;
		FString LastReason;
	};

	/** Game thread only. Installs our deformer only after a valid correction, and removes it when correction stops. */
	KAWAIIPHYSICSBAKERRUNTIME_API TSharedPtr<FState, ESPMode::ThreadSafe> Prepare(const UAnimInstance* Instance,
																				  const void* Node,
																				  bool bNeedsCorrection,
																				  const TSharedPtr<FState, ESPMode::ThreadSafe>& PreviousState);

	/** A downstream bone edit invalidates the matrices built against the correction node's baseline. */
	bool MatchesComponentPose(const FFrame& Frame, TConstArrayView<FTransform> ComponentPose, int32& OutMismatchedBone);
}
