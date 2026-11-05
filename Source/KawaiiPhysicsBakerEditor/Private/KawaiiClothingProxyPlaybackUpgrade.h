#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

class UKawaiiClothingProxyMotion;

namespace KawaiiClothingProxyUpgrade {
	/** Editor metadata only. The resolved soft path is persisted on the runtime motion asset. */
	struct FBodyPlaybackCandidate
	{
		FSoftObjectPath Animation;
		FSoftObjectPath Source;
		FGuid Group;
		FName Part;
	};

	bool ResolveBodyPlayback(const UKawaiiClothingProxyMotion& Motion,
							 const TArray<FBodyPlaybackCandidate>& Candidates,
							 FSoftObjectPath& OutAnimation,
							 FString& Error);
	bool HasSelectedSets();
	void UpgradeSelectedSets();
}
