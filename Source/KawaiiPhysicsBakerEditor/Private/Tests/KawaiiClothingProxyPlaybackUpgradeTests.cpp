#include "Animation/AnimSequence.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiClothingProxyPlaybackUpgrade.h"
#include "KawaiiClothingProxyMotion.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiProxyPlaybackUpgradeTest,
	"KawaiiPhysicsBaker.ClothingProxy.PlaybackUpgradeIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiProxyPlaybackUpgradeTest::RunTest(const FString&) {
	using namespace KawaiiClothingProxyUpgrade;
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Motion(NewObject<UKawaiiClothingProxyMotion>());
	Motion->BakeGroup = FGuid::NewGuid();
	Motion->SourceAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(TEXT("/Game/Test/Walk.Walk")));
	FBodyPlaybackCandidate Match;
	Match.Animation = FSoftObjectPath(TEXT("/Game/Test/Walk_Main.Walk_Main"));
	Match.Source = Motion->SourceAnimation.ToSoftObjectPath();
	Match.Group = Motion->BakeGroup;
	Match.Part = TEXT("Main");
	FBodyPlaybackCandidate WrongPart = Match;
	WrongPart.Part = TEXT("Base");
	FBodyPlaybackCandidate WrongGroup = Match;
	WrongGroup.Group = FGuid::NewGuid();
	FBodyPlaybackCandidate WrongSource = Match;
	WrongSource.Source = FSoftObjectPath(TEXT("/Game/Test/Run.Run"));
	FSoftObjectPath Result;
	FString Error;
	TestTrue(
		TEXT("Group, source and Main identify the exact body playback"),
		ResolveBodyPlayback(*Motion.Get(), {WrongPart, WrongGroup, Match, WrongSource}, Result, Error)
	);
	TestTrue(TEXT("Correct path survives unrelated metadata candidates"), Result == Match.Animation);
	TestFalse(TEXT("No fallback to same-name or other bake take"), ResolveBodyPlayback(*Motion.Get(), {WrongPart, WrongGroup, WrongSource}, Result, Error));
	TestTrue(TEXT("Failure clears previous match"), Result.IsNull() && !Error.IsEmpty());
	FBodyPlaybackCandidate DuplicateTake = Match;
	DuplicateTake.Animation = FSoftObjectPath(TEXT("/Game/Test/CopiedMain.CopiedMain"));
	TestFalse(TEXT("Two distinct assets with identical metadata are ambiguous"), ResolveBodyPlayback(*Motion.Get(), {Match, DuplicateTake}, Result, Error));
	TestTrue(TEXT("Ambiguous mapping cannot retain stale output"), Result.IsNull());
	TestTrue(TEXT("Duplicate registry entries for one asset are harmless"), ResolveBodyPlayback(*Motion.Get(), {Match, Match}, Result, Error));
	Motion->BakeGroup.Invalidate();
	TestFalse(TEXT("Missing source metadata requires rebake or repair"), ResolveBodyPlayback(*Motion.Get(), {Match}, Result, Error));
	return true;
}
#endif
