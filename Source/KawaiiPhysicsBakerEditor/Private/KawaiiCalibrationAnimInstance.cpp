#include "KawaiiCalibrationAnimInstance.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeSpaceConversions.h"

struct FCalibrationInputPose : FAnimNode_Base
{
	TArray<FTransform> Local;
	uint64 EvaluationSerial = 0;

	virtual void Evaluate_AnyThread(FPoseContext& Output) override {
		++EvaluationSerial;
		Output.ResetToRefPose();
		const auto& Bones = Output.Pose.GetBoneContainer();
		for (auto I : Output.Pose.ForEachBoneIndex()) {
			const int32 MeshIndex = Bones.MakeMeshPoseIndex(I).GetInt();
			if (Local.IsValidIndex(MeshIndex)) {
				Output.Pose[I] = Local[MeshIndex];
			}
		}
	}
};

struct FCalibrationAnimProxy : FAnimInstanceProxy
{
	FCalibrationInputPose Input;
	FAnimNode_ConvertLocalToComponentSpace ToCS;
	FAnimNode_KawaiiPhysics Physics;
	FAnimNode_ConvertComponentToLocalSpace ToLocal;
	bool bPhysics = false;

	FCalibrationAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	virtual FAnimNode_Base* GetCustomRootNode() override { return bPhysics ? static_cast<FAnimNode_Base*>(&ToLocal) : &Input; }

	virtual void Initialize(UAnimInstance* Instance) override {
		auto* Source = CastChecked<UKawaiiCalibrationAnimInstance>(Instance);
		bPhysics = Source->bEnablePhysics;
		Physics = Source->PhysicsTemplate;
		ToCS.LocalPose.SetLinkNode(&Input);
		Physics.ComponentPose.SetLinkNode(&ToCS);
		ToLocal.ComponentPose.SetLinkNode(&Physics);
		FAnimInstanceProxy::Initialize(Instance);
	}

	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override {
		FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
		Input.Local = CastChecked<UKawaiiCalibrationAnimInstance>(Instance)->InputPose;
		if (bPhysics) {
			Physics.PreUpdate(Instance);
		}
	}
};

UKawaiiCalibrationAnimInstance::UKawaiiCalibrationAnimInstance() {
	bUseMultiThreadedAnimationUpdate = false;
	RootMotionMode = ERootMotionMode::NoRootMotionExtraction;
}

FAnimInstanceProxy* UKawaiiCalibrationAnimInstance::CreateAnimInstanceProxy() {
	return new FCalibrationAnimProxy(this);
}

void UKawaiiCalibrationAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) {
	delete Proxy;
}

int32 UKawaiiCalibrationAnimInstance::GetSimulatedBoneCount() const {
	return GetProxyOnGameThread<FCalibrationAnimProxy>().Physics.ModifyBones.Num();
}

uint64 UKawaiiCalibrationAnimInstance::GetSourceEvaluationSerial() const {
	return GetProxyOnGameThread<FCalibrationAnimProxy>().Input.EvaluationSerial;
}
