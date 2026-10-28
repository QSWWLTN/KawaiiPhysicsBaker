#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "KawaiiPhysicsIKEditorLibrary.generated.h"

class UEdGraphNode;
class UBlueprint;

/** Small editor scripting bridge for the native cached-pose reference. */
UCLASS()
class UKawaiiPhysicsIKEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Bind a Use Cached Pose to a Save Cached Pose in the same animation graph. */
	UFUNCTION(BlueprintCallable, Category = "Kawaii Physics|Editor")
		static bool BindUseCachedPose(UEdGraphNode* UseNode, UEdGraphNode* SaveNode);

	/** Read the compiler result that UBlueprint does not expose to Python. */
	UFUNCTION(BlueprintPure, Category = "Kawaii Physics|Editor")
		static FString GetBlueprintStatus(const UBlueprint* Blueprint);

	UFUNCTION(BlueprintPure, Category = "Kawaii Physics|Editor")
		static bool IsBlueprintUpToDate(const UBlueprint* Blueprint);
};
