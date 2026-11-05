#include "KawaiiClothingProxyAnimSubsystem.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"

void FAnimSubsystem_KawaiiClothingProxy::OnPreUpdate_GameThread(FAnimSubsystemUpdateContext& Context) const {
	auto& InstanceData = static_cast<FAnimSubsystemInstance_KawaiiClothingProxy&>(Context.InnerContext.SubsystemInstance);
	if (Context.AnimInstance) {
		// UE has just advanced montages and created MontageEvaluationData. Capture now,
		// before the event graph can change playback for the next evaluation snapshot.
		KawaiiClothingProxy::CaptureMontages(*Context.AnimInstance, InstanceData.MontageSnapshot);
	} else {
		InstanceData.MontageSnapshot = KawaiiClothingProxy::FTrackedMontageSnapshot();
	}
}

const KawaiiClothingProxy::FTrackedMontageSnapshot* KawaiiClothingProxy::GetMontageSnapshot(FAnimInstanceProxy& Proxy) {
	UAnimInstance* Instance = Cast<UAnimInstance>(Proxy.GetAnimInstanceObject());
	FAnimSubsystemInstance_KawaiiClothingProxy* Data = Instance ? Instance->FindSubsystem<FAnimSubsystemInstance_KawaiiClothingProxy>() : nullptr;
	return Data ? &Data->MontageSnapshot : nullptr;
}
