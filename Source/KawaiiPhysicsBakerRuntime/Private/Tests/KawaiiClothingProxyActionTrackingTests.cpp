#include "KawaiiClothingProxyActionTracking.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/BlendSpace.h"
#include "Components/SkeletalMeshComponent.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataController.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiProxyScopedActionTrackingTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutomaticTracking.ScopeAndCurrentTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiProxyScopedActionTrackingTest::RunTest(const FString&) {
	TStrongObjectPtr<UAnimSequence> First(NewObject<UAnimSequence>());
	TStrongObjectPtr<UAnimSequence> Second(NewObject<UAnimSequence>());
	FAnimInstanceProxy Proxy;
	FAnimationUpdateSharedContext Shared;
	FAnimationUpdateContext Context(&Proxy, 1.f / 60.f, &Shared);
	int32 Owner = 0;
	float FirstTime = 0.2f;
	float SecondTime = 0.6f;
	FMarkerTickRecord FirstMarker, SecondMarker;
	FAnimTickRecord FirstRecord(First.Get(), true, 1.f, false, 0.3f, FirstTime, FirstMarker);
	FAnimTickRecord SecondRecord(Second.Get(), false, 0.f, true, 0.2f, SecondTime, SecondMarker);
	{
		UE::Anim::TScopedGraphMessage<KawaiiClothingProxy::FActionTrackingScope> Scope(Context, Context, &Owner, 7u);
		FirstRecord.GatherContextData(Context);
		SecondRecord.GatherContextData(Context);
	}
	// Populate the completed read buffer directly, independently of engine tick behavior.
	auto& Records = const_cast<TArray<FAnimTickRecord>&>(Proxy.GetUngroupedActivePlayersRead());
	Records = {FirstRecord, SecondRecord};
	FAnimTickRecord Unrelated(Second.Get(), true, 1.f, false, 1.f, SecondTime, SecondMarker);
	Records.Add(Unrelated);
	FirstTime = 0.4f; // synchronization updates the accumulator after graph Update
	TArray<KawaiiClothingProxy::FTrackedAction> Actions;
	FString Error;
	KawaiiClothingProxy::FTrackedMontageSnapshot Snapshot;
	Snapshot.bCaptured = true;
	TestTrue(
		TEXT("Tracks only the tagged full-body branch including an evaluator"),
		KawaiiClothingProxy::CollectActions(Proxy, &Owner, 7u, 0.5f, NAME_None, Actions, Error, &Snapshot)
	);
	if (TestEqual(TEXT("Unrelated players are excluded"), Actions.Num(), 2)) {
		TestEqual(TEXT("Time is read after synchronization, not captured during update"), Actions[0].Time, 0.4f);
		TestEqual(TEXT("Weight is relative to the tracked branch"), Actions[0].Weight, 0.6f);
		TestEqual(TEXT("Evaluator explicit time is preserved"), Actions[1].Time, SecondTime);
		TestTrue(TEXT("Asset identity is retained"), Actions[0].Animation == FSoftObjectPath(First.Get()));
	}
	TestFalse(
		TEXT("Previous update tags cannot be reused"),
		KawaiiClothingProxy::CollectActions(Proxy, &Owner, 8u, 0.5f, NAME_None, Actions, Error, &Snapshot)
	);
	TestTrue(TEXT("Failure clears stale results"), Actions.IsEmpty());
	Records.RemoveAt(1);
	TestFalse(
		TEXT("Missing cached-pose contributions are not silently normalized"),
		KawaiiClothingProxy::CollectActions(Proxy, &Owner, 7u, 0.5f, NAME_None, Actions, Error, &Snapshot)
	);
	Records = {FirstRecord, SecondRecord};
	Records[0].bRequestedInertialization = true;
	TestFalse(
		TEXT("Inertialized tracks require explicit handling"),
		KawaiiClothingProxy::CollectActions(Proxy, &Owner, 7u, 0.5f, NAME_None, Actions, Error, &Snapshot)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiProxyBlendSpaceTrackingTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutomaticTracking.BlendSpaceSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiProxyBlendSpaceTrackingTest::RunTest(const FString&) {
	TStrongObjectPtr<UAnimSequence> Walk(NewObject<UAnimSequence>());
	TStrongObjectPtr<UAnimSequence> Run(NewObject<UAnimSequence>());
	TStrongObjectPtr<UBlendSpace> BlendSpace(NewObject<UBlendSpace>());
	TArray<FBlendSampleData> Samples;
	FBlendSampleData& WalkSample = Samples.AddDefaulted_GetRef();
	WalkSample.Animation = Walk.Get();
	WalkSample.TotalWeight = 0.25f;
	WalkSample.Time = 0.75f;
	FBlendSampleData& RunSample = Samples.AddDefaulted_GetRef();
	RunSample.Animation = Run.Get();
	RunSample.TotalWeight = 0.75f;
	RunSample.Time = 0.35f;
	FAnimTickRecord Record;
	Record.SourceAsset = BlendSpace.Get();
	Record.EffectiveBlendWeight = 1.f;
	Record.BlendSpace.BlendSampleDataCache = &Samples;
	FAnimInstanceProxy Proxy;
	FAnimationUpdateSharedContext Shared;
	FAnimationUpdateContext Context(&Proxy, 1.f / 60.f, &Shared);
	{
		UE::Anim::TScopedGraphMessage<KawaiiClothingProxy::FActionTrackingScope> Scope(Context, Context, &Proxy, 1u);
		Record.GatherContextData(Context);
	}
	const_cast<TArray<FAnimTickRecord>&>(Proxy.GetUngroupedActivePlayersRead()).Add(Record);
	TArray<KawaiiClothingProxy::FTrackedAction> Actions;
	FString Error;
	KawaiiClothingProxy::FTrackedMontageSnapshot Snapshot;
	Snapshot.bCaptured = true;
	TestTrue(
		TEXT("Expands BlendSpace samples into sequence-local clocks"),
		KawaiiClothingProxy::CollectActions(Proxy, &Proxy, 1u, 1.f, NAME_None, Actions, Error, &Snapshot)
	);
	if (TestEqual(TEXT("Both sequence samples are retained"), Actions.Num(), 2)) {
		TestEqual(TEXT("Walk time is sequence-local, not BlendSpace normalized time"), Actions[0].Time, 0.75f);
		TestEqual(TEXT("Run uses its own sample time"), Actions[1].Time, 0.35f);
		TestEqual(TEXT("Sample weight survives"), Actions[1].Weight, 0.75f);
	}
	Samples[0].PerBoneBlendData.Add(0.5f);
	TestFalse(
		TEXT("Per-bone sample blending cannot be approximated by scalar weights"),
		KawaiiClothingProxy::CollectActions(Proxy, &Proxy, 1u, 1.f, NAME_None, Actions, Error, &Snapshot)
	);
	TestTrue(TEXT("Unsupported sample cannot leave previous results"), Actions.IsEmpty());
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiProxyMontageTrackingTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutomaticTracking.MontageSegmentTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiProxyMontageTrackingTest::RunTest(const FString&) {
	TStrongObjectPtr<UAnimSequence> Idle(NewObject<UAnimSequence>());
	TStrongObjectPtr<UAnimSequence> Attack(NewObject<UAnimSequence>());
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>());
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}
	Attack->SetSkeleton(Skeleton.Get());
	IAnimationDataController& Controller = Attack->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Montage tracking test fixture")), false);
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
	TStrongObjectPtr<UAnimMontage> Montage(NewObject<UAnimMontage>());
	const FName SlotName(TEXT("ProxyFullBody"));
	FSlotAnimationTrack& Slot = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	Slot.SlotName = SlotName;
	FAnimSegment& Segment = Slot.AnimTrack.AnimSegments.AddDefaulted_GetRef();
	Segment.SetAnimReference(Attack.Get());
	Segment.StartPos = 2.f;
	Segment.AnimStartTime = 0.1f;
	Segment.AnimEndTime = 0.9f;
	Segment.AnimPlayRate = 2.f;
	Segment.LoopingCount = 2;
	FAnimInstanceProxy Proxy;
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	FAnimMontageInstance MontageInstance;
	MontageInstance.Montage = Montage.Get();
	MontageInstance.SetPosition(2.5f);
	MontageInstance.SetWeight(0.4f);
	Instance->MontageInstances.Add(&MontageInstance);
	KawaiiClothingProxy::FTrackedMontageSnapshot Snapshot;
	KawaiiClothingProxy::CaptureMontages(*Instance, Snapshot);
	Instance->MontageInstances.Reset();
	FAnimationUpdateSharedContext Shared;
	FAnimationUpdateContext Context(&Proxy, 1.f / 60.f, &Shared);
	float IdleTime = 0.7f;
	FMarkerTickRecord Marker;
	FAnimTickRecord IdleRecord(Idle.Get(), true, 1.f, false, 0.6f, IdleTime, Marker);
	{
		UE::Anim::TScopedGraphMessage<KawaiiClothingProxy::FActionTrackingScope> Scope(Context, Context, &Proxy, 3u);
		IdleRecord.GatherContextData(Context);
	}
	const_cast<TArray<FAnimTickRecord>&>(Proxy.GetUngroupedActivePlayersRead()).Add(IdleRecord);
	TArray<KawaiiClothingProxy::FTrackedAction> Actions;
	FString Error;
	TestFalse(
		TEXT("A montage cannot silently be ignored without a configured slot"),
		KawaiiClothingProxy::CollectActions(Proxy, &Proxy, 3u, 1.f, NAME_None, Actions, Error, &Snapshot)
	);
	TestTrue(
		TEXT("The configured full-body slot includes its source and montage blend"),
		KawaiiClothingProxy::CollectActions(Proxy, &Proxy, 3u, 1.f, SlotName, Actions, Error, &Snapshot)
	);
	if (TestEqual(TEXT("Idle and attack both contribute"), Actions.Num(), 2)) {
		TestTrue(
			FString::Printf(TEXT("Trim, speed and looping yield 0.3s (actual %.8f)"), Actions[1].Time),
			FMath::IsNearlyEqual(Actions[1].Time, 0.3f, 1.e-4f)
		);
		TestEqual(TEXT("Montage blend weight is retained"), Actions[1].Weight, 0.4f);
		TestTrue(TEXT("Mapping identity uses the segment sequence"), Actions[1].Animation == FSoftObjectPath(Attack.Get()));
	}
	Segment.AnimPlayRate = -2.f;
	Instance->MontageInstances.Add(&MontageInstance);
	KawaiiClothingProxy::CaptureMontages(*Instance, Snapshot);
	Instance->MontageInstances.Reset();
	TestTrue(TEXT("Reversed segments remain supported"), KawaiiClothingProxy::CollectActions(Proxy, &Proxy, 3u, 1.f, SlotName, Actions, Error, &Snapshot));
	if (Actions.Num() == 2) {
		TestTrue(
			FString::Printf(TEXT("Reverse sequence-local time is 0.7s (actual %.8f)"), Actions[1].Time),
			FMath::IsNearlyEqual(Actions[1].Time, 0.7f, 1.e-4f)
		);
	}
	return true;
}

#endif
#endif
