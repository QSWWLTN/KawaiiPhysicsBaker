#if WITH_DEV_AUTOMATION_TESTS

#include "KawaiiPhysicsBakeSharedCollisionSubsystem.h"
#include "Misc/AutomationTest.h"
#include "PreviewScene.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiPhysicsBakeSubsystemWorldTypesTest,
								 "KawaiiPhysicsBaker.Preview.SharedCollisionWorldTypes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiPhysicsBakeSubsystemWorldTypesTest::RunTest(const FString&) {
	const UKawaiiPhysicsBakeSharedCollisionSubsystem* Defaults = GetDefault<UKawaiiPhysicsBakeSharedCollisionSubsystem>();
	TestTrue(TEXT("Baking registry supports the preview world"), Defaults->DoesSupportWorldType(EWorldType::EditorPreview));
	TestFalse(TEXT("No duplicate registry in Game"), Defaults->DoesSupportWorldType(EWorldType::Game));
	TestFalse(TEXT("No duplicate registry in PIE"), Defaults->DoesSupportWorldType(EWorldType::PIE));
	TestFalse(TEXT("No duplicate registry in Editor"), Defaults->DoesSupportWorldType(EWorldType::Editor));
	TestFalse(TEXT("GamePreview is outside this adapter's scope"), Defaults->DoesSupportWorldType(EWorldType::GamePreview));
	TestFalse(TEXT("Inactive worlds are not supported"), Defaults->DoesSupportWorldType(EWorldType::Inactive));
	TestFalse(TEXT("Untyped worlds are not supported"), Defaults->DoesSupportWorldType(EWorldType::None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiPhysicsBakeSubsystemPreviewLookupTest,
								 "KawaiiPhysicsBaker.Preview.SharedCollisionLookupAndIsolation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiPhysicsBakeSubsystemPreviewLookupTest::RunTest(const FString&) {
	// Construct real preview worlds so the test covers subsystem creation and the
	// base-type lookup used by KawaiiPhysics, rather than just checking the enum.
	FPreviewScene First(FPreviewScene::ConstructionValues().SetEditor(true).SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	FPreviewScene Second(FPreviewScene::ConstructionValues().SetEditor(true).SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));

	UKawaiiPhysicsSharedCollisionSubsystem* Base = First.GetWorld()->GetSubsystem<UKawaiiPhysicsSharedCollisionSubsystem>();
	UKawaiiPhysicsBakeSharedCollisionSubsystem* Derived = First.GetWorld()->GetSubsystem<UKawaiiPhysicsBakeSharedCollisionSubsystem>();
	UKawaiiPhysicsSharedCollisionSubsystem* Other = Second.GetWorld()->GetSubsystem<UKawaiiPhysicsSharedCollisionSubsystem>();

	TestNotNull(TEXT("Preview initializes the baking subsystem"), Derived);
	TestTrue(TEXT("Runtime base lookup resolves the same baking registry"), Base != nullptr && Base == Derived);
	TestTrue(TEXT("Each preview has its own registry"), Other != nullptr && Other != Base);
	TestTrue(TEXT("Resolved registry belongs to the correct world"), Base && Base->GetWorld() == First.GetWorld());
	return true;
}

#endif
