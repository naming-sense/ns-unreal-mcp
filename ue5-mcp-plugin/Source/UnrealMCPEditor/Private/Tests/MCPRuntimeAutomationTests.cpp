#if WITH_DEV_AUTOMATION_TESTS

#include "MCPCommandRouterSubsystem.h"
#include "MCPObjectUtils.h"

#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace
{
	bool ParseJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool ExecuteMCPRequest(const FString& RequestJson, FString& OutResponseJson, bool& bOutSuccess)
	{
		if (GEditor == nullptr)
		{
			bOutSuccess = false;
			return false;
		}

		UMCPCommandRouterSubsystem* Router = GEditor->GetEditorSubsystem<UMCPCommandRouterSubsystem>();
		if (Router == nullptr)
		{
			bOutSuccess = false;
			return false;
		}

		OutResponseJson = Router->ExecuteRequestJson(RequestJson, bOutSuccess);
		return true;
	}

	FString MakeRequestEnvelope(const FString& ToolName, const FString& ParamsJson, const bool bDryRun = true)
	{
		const FString RequestToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString ToolKey = ToolName.Replace(TEXT("."), TEXT("-"));
		const FString IdempotencyKey = FString::Printf(TEXT("auto-%s-%s"), *ToolKey, *RequestToken);
		return FString::Printf(
			TEXT("{\"protocol\":\"unreal-mcp/1.0\",\"request_id\":\"auto-test-%s\",\"session_id\":\"auto-session\",\"tool\":\"%s\",\"params\":%s,\"context\":{\"deterministic\":true,\"dry_run\":%s,\"idempotency_key\":\"%s\"}}"),
			*RequestToken,
			*ToolName,
			*ParamsJson,
			bDryRun ? TEXT("true") : TEXT("false"),
			*IdempotencyKey);
	}

	bool JsonArrayContainsString(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& ExpectedValue)
	{
		if (Values == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString TextValue;
			if (Value.IsValid() && Value->TryGetString(TextValue) && TextValue.Equals(ExpectedValue, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		return false;
	}

	UMaterialInstanceConstant* GetOrCreateRuntimeMaterialInstance()
	{
		const FString PackageName = TEXT("/Game/MCPRuntimeTests/MI_MCPRuntimeTest");
		const FName AssetName(TEXT("MI_MCPRuntimeTest"));

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package == nullptr)
		{
			Package = CreatePackage(*PackageName);
		}

		if (Package == nullptr)
		{
			return nullptr;
		}

		UMaterialInstanceConstant* MaterialInstance = FindObject<UMaterialInstanceConstant>(Package, *AssetName.ToString());
		if (MaterialInstance != nullptr)
		{
			return MaterialInstance;
		}

		MaterialInstance = NewObject<UMaterialInstanceConstant>(Package, AssetName, RF_Public | RF_Standalone);
		if (MaterialInstance == nullptr)
		{
			return nullptr;
		}

		UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
		if (ParentMaterial != nullptr)
		{
			MaterialInstance->SetParentEditorOnly(ParentMaterial);
		}

		MaterialInstance->PostEditChange();
		return MaterialInstance;
	}

	UWidgetBlueprint* GetOrCreateRuntimeWidgetBlueprint()
	{
		const FString PackageName = TEXT("/Game/MCPRuntimeTests/WBP_MCPRuntimeTest");
		const FName AssetName(TEXT("WBP_MCPRuntimeTest"));

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package == nullptr)
		{
			Package = CreatePackage(*PackageName);
		}

		if (Package == nullptr)
		{
			return nullptr;
		}

		UWidgetBlueprint* WidgetBlueprint = FindObject<UWidgetBlueprint>(Package, *AssetName.ToString());
		if (WidgetBlueprint == nullptr)
		{
			WidgetBlueprint = Cast<UWidgetBlueprint>(
				FKismetEditorUtilities::CreateBlueprint(
					UUserWidget::StaticClass(),
					Package,
					AssetName,
					BPTYPE_Normal,
					UWidgetBlueprint::StaticClass(),
					UWidgetBlueprintGeneratedClass::StaticClass(),
					FName(TEXT("MCPRuntimeAutomation"))));
		}

		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			return WidgetBlueprint;
		}

		UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
		if (RootCanvas == nullptr)
		{
			RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
			WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		}

		if (RootCanvas == nullptr)
		{
			return WidgetBlueprint;
		}

		UTextBlock* TextWidget = Cast<UTextBlock>(WidgetBlueprint->WidgetTree->FindWidget(TEXT("RuntimeTextWidget")));
		if (TextWidget == nullptr)
		{
			TextWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("RuntimeTextWidget"));
			RootCanvas->AddChild(TextWidget);
		}

		return WidgetBlueprint;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolsListAutomationTest,
	"UnrealMCP.Runtime.ToolsList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPToolsListAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FString ResponseJson;
	bool bSuccess = false;
	const FString RequestJson = MakeRequestEnvelope(TEXT("tools.list"), TEXT("{\"include_schemas\":false}"));
	TestTrue(TEXT("Execute tools.list request"), ExecuteMCPRequest(RequestJson, ResponseJson, bSuccess));
	TestTrue(TEXT("tools.list status should be success"), bSuccess);

	TSharedPtr<FJsonObject> ResponseObject;
	TestTrue(TEXT("Parse tools.list response"), ParseJsonObject(ResponseJson, ResponseObject));

	FString Status;
	TestTrue(TEXT("tools.list response has status"), ResponseObject->TryGetStringField(TEXT("status"), Status));
	TestEqual(TEXT("tools.list response status"), Status, FString(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ResultObject = nullptr;
	TestTrue(TEXT("tools.list response has result"), ResponseObject->TryGetObjectField(TEXT("result"), ResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	TestTrue(TEXT("tools.list result has tools"), (*ResultObject)->TryGetArrayField(TEXT("tools"), Tools));

	bool bFoundObjectInspect = false;
	bool bFoundWorldOutliner = false;
	bool bFoundAssetFind = false;
	bool bFoundAssetLoad = false;
	bool bFoundAssetSave = false;
	bool bFoundAssetImport = false;
	bool bFoundAssetCreate = false;
	bool bFoundBlueprintClassCreate = false;
	bool bFoundAssetDuplicate = false;
	bool bFoundAssetRename = false;
	bool bFoundAssetDelete = false;
	bool bFoundSettingsProjectGet = false;
	bool bFoundSettingsProjectPatch = false;
	bool bFoundSettingsProjectApply = false;
	bool bFoundSettingsGameModeGet = false;
	bool bFoundSettingsGameModeSetDefault = false;
	bool bFoundSettingsGameModeCompose = false;
	bool bFoundSettingsGameModeSetMapOverride = false;
	bool bFoundSettingsGameModeRemoveMapOverride = false;
	bool bFoundMatParamsGet = false;
	bool bFoundNiagaraStackList = false;
	bool bFoundUMGBlueprintCreate = false;
	bool bFoundUMGBlueprintPatch = false;
	bool bFoundUMGBlueprintReparent = false;
	bool bFoundUMGTreeGet = false;
	bool bFoundUMGWidgetClassList = false;
	bool bFoundUMGWidgetAdd = false;
	bool bFoundUMGWidgetRemove = false;
	bool bFoundUMGWidgetReparent = false;
	for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
	{
		if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object)
		{
			continue;
		}

		FString Name;
		if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), Name))
		{
			bFoundObjectInspect |= Name == TEXT("object.inspect");
			bFoundWorldOutliner |= Name == TEXT("world.outliner.list");
			bFoundAssetFind |= Name == TEXT("asset.find");
			bFoundAssetLoad |= Name == TEXT("asset.load");
			bFoundAssetSave |= Name == TEXT("asset.save");
			bFoundAssetImport |= Name == TEXT("asset.import");
			bFoundAssetCreate |= Name == TEXT("asset.create");
			bFoundBlueprintClassCreate |= Name == TEXT("blueprint.class.create");
			bFoundAssetDuplicate |= Name == TEXT("asset.duplicate");
			bFoundAssetRename |= Name == TEXT("asset.rename");
			bFoundAssetDelete |= Name == TEXT("asset.delete");
			bFoundSettingsProjectGet |= Name == TEXT("settings.project.get");
			bFoundSettingsProjectPatch |= Name == TEXT("settings.project.patch");
			bFoundSettingsProjectApply |= Name == TEXT("settings.project.apply");
			bFoundSettingsGameModeGet |= Name == TEXT("settings.gamemode.get");
			bFoundSettingsGameModeSetDefault |= Name == TEXT("settings.gamemode.set_default");
			bFoundSettingsGameModeCompose |= Name == TEXT("settings.gamemode.compose");
			bFoundSettingsGameModeSetMapOverride |= Name == TEXT("settings.gamemode.set_map_override");
			bFoundSettingsGameModeRemoveMapOverride |= Name == TEXT("settings.gamemode.remove_map_override");
			bFoundMatParamsGet |= Name == TEXT("mat.instance.params.get");
			bFoundNiagaraStackList |= Name == TEXT("niagara.stack.list");
			bFoundUMGBlueprintCreate |= Name == TEXT("umg.blueprint.create");
			bFoundUMGBlueprintPatch |= Name == TEXT("umg.blueprint.patch");
			bFoundUMGBlueprintReparent |= Name == TEXT("umg.blueprint.reparent");
			bFoundUMGWidgetClassList |= Name == TEXT("umg.widget.class.list");
			bFoundUMGTreeGet |= Name == TEXT("umg.tree.get");
			bFoundUMGWidgetAdd |= Name == TEXT("umg.widget.add");
			bFoundUMGWidgetRemove |= Name == TEXT("umg.widget.remove");
			bFoundUMGWidgetReparent |= Name == TEXT("umg.widget.reparent");
		}
		}

	TestTrue(TEXT("tools.list contains object.inspect"), bFoundObjectInspect);
	TestTrue(TEXT("tools.list contains world.outliner.list"), bFoundWorldOutliner);
	TestTrue(TEXT("tools.list contains asset.find"), bFoundAssetFind);
	TestTrue(TEXT("tools.list contains asset.load"), bFoundAssetLoad);
	TestTrue(TEXT("tools.list contains asset.save"), bFoundAssetSave);
	TestTrue(TEXT("tools.list contains asset.import"), bFoundAssetImport);
	TestTrue(TEXT("tools.list contains asset.create"), bFoundAssetCreate);
	TestTrue(TEXT("tools.list contains blueprint.class.create"), bFoundBlueprintClassCreate);
	TestTrue(TEXT("tools.list contains asset.duplicate"), bFoundAssetDuplicate);
	TestTrue(TEXT("tools.list contains asset.rename"), bFoundAssetRename);
	TestTrue(TEXT("tools.list contains asset.delete"), bFoundAssetDelete);
	TestTrue(TEXT("tools.list contains settings.project.get"), bFoundSettingsProjectGet);
	TestTrue(TEXT("tools.list contains settings.project.patch"), bFoundSettingsProjectPatch);
	TestTrue(TEXT("tools.list contains settings.project.apply"), bFoundSettingsProjectApply);
	TestTrue(TEXT("tools.list contains settings.gamemode.get"), bFoundSettingsGameModeGet);
	TestTrue(TEXT("tools.list contains settings.gamemode.set_default"), bFoundSettingsGameModeSetDefault);
	TestTrue(TEXT("tools.list contains settings.gamemode.compose"), bFoundSettingsGameModeCompose);
	TestTrue(TEXT("tools.list contains settings.gamemode.set_map_override"), bFoundSettingsGameModeSetMapOverride);
	TestTrue(TEXT("tools.list contains settings.gamemode.remove_map_override"), bFoundSettingsGameModeRemoveMapOverride);
	TestTrue(TEXT("tools.list contains mat.instance.params.get"), bFoundMatParamsGet);
	TestTrue(TEXT("tools.list contains niagara.stack.list"), bFoundNiagaraStackList);
	TestTrue(TEXT("tools.list contains umg.blueprint.create"), bFoundUMGBlueprintCreate);
	TestTrue(TEXT("tools.list contains umg.blueprint.patch"), bFoundUMGBlueprintPatch);
	TestTrue(TEXT("tools.list contains umg.blueprint.reparent"), bFoundUMGBlueprintReparent);
	TestTrue(TEXT("tools.list contains umg.widget.class.list"), bFoundUMGWidgetClassList);
	TestTrue(TEXT("tools.list contains umg.tree.get"), bFoundUMGTreeGet);
	TestTrue(TEXT("tools.list contains umg.widget.add"), bFoundUMGWidgetAdd);
	TestTrue(TEXT("tools.list contains umg.widget.remove"), bFoundUMGWidgetRemove);
	TestTrue(TEXT("tools.list contains umg.widget.reparent"), bFoundUMGWidgetReparent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPSystemHealthTelemetryAutomationTest,
	"UnrealMCP.Runtime.SystemHealthTelemetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPSystemHealthTelemetryAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FString ToolsResponseJson;
	bool bToolsSuccess = false;
	const FString ToolsRequestJson = MakeRequestEnvelope(TEXT("tools.list"), TEXT("{\"include_schemas\":false}"));
	TestTrue(TEXT("Execute tools.list request for telemetry warmup"), ExecuteMCPRequest(ToolsRequestJson, ToolsResponseJson, bToolsSuccess));
	TestTrue(TEXT("tools.list warmup should be success"), bToolsSuccess);

	FString HealthResponseJson;
	bool bHealthSuccess = false;
	const FString HealthRequestJson = MakeRequestEnvelope(TEXT("system.health"), TEXT("{}"));
	TestTrue(TEXT("Execute system.health request"), ExecuteMCPRequest(HealthRequestJson, HealthResponseJson, bHealthSuccess));
	TestTrue(TEXT("system.health status should be success"), bHealthSuccess);

	TSharedPtr<FJsonObject> HealthResponseObject;
	TestTrue(TEXT("Parse system.health response"), ParseJsonObject(HealthResponseJson, HealthResponseObject));

	const TSharedPtr<FJsonObject>* ResultObject = nullptr;
	TestTrue(TEXT("system.health response has result"), HealthResponseObject->TryGetObjectField(TEXT("result"), ResultObject));

	const TSharedPtr<FJsonObject>* EditorStateObject = nullptr;
	TestTrue(TEXT("system.health result has editor_state"), (*ResultObject)->TryGetObjectField(TEXT("editor_state"), EditorStateObject));

	const TSharedPtr<FJsonObject>* EventStreamObject = nullptr;
	TestTrue(TEXT("system.health editor_state has event_stream"), (*EditorStateObject)->TryGetObjectField(TEXT("event_stream"), EventStreamObject));

	bool bEventStreamSupported = false;
	TestTrue(TEXT("event_stream has supported field"), (*EventStreamObject)->TryGetBoolField(TEXT("supported"), bEventStreamSupported));
	TestTrue(TEXT("event_stream should be supported"), bEventStreamSupported);

	double TotalEventCount = 0.0;
	TestTrue(TEXT("event_stream has total_emitted_event_count"), (*EventStreamObject)->TryGetNumberField(TEXT("total_emitted_event_count"), TotalEventCount));
	TestTrue(TEXT("event_stream emits at least one event"), TotalEventCount > 0.0);

	const TArray<TSharedPtr<FJsonValue>>* RecentEvents = nullptr;
	TestTrue(TEXT("event_stream has recent_events"), (*EventStreamObject)->TryGetArrayField(TEXT("recent_events"), RecentEvents));
	TestTrue(TEXT("event_stream recent_events should not be empty"), RecentEvents != nullptr && RecentEvents->Num() > 0);

	const TSharedPtr<FJsonObject>* ObservabilityObject = nullptr;
	TestTrue(TEXT("system.health editor_state has observability"), (*EditorStateObject)->TryGetObjectField(TEXT("observability"), ObservabilityObject));

	const TSharedPtr<FJsonObject>* TransportObject = nullptr;
	TestTrue(TEXT("system.health editor_state has event_stream_transport"), (*EditorStateObject)->TryGetObjectField(TEXT("event_stream_transport"), TransportObject));

	bool bTransportEnabled = false;
	TestTrue(TEXT("event_stream_transport has enabled"), (*TransportObject)->TryGetBoolField(TEXT("enabled"), bTransportEnabled));
	TestTrue(TEXT("event_stream_transport should be enabled"), bTransportEnabled);

	bool bTransportListening = false;
	TestTrue(TEXT("event_stream_transport has listening"), (*TransportObject)->TryGetBoolField(TEXT("listening"), bTransportListening));
	TestTrue(TEXT("event_stream_transport should be listening"), bTransportListening);

	double TransportPort = 0.0;
	TestTrue(TEXT("event_stream_transport has port"), (*TransportObject)->TryGetNumberField(TEXT("port"), TransportPort));
	TestTrue(TEXT("event_stream_transport port should be positive"), TransportPort > 0.0);

	const TArray<TSharedPtr<FJsonValue>>* ToolMetrics = nullptr;
	TestTrue(TEXT("observability has tool_metrics"), (*ObservabilityObject)->TryGetArrayField(TEXT("tool_metrics"), ToolMetrics));
	TestTrue(TEXT("tool_metrics should not be empty"), ToolMetrics != nullptr && ToolMetrics->Num() > 0);

	bool bFoundToolsListMetric = false;
	for (const TSharedPtr<FJsonValue>& MetricValue : *ToolMetrics)
	{
		if (!MetricValue.IsValid() || MetricValue->Type != EJson::Object)
		{
			continue;
		}

		FString ToolName;
		double TotalRequests = 0.0;
		MetricValue->AsObject()->TryGetStringField(TEXT("tool"), ToolName);
		MetricValue->AsObject()->TryGetNumberField(TEXT("total_requests"), TotalRequests);
		if (ToolName == TEXT("tools.list") && TotalRequests >= 1.0)
		{
			bFoundToolsListMetric = true;
			break;
		}
	}
	TestTrue(TEXT("observability contains tools.list metric"), bFoundToolsListMetric);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPWorldOutlinerAutomationTest,
	"UnrealMCP.Runtime.WorldOutlinerList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPWorldOutlinerAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	TestNotNull(TEXT("Create map for world test"), World);
	if (World == nullptr)
	{
		return false;
	}

	AActor* SpawnedActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	TestNotNull(TEXT("Spawn actor in world"), SpawnedActor);
	if (SpawnedActor == nullptr)
	{
		return false;
	}

	const FString TestActorLabel = TEXT("MCPRuntimeTestActor");
	SpawnedActor->SetActorLabel(TestActorLabel);

	FString ResponseJson;
	bool bSuccess = false;
	const FString RequestJson = MakeRequestEnvelope(
		TEXT("world.outliner.list"),
		TEXT("{\"limit\":200,\"include\":{\"class\":true,\"folder_path\":true,\"tags\":false,\"transform\":false},\"filters\":{\"name_glob\":\"MCPRuntimeTestActor\"}}"));
	TestTrue(TEXT("Execute world.outliner.list request"), ExecuteMCPRequest(RequestJson, ResponseJson, bSuccess));
	TestTrue(TEXT("world.outliner.list status should be success"), bSuccess);

	TSharedPtr<FJsonObject> ResponseObject;
	TestTrue(TEXT("Parse world.outliner.list response"), ParseJsonObject(ResponseJson, ResponseObject));
	const TSharedPtr<FJsonObject>* ResultObject = nullptr;
	TestTrue(TEXT("world.outliner.list response has result"), ResponseObject->TryGetObjectField(TEXT("result"), ResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("world.outliner.list result has nodes"), (*ResultObject)->TryGetArrayField(TEXT("nodes"), Nodes));

	bool bFoundActorNode = false;
	for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
	{
		if (!NodeValue.IsValid() || NodeValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
		FString NodeType;
		FString NodeName;
		NodeObject->TryGetStringField(TEXT("node_type"), NodeType);
		NodeObject->TryGetStringField(TEXT("name"), NodeName);
		if (NodeType == TEXT("actor") && NodeName == TestActorLabel)
		{
			bFoundActorNode = true;
			break;
		}
	}

	TestTrue(TEXT("world.outliner.list contains spawned actor"), bFoundActorNode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPObjectInspectAutomationTest,
	"UnrealMCP.Runtime.ObjectInspectActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPObjectInspectAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	TestNotNull(TEXT("Create map for object.inspect test"), World);
	if (World == nullptr)
	{
		return false;
	}

	AActor* SpawnedActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	TestNotNull(TEXT("Spawn actor for object.inspect"), SpawnedActor);
	if (SpawnedActor == nullptr)
	{
		return false;
	}

	SpawnedActor->SetActorLabel(TEXT("MCPInspectActor"));
	const FString ActorPath = MCPObjectUtils::BuildActorPath(SpawnedActor);

	const FString ParamsJson = FString::Printf(
		TEXT("{\"target\":{\"type\":\"actor\",\"path\":\"%s\"},\"filters\":{\"only_editable\":true},\"depth\":1}"),
		*ActorPath);

	FString ResponseJson;
	bool bSuccess = false;
	const FString RequestJson = MakeRequestEnvelope(TEXT("object.inspect"), ParamsJson);
	TestTrue(TEXT("Execute object.inspect request"), ExecuteMCPRequest(RequestJson, ResponseJson, bSuccess));
	TestTrue(TEXT("object.inspect status should be success"), bSuccess);

	TSharedPtr<FJsonObject> ResponseObject;
	TestTrue(TEXT("Parse object.inspect response"), ParseJsonObject(ResponseJson, ResponseObject));
	const TSharedPtr<FJsonObject>* ResultObject = nullptr;
	TestTrue(TEXT("object.inspect response has result"), ResponseObject->TryGetObjectField(TEXT("result"), ResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("object.inspect result has properties"), (*ResultObject)->TryGetArrayField(TEXT("properties"), Properties));
	TestTrue(TEXT("object.inspect returns editable properties"), Properties != nullptr && Properties->Num() > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAssetToolsAutomationTest,
	"UnrealMCP.Runtime.AssetTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPAssetToolsAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FString FindResponseJson;
	bool bFindSuccess = false;
	const FString FindRequestJson = MakeRequestEnvelope(
		TEXT("asset.find"),
		TEXT("{\"path_glob\":\"/Engine/EngineMaterials/*\",\"name_glob\":\"Default*\",\"limit\":20}"));
	TestTrue(TEXT("Execute asset.find request"), ExecuteMCPRequest(FindRequestJson, FindResponseJson, bFindSuccess));
	TestTrue(TEXT("asset.find status should be success"), bFindSuccess);

	TSharedPtr<FJsonObject> FindResponseObject;
	TestTrue(TEXT("Parse asset.find response"), ParseJsonObject(FindResponseJson, FindResponseObject));
	const TSharedPtr<FJsonObject>* FindResultObject = nullptr;
	TestTrue(TEXT("asset.find has result"), FindResponseObject->TryGetObjectField(TEXT("result"), FindResultObject));
	const TArray<TSharedPtr<FJsonValue>>* FoundAssets = nullptr;
	TestTrue(TEXT("asset.find result has assets"), (*FindResultObject)->TryGetArrayField(TEXT("assets"), FoundAssets));
	TestTrue(TEXT("asset.find returns at least one asset"), FoundAssets != nullptr && FoundAssets->Num() > 0);

	FString LoadResponseJson;
	bool bLoadSuccess = false;
	const FString LoadRequestJson = MakeRequestEnvelope(
		TEXT("asset.load"),
		TEXT("{\"object_path\":\"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial\"}"));
	TestTrue(TEXT("Execute asset.load request"), ExecuteMCPRequest(LoadRequestJson, LoadResponseJson, bLoadSuccess));
	TestTrue(TEXT("asset.load status should be success"), bLoadSuccess);

	TSharedPtr<FJsonObject> LoadResponseObject;
	TestTrue(TEXT("Parse asset.load response"), ParseJsonObject(LoadResponseJson, LoadResponseObject));
	const TSharedPtr<FJsonObject>* LoadResultObject = nullptr;
	TestTrue(TEXT("asset.load has result"), LoadResponseObject->TryGetObjectField(TEXT("result"), LoadResultObject));
	bool bLoaded = false;
	TestTrue(TEXT("asset.load result has loaded"), (*LoadResultObject)->TryGetBoolField(TEXT("loaded"), bLoaded));
	TestTrue(TEXT("asset.load loaded should be true"), bLoaded);

	UMaterialInstanceConstant* MaterialInstance = GetOrCreateRuntimeMaterialInstance();
	TestNotNull(TEXT("Create package source asset for asset.save"), MaterialInstance);
	if (MaterialInstance == nullptr)
	{
		return false;
	}

	const FString SaveRequestParams = FString::Printf(
		TEXT("{\"packages\":[\"%s\"],\"only_dirty\":true}"),
		*MaterialInstance->GetOutermost()->GetName());

	FString SaveResponseJson;
	bool bSaveSuccess = false;
	const FString SaveRequestJson = MakeRequestEnvelope(TEXT("asset.save"), SaveRequestParams);
	TestTrue(TEXT("Execute asset.save request"), ExecuteMCPRequest(SaveRequestJson, SaveResponseJson, bSaveSuccess));
	TestTrue(TEXT("asset.save status should be success"), bSaveSuccess);

	TSharedPtr<FJsonObject> SaveResponseObject;
	TestTrue(TEXT("Parse asset.save response"), ParseJsonObject(SaveResponseJson, SaveResponseObject));
	const TSharedPtr<FJsonObject>* SaveResultObject = nullptr;
	TestTrue(TEXT("asset.save has result"), SaveResponseObject->TryGetObjectField(TEXT("result"), SaveResultObject));
	const TArray<TSharedPtr<FJsonValue>>* SavedPackages = nullptr;
	TestTrue(TEXT("asset.save result has saved"), (*SaveResultObject)->TryGetArrayField(TEXT("saved"), SavedPackages));
	TestTrue(TEXT("asset.save reports at least one saved package"), SavedPackages != nullptr && SavedPackages->Num() > 0);

	const FString RuntimeFolder = TEXT("/Game/MCPRuntimeTests");
	const FString RuntimeSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString DuplicatedAssetName = FString::Printf(TEXT("MI_MCPRuntimeDuplicate_%s"), *RuntimeSuffix);
	const FString RenamedAssetName = FString::Printf(TEXT("MI_MCPRuntimeRenamed_%s"), *RuntimeSuffix);
	const FString CreatedAssetName = FString::Printf(TEXT("MI_MCPRuntimeCreated_%s"), *RuntimeSuffix);
	const FString DuplicatedObjectPath = FString::Printf(TEXT("%s/%s.%s"), *RuntimeFolder, *DuplicatedAssetName, *DuplicatedAssetName);
	const FString RenamedObjectPath = FString::Printf(TEXT("%s/%s.%s"), *RuntimeFolder, *RenamedAssetName, *RenamedAssetName);
	const FString CreatedObjectPath = FString::Printf(TEXT("%s/%s.%s"), *RuntimeFolder, *CreatedAssetName, *CreatedAssetName);

	FString DuplicateResponseJson;
	bool bDuplicateSuccess = false;
	const FString DuplicateRequestParams = FString::Printf(
		TEXT("{\"source_object_path\":\"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial\",\"dest_package_path\":\"%s\",\"dest_asset_name\":\"%s\",\"overwrite\":false}"),
		*RuntimeFolder,
		*DuplicatedAssetName);
	const FString DuplicateRequestJson = MakeRequestEnvelope(TEXT("asset.duplicate"), DuplicateRequestParams, false);
	TestTrue(TEXT("Execute asset.duplicate request"), ExecuteMCPRequest(DuplicateRequestJson, DuplicateResponseJson, bDuplicateSuccess));
	TestTrue(TEXT("asset.duplicate status should be success"), bDuplicateSuccess);

	FString RenameResponseJson;
	bool bRenameSuccess = false;
	const FString RenameRequestParams = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"new_package_path\":\"%s\",\"new_asset_name\":\"%s\",\"fixup_redirectors\":false}"),
		*DuplicatedObjectPath,
		*RuntimeFolder,
		*RenamedAssetName);
	const FString RenameRequestJson = MakeRequestEnvelope(TEXT("asset.rename"), RenameRequestParams, false);
	TestTrue(TEXT("Execute asset.rename request"), ExecuteMCPRequest(RenameRequestJson, RenameResponseJson, bRenameSuccess));
	TestTrue(TEXT("asset.rename status should be success"), bRenameSuccess);

	FString CreateResponseJson;
	bool bCreateSuccess = false;
	const FString CreateRequestParams = FString::Printf(
		TEXT("{\"package_path\":\"%s\",\"asset_name\":\"%s\",\"asset_class_path\":\"/Script/Engine.MaterialInstanceConstant\",\"overwrite\":false}"),
		*RuntimeFolder,
		*CreatedAssetName);
	const FString CreateRequestJson = MakeRequestEnvelope(TEXT("asset.create"), CreateRequestParams, false);
	TestTrue(TEXT("Execute asset.create request"), ExecuteMCPRequest(CreateRequestJson, CreateResponseJson, bCreateSuccess));
	TestTrue(TEXT("asset.create status should be success"), bCreateSuccess);

	FString LoadCreatedResponseJson;
	bool bLoadCreatedSuccess = false;
	const FString LoadCreatedRequestJson = MakeRequestEnvelope(
		TEXT("asset.load"),
		FString::Printf(TEXT("{\"object_path\":\"%s\"}"), *CreatedObjectPath),
		false);
	TestTrue(TEXT("Execute asset.load for created asset"), ExecuteMCPRequest(LoadCreatedRequestJson, LoadCreatedResponseJson, bLoadCreatedSuccess));
	TestTrue(TEXT("asset.load for created asset should be success"), bLoadCreatedSuccess);

	FString DeletePreviewResponseJson;
	bool bDeletePreviewSuccess = false;
	const FString DeletePreviewParams = FString::Printf(
		TEXT("{\"object_paths\":[\"%s\",\"%s\"],\"mode\":\"preview\",\"fail_if_referenced\":true}"),
		*RenamedObjectPath,
		*CreatedObjectPath);
	const FString DeletePreviewRequestJson = MakeRequestEnvelope(TEXT("asset.delete"), DeletePreviewParams, false);
	TestTrue(TEXT("Execute asset.delete preview request"), ExecuteMCPRequest(DeletePreviewRequestJson, DeletePreviewResponseJson, bDeletePreviewSuccess));
	TestTrue(TEXT("asset.delete preview should be success"), bDeletePreviewSuccess);

	TSharedPtr<FJsonObject> DeletePreviewResponseObject;
	TestTrue(TEXT("Parse asset.delete preview response"), ParseJsonObject(DeletePreviewResponseJson, DeletePreviewResponseObject));
	const TSharedPtr<FJsonObject>* DeletePreviewResultObject = nullptr;
	TestTrue(TEXT("asset.delete preview has result"), DeletePreviewResponseObject->TryGetObjectField(TEXT("result"), DeletePreviewResultObject));
	FString ConfirmToken;
	TestTrue(TEXT("asset.delete preview has confirm_token"), (*DeletePreviewResultObject)->TryGetStringField(TEXT("confirm_token"), ConfirmToken));
	TestFalse(TEXT("asset.delete preview token is not empty"), ConfirmToken.IsEmpty());

	FString DeleteApplyResponseJson;
	bool bDeleteApplySuccess = false;
	const FString DeleteApplyParams = FString::Printf(
		TEXT("{\"object_paths\":[\"%s\",\"%s\"],\"mode\":\"apply\",\"fail_if_referenced\":true,\"confirm_token\":\"%s\"}"),
		*RenamedObjectPath,
		*CreatedObjectPath,
		*ConfirmToken);
	const FString DeleteApplyRequestJson = MakeRequestEnvelope(TEXT("asset.delete"), DeleteApplyParams, false);
	TestTrue(TEXT("Execute asset.delete apply request"), ExecuteMCPRequest(DeleteApplyRequestJson, DeleteApplyResponseJson, bDeleteApplySuccess));
	TestTrue(TEXT("asset.delete apply should be success"), bDeleteApplySuccess);

	FString LoadDeletedResponseJson;
	bool bLoadDeletedSuccess = false;
	const FString LoadDeletedRequestJson = MakeRequestEnvelope(
		TEXT("asset.load"),
		FString::Printf(TEXT("{\"object_path\":\"%s\"}"), *CreatedObjectPath),
		false);
	TestTrue(TEXT("Execute asset.load for deleted asset"), ExecuteMCPRequest(LoadDeletedRequestJson, LoadDeletedResponseJson, bLoadDeletedSuccess));
	TestFalse(TEXT("asset.load for deleted asset should fail"), bLoadDeletedSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPMaterialToolsAutomationTest,
	"UnrealMCP.Runtime.MaterialTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPMaterialToolsAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UMaterialInstanceConstant* MaterialInstance = GetOrCreateRuntimeMaterialInstance();
	TestNotNull(TEXT("Create material instance for mat tools"), MaterialInstance);
	if (MaterialInstance == nullptr)
	{
		return false;
	}

	const FString MaterialPath = MaterialInstance->GetPathName();

	FString GetResponseJson;
	bool bGetSuccess = false;
	const FString GetParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"include_inherited\":true,\"kinds\":[\"scalar\",\"vector\"]}"),
		*MaterialPath);
	const FString GetRequestJson = MakeRequestEnvelope(TEXT("mat.instance.params.get"), GetParamsJson);
	TestTrue(TEXT("Execute mat.instance.params.get request"), ExecuteMCPRequest(GetRequestJson, GetResponseJson, bGetSuccess));
	TestTrue(TEXT("mat.instance.params.get status should be success"), bGetSuccess);

	TSharedPtr<FJsonObject> GetResponseObject;
	TestTrue(TEXT("Parse mat.instance.params.get response"), ParseJsonObject(GetResponseJson, GetResponseObject));
	const TSharedPtr<FJsonObject>* GetResultObject = nullptr;
	TestTrue(TEXT("mat.instance.params.get has result"), GetResponseObject->TryGetObjectField(TEXT("result"), GetResultObject));
	const TArray<TSharedPtr<FJsonValue>>* GetParams = nullptr;
	TestTrue(TEXT("mat.instance.params.get result has params"), (*GetResultObject)->TryGetArrayField(TEXT("params"), GetParams));

	FString SetResponseJson;
	bool bSetSuccess = false;
	const FString SetParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"recompile\":\"never\",\"set\":[{\"name\":\"MCPScalar\",\"kind\":\"scalar\",\"value\":1.5},{\"name\":\"MCPVector\",\"kind\":\"vector\",\"value\":{\"r\":1.0,\"g\":0.5,\"b\":0.25,\"a\":1.0}}],\"clear\":[]}"),
		*MaterialPath);
	const FString SetRequestJson = MakeRequestEnvelope(TEXT("mat.instance.params.set"), SetParamsJson);
	TestTrue(TEXT("Execute mat.instance.params.set request"), ExecuteMCPRequest(SetRequestJson, SetResponseJson, bSetSuccess));
	TestTrue(TEXT("mat.instance.params.set status should be success"), bSetSuccess);

	TSharedPtr<FJsonObject> SetResponseObject;
	TestTrue(TEXT("Parse mat.instance.params.set response"), ParseJsonObject(SetResponseJson, SetResponseObject));
	const TSharedPtr<FJsonObject>* SetResultObject = nullptr;
	TestTrue(TEXT("mat.instance.params.set has result"), SetResponseObject->TryGetObjectField(TEXT("result"), SetResultObject));

	const TArray<TSharedPtr<FJsonValue>>* Updated = nullptr;
	TestTrue(TEXT("mat.instance.params.set result has updated"), (*SetResultObject)->TryGetArrayField(TEXT("updated"), Updated));
	TestTrue(TEXT("mat.instance.params.set updated includes MCPScalar"), JsonArrayContainsString(Updated, TEXT("MCPScalar")));
	TestTrue(TEXT("mat.instance.params.set updated includes MCPVector"), JsonArrayContainsString(Updated, TEXT("MCPVector")));

	FString CompileStatus;
	TestTrue(TEXT("mat.instance.params.set has compile_status"), (*SetResultObject)->TryGetStringField(TEXT("compile_status"), CompileStatus));
	TestEqual(TEXT("mat.instance.params.set compile_status"), CompileStatus, FString(TEXT("skipped")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPNiagaraToolsAutomationTest,
	"UnrealMCP.Runtime.NiagaraTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPNiagaraToolsAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UMaterialInstanceConstant* MaterialInstance = GetOrCreateRuntimeMaterialInstance();
	TestNotNull(TEXT("Create object for niagara tools"), MaterialInstance);
	if (MaterialInstance == nullptr)
	{
		return false;
	}

	const FString ObjectPath = MaterialInstance->GetPathName();

	FString GetResponseJson;
	bool bGetSuccess = false;
	const FString GetParamsJson = FString::Printf(TEXT("{\"object_path\":\"%s\"}"), *ObjectPath);
	const FString GetRequestJson = MakeRequestEnvelope(TEXT("niagara.params.get"), GetParamsJson);
	TestTrue(TEXT("Execute niagara.params.get request"), ExecuteMCPRequest(GetRequestJson, GetResponseJson, bGetSuccess));
	TestTrue(TEXT("niagara.params.get status should be success"), bGetSuccess);

	TSharedPtr<FJsonObject> GetResponseObject;
	TestTrue(TEXT("Parse niagara.params.get response"), ParseJsonObject(GetResponseJson, GetResponseObject));
	const TSharedPtr<FJsonObject>* GetResultObject = nullptr;
	TestTrue(TEXT("niagara.params.get has result"), GetResponseObject->TryGetObjectField(TEXT("result"), GetResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
	TestTrue(TEXT("niagara.params.get result has params"), (*GetResultObject)->TryGetArrayField(TEXT("params"), Params));

	FString SetResponseJson;
	bool bSetSuccess = false;
	const FString SetParamsJson = FString::Printf(TEXT("{\"object_path\":\"%s\",\"strict_types\":true,\"set\":[]}"), *ObjectPath);
	const FString SetRequestJson = MakeRequestEnvelope(TEXT("niagara.params.set"), SetParamsJson);
	TestTrue(TEXT("Execute niagara.params.set request"), ExecuteMCPRequest(SetRequestJson, SetResponseJson, bSetSuccess));
	TestTrue(TEXT("niagara.params.set status should be success"), bSetSuccess);

	TSharedPtr<FJsonObject> SetResponseObject;
	TestTrue(TEXT("Parse niagara.params.set response"), ParseJsonObject(SetResponseJson, SetResponseObject));
	const TSharedPtr<FJsonObject>* SetResultObject = nullptr;
	TestTrue(TEXT("niagara.params.set has result"), SetResponseObject->TryGetObjectField(TEXT("result"), SetResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Updated = nullptr;
	TestTrue(TEXT("niagara.params.set result has updated"), (*SetResultObject)->TryGetArrayField(TEXT("updated"), Updated));

	FString StackListResponseJson;
	bool bStackListSuccess = false;
	const FString StackListParamsJson = FString::Printf(TEXT("{\"object_path\":\"%s\"}"), *ObjectPath);
	const FString StackListRequestJson = MakeRequestEnvelope(TEXT("niagara.stack.list"), StackListParamsJson);
	TestTrue(TEXT("Execute niagara.stack.list request"), ExecuteMCPRequest(StackListRequestJson, StackListResponseJson, bStackListSuccess));
	TestTrue(TEXT("niagara.stack.list status should be success"), bStackListSuccess);

	TSharedPtr<FJsonObject> StackListResponseObject;
	TestTrue(TEXT("Parse niagara.stack.list response"), ParseJsonObject(StackListResponseJson, StackListResponseObject));
	const TSharedPtr<FJsonObject>* StackListResultObject = nullptr;
	TestTrue(TEXT("niagara.stack.list has result"), StackListResponseObject->TryGetObjectField(TEXT("result"), StackListResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	TestTrue(TEXT("niagara.stack.list result has modules"), (*StackListResultObject)->TryGetArrayField(TEXT("modules"), Modules));
	TestTrue(TEXT("niagara.stack.list returns at least one module"), Modules != nullptr && Modules->Num() > 0);

	FString ModuleKey;
	if (Modules != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
		{
			if (!ModuleValue.IsValid() || ModuleValue->Type != EJson::Object)
			{
				continue;
			}

			ModuleValue->AsObject()->TryGetStringField(TEXT("module_key"), ModuleKey);
			if (!ModuleKey.IsEmpty())
			{
				break;
			}
		}
	}
	TestTrue(TEXT("niagara.stack.list provides module_key"), !ModuleKey.IsEmpty());

	FString StackSetResponseJson;
	bool bStackSetSuccess = false;
	const FString StackSetParamsJson = FString::Printf(TEXT("{\"module_key\":\"%s\",\"object_path\":\"%s\",\"set\":[]}"), *ModuleKey, *ObjectPath);
	const FString StackSetRequestJson = MakeRequestEnvelope(TEXT("niagara.stack.module.set_param"), StackSetParamsJson);
	TestTrue(TEXT("Execute niagara.stack.module.set_param request"), ExecuteMCPRequest(StackSetRequestJson, StackSetResponseJson, bStackSetSuccess));
	TestTrue(TEXT("niagara.stack.module.set_param status should be success"), bStackSetSuccess);

	TSharedPtr<FJsonObject> StackSetResponseObject;
	TestTrue(TEXT("Parse niagara.stack.module.set_param response"), ParseJsonObject(StackSetResponseJson, StackSetResponseObject));
	const TSharedPtr<FJsonObject>* StackSetResultObject = nullptr;
	TestTrue(TEXT("niagara.stack.module.set_param has result"), StackSetResponseObject->TryGetObjectField(TEXT("result"), StackSetResultObject));
	const TArray<TSharedPtr<FJsonValue>>* StackSetUpdated = nullptr;
	TestTrue(TEXT("niagara.stack.module.set_param result has updated"), (*StackSetResultObject)->TryGetArrayField(TEXT("updated"), StackSetUpdated));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPUMGToolsAutomationTest,
	"UnrealMCP.Runtime.UMGTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPUMGToolsAutomationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UWidgetBlueprint* WidgetBlueprint = GetOrCreateRuntimeWidgetBlueprint();
	TestNotNull(TEXT("Create widget blueprint for umg tools"), WidgetBlueprint);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
	{
		return false;
	}

	UWidget* TargetWidget = WidgetBlueprint->WidgetTree->FindWidget(TEXT("RuntimeTextWidget"));
	TestNotNull(TEXT("Create target widget for umg tools"), TargetWidget);
	if (TargetWidget == nullptr)
	{
		return false;
	}

	const FString WidgetBlueprintPath = WidgetBlueprint->GetPathName();
	const FString TargetWidgetName = TargetWidget->GetName();

	FString TreeResponseJson;
	bool bTreeSuccess = false;
	const FString TreeParamsJson = FString::Printf(TEXT("{\"object_path\":\"%s\",\"depth\":5}"), *WidgetBlueprintPath);
	const FString TreeRequestJson = MakeRequestEnvelope(TEXT("umg.tree.get"), TreeParamsJson);
	TestTrue(TEXT("Execute umg.tree.get request"), ExecuteMCPRequest(TreeRequestJson, TreeResponseJson, bTreeSuccess));
	TestTrue(TEXT("umg.tree.get status should be success"), bTreeSuccess);

	TSharedPtr<FJsonObject> TreeResponseObject;
	TestTrue(TEXT("Parse umg.tree.get response"), ParseJsonObject(TreeResponseJson, TreeResponseObject));
	const TSharedPtr<FJsonObject>* TreeResultObject = nullptr;
	TestTrue(TEXT("umg.tree.get has result"), TreeResponseObject->TryGetObjectField(TEXT("result"), TreeResultObject));
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("umg.tree.get result has nodes"), (*TreeResultObject)->TryGetArrayField(TEXT("nodes"), Nodes));

	bool bFoundTargetWidget = false;
	for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
	{
		if (!NodeValue.IsValid() || NodeValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
		FString NodeName;
		NodeObject->TryGetStringField(TEXT("name"), NodeName);
		if (NodeName == TargetWidgetName)
		{
			bFoundTargetWidget = true;
			break;
		}
	}
	TestTrue(TEXT("umg.tree.get contains RuntimeTextWidget node"), bFoundTargetWidget);

	FString InspectResponseJson;
	bool bInspectSuccess = false;
	const FString InspectParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"depth\":1}"),
		*WidgetBlueprintPath,
		*TargetWidgetName);
	const FString InspectRequestJson = MakeRequestEnvelope(TEXT("umg.widget.inspect"), InspectParamsJson);
	TestTrue(TEXT("Execute umg.widget.inspect request"), ExecuteMCPRequest(InspectRequestJson, InspectResponseJson, bInspectSuccess));
	TestTrue(TEXT("umg.widget.inspect status should be success"), bInspectSuccess);

	TSharedPtr<FJsonObject> InspectResponseObject;
	TestTrue(TEXT("Parse umg.widget.inspect response"), ParseJsonObject(InspectResponseJson, InspectResponseObject));
	const TSharedPtr<FJsonObject>* InspectResultObject = nullptr;
	TestTrue(TEXT("umg.widget.inspect has result"), InspectResponseObject->TryGetObjectField(TEXT("result"), InspectResultObject));
	const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
	TestTrue(TEXT("umg.widget.inspect result has widget"), (*InspectResultObject)->TryGetObjectField(TEXT("widget"), WidgetObject));

	FString WidgetName;
	TestTrue(TEXT("umg.widget.inspect widget has name"), (*WidgetObject)->TryGetStringField(TEXT("name"), WidgetName));
	TestEqual(TEXT("umg.widget.inspect widget name"), WidgetName, TargetWidgetName);

	FString WidgetPatchResponseJson;
	bool bWidgetPatchSuccess = false;
	const FString WidgetPatchParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"patch\":[{\"op\":\"replace\",\"path\":\"/RenderOpacity\",\"value\":0.5}],\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*TargetWidgetName);
	const FString WidgetPatchRequestJson = MakeRequestEnvelope(TEXT("umg.widget.patch"), WidgetPatchParamsJson);
	TestTrue(TEXT("Execute umg.widget.patch request"), ExecuteMCPRequest(WidgetPatchRequestJson, WidgetPatchResponseJson, bWidgetPatchSuccess));
	TestTrue(TEXT("umg.widget.patch status should be success"), bWidgetPatchSuccess);

	TSharedPtr<FJsonObject> WidgetPatchResponseObject;
	TestTrue(TEXT("Parse umg.widget.patch response"), ParseJsonObject(WidgetPatchResponseJson, WidgetPatchResponseObject));
	const TSharedPtr<FJsonObject>* WidgetPatchResultObject = nullptr;
	TestTrue(TEXT("umg.widget.patch has result"), WidgetPatchResponseObject->TryGetObjectField(TEXT("result"), WidgetPatchResultObject));
	const TArray<TSharedPtr<FJsonValue>>* WidgetChangedProperties = nullptr;
	TestTrue(TEXT("umg.widget.patch result has changed_properties"), (*WidgetPatchResultObject)->TryGetArrayField(TEXT("changed_properties"), WidgetChangedProperties));
	TestTrue(TEXT("umg.widget.patch changed_properties includes RenderOpacity"), JsonArrayContainsString(WidgetChangedProperties, TEXT("RenderOpacity")));

	const TSharedPtr<FJsonObject>* CompileObject = nullptr;
	TestTrue(TEXT("umg.widget.patch result has compile"), (*WidgetPatchResultObject)->TryGetObjectField(TEXT("compile"), CompileObject));
	FString CompileStatus;
	TestTrue(TEXT("umg.widget.patch compile has status"), (*CompileObject)->TryGetStringField(TEXT("status"), CompileStatus));
	TestEqual(TEXT("umg.widget.patch compile status"), CompileStatus, FString(TEXT("skipped")));

	FString SlotPatchResponseJson;
	bool bSlotPatchSuccess = false;
	const FString SlotPatchParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"patch\":[{\"op\":\"replace\",\"path\":\"/Padding\",\"value\":{\"Left\":1.0,\"Top\":2.0,\"Right\":3.0,\"Bottom\":4.0}}]}"),
		*WidgetBlueprintPath,
		*TargetWidgetName);
	const FString SlotPatchRequestJson = MakeRequestEnvelope(TEXT("umg.slot.patch"), SlotPatchParamsJson);
	TestTrue(TEXT("Execute umg.slot.patch request"), ExecuteMCPRequest(SlotPatchRequestJson, SlotPatchResponseJson, bSlotPatchSuccess));
	TestTrue(TEXT("umg.slot.patch status should be success"), bSlotPatchSuccess);

	TSharedPtr<FJsonObject> SlotPatchResponseObject;
	TestTrue(TEXT("Parse umg.slot.patch response"), ParseJsonObject(SlotPatchResponseJson, SlotPatchResponseObject));
	const TSharedPtr<FJsonObject>* SlotPatchResultObject = nullptr;
	TestTrue(TEXT("umg.slot.patch has result"), SlotPatchResponseObject->TryGetObjectField(TEXT("result"), SlotPatchResultObject));
	const TArray<TSharedPtr<FJsonValue>>* SlotChangedProperties = nullptr;
	TestTrue(TEXT("umg.slot.patch result has changed_properties"), (*SlotPatchResultObject)->TryGetArrayField(TEXT("changed_properties"), SlotChangedProperties));
	TestTrue(TEXT("umg.slot.patch changed_properties includes Padding"), JsonArrayContainsString(SlotChangedProperties, TEXT("Padding")));

	FString ClassListResponseJson;
	bool bClassListSuccess = false;
	const FString ClassListRequestJson = MakeRequestEnvelope(TEXT("umg.widget.class.list"), TEXT("{\"name_glob\":\"*Button*\",\"limit\":200}"));
	TestTrue(TEXT("Execute umg.widget.class.list request"), ExecuteMCPRequest(ClassListRequestJson, ClassListResponseJson, bClassListSuccess));
	TestTrue(TEXT("umg.widget.class.list status should be success"), bClassListSuccess);

	TSharedPtr<FJsonObject> ClassListResponseObject;
	TestTrue(TEXT("Parse umg.widget.class.list response"), ParseJsonObject(ClassListResponseJson, ClassListResponseObject));
	const TSharedPtr<FJsonObject>* ClassListResultObject = nullptr;
	TestTrue(TEXT("umg.widget.class.list has result"), ClassListResponseObject->TryGetObjectField(TEXT("result"), ClassListResultObject));
	const TArray<TSharedPtr<FJsonValue>>* ClassValues = nullptr;
	TestTrue(TEXT("umg.widget.class.list has classes"), (*ClassListResultObject)->TryGetArrayField(TEXT("classes"), ClassValues));
	bool bFoundButtonClass = false;
	if (ClassValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& ClassValue : *ClassValues)
		{
			if (!ClassValue.IsValid() || ClassValue->Type != EJson::Object)
			{
				continue;
			}

			FString ClassPath;
			ClassValue->AsObject()->TryGetStringField(TEXT("class_path"), ClassPath);
			if (ClassPath == TEXT("/Script/UMG.Button"))
			{
				bFoundButtonClass = true;
				break;
			}
		}
	}
	TestTrue(TEXT("umg.widget.class.list contains /Script/UMG.Button"), bFoundButtonClass);

	FString AddContainerResponseJson;
	bool bAddContainerSuccess = false;
	const FString AddContainerParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_class_path\":\"/Script/UMG.SizeBox\",\"widget_name\":\"RuntimeContainer\",\"parent_ref\":{\"name\":\"RootCanvas\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath);
	const FString AddContainerRequestJson = MakeRequestEnvelope(TEXT("umg.widget.add"), AddContainerParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.add for container"), ExecuteMCPRequest(AddContainerRequestJson, AddContainerResponseJson, bAddContainerSuccess));
	TestTrue(TEXT("umg.widget.add container status should be success"), bAddContainerSuccess);

	TSharedPtr<FJsonObject> AddContainerResponseObject;
	TestTrue(TEXT("Parse umg.widget.add container response"), ParseJsonObject(AddContainerResponseJson, AddContainerResponseObject));
	const TSharedPtr<FJsonObject>* AddContainerResultObject = nullptr;
	TestTrue(TEXT("umg.widget.add container has result"), AddContainerResponseObject->TryGetObjectField(TEXT("result"), AddContainerResultObject));
	const TSharedPtr<FJsonObject>* AddedContainerWidgetObject = nullptr;
	TestTrue(TEXT("umg.widget.add container result has widget"), (*AddContainerResultObject)->TryGetObjectField(TEXT("widget"), AddedContainerWidgetObject));
	FString AddedContainerName;
	TestTrue(TEXT("umg.widget.add container widget has name"), (*AddedContainerWidgetObject)->TryGetStringField(TEXT("name"), AddedContainerName));

	FString AddButtonResponseJson;
	bool bAddButtonSuccess = false;
	const FString AddButtonParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_class_path\":\"/Script/UMG.Button\",\"widget_name\":\"RuntimeAddedButton\",\"parent_ref\":{\"name\":\"RootCanvas\"},\"slot_patch\":[{\"op\":\"replace\",\"path\":\"/ZOrder\",\"value\":55}],\"compile_on_success\":false}"),
		*WidgetBlueprintPath);
	const FString AddButtonRequestJson = MakeRequestEnvelope(TEXT("umg.widget.add"), AddButtonParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.add for button"), ExecuteMCPRequest(AddButtonRequestJson, AddButtonResponseJson, bAddButtonSuccess));
	TestTrue(TEXT("umg.widget.add button status should be success"), bAddButtonSuccess);

	TSharedPtr<FJsonObject> AddButtonResponseObject;
	TestTrue(TEXT("Parse umg.widget.add button response"), ParseJsonObject(AddButtonResponseJson, AddButtonResponseObject));
	const TSharedPtr<FJsonObject>* AddButtonResultObject = nullptr;
	TestTrue(TEXT("umg.widget.add button has result"), AddButtonResponseObject->TryGetObjectField(TEXT("result"), AddButtonResultObject));
	const TSharedPtr<FJsonObject>* AddedButtonWidgetObject = nullptr;
	TestTrue(TEXT("umg.widget.add button result has widget"), (*AddButtonResultObject)->TryGetObjectField(TEXT("widget"), AddedButtonWidgetObject));
	FString AddedButtonName;
	TestTrue(TEXT("umg.widget.add button widget has name"), (*AddedButtonWidgetObject)->TryGetStringField(TEXT("name"), AddedButtonName));
	const TArray<TSharedPtr<FJsonValue>>* AddedButtonSlotChanges = nullptr;
	TestTrue(TEXT("umg.widget.add button has slot_changed_properties"), (*AddButtonResultObject)->TryGetArrayField(TEXT("slot_changed_properties"), AddedButtonSlotChanges));
	TestTrue(TEXT("umg.widget.add button slot_changed_properties includes ZOrder"), JsonArrayContainsString(AddedButtonSlotChanges, TEXT("ZOrder")));

	FString ReparentResponseJson;
	bool bReparentSuccess = false;
	const FString ReparentParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"new_parent_ref\":{\"name\":\"%s\"},\"replace_content\":true,\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedButtonName,
		*AddedContainerName);
	const FString ReparentRequestJson = MakeRequestEnvelope(TEXT("umg.widget.reparent"), ReparentParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.reparent request"), ExecuteMCPRequest(ReparentRequestJson, ReparentResponseJson, bReparentSuccess));
	TestTrue(TEXT("umg.widget.reparent status should be success"), bReparentSuccess);

	TSharedPtr<FJsonObject> ReparentResponseObject;
	TestTrue(TEXT("Parse umg.widget.reparent response"), ParseJsonObject(ReparentResponseJson, ReparentResponseObject));
	const TSharedPtr<FJsonObject>* ReparentResultObject = nullptr;
	TestTrue(TEXT("umg.widget.reparent has result"), ReparentResponseObject->TryGetObjectField(TEXT("result"), ReparentResultObject));
	bool bMoved = false;
	TestTrue(TEXT("umg.widget.reparent result has moved"), (*ReparentResultObject)->TryGetBoolField(TEXT("moved"), bMoved));
	TestTrue(TEXT("umg.widget.reparent moved should be true"), bMoved);

	FString AddNamedSlotHostResponseJson;
	bool bAddNamedSlotHostSuccess = false;
	const FString AddNamedSlotHostParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_class_path\":\"/Script/UMG.ExpandableArea\",\"widget_name\":\"RuntimeNamedSlotHost\",\"parent_ref\":{\"name\":\"RootCanvas\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath);
	const FString AddNamedSlotHostRequestJson = MakeRequestEnvelope(TEXT("umg.widget.add"), AddNamedSlotHostParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.add for named slot host"), ExecuteMCPRequest(AddNamedSlotHostRequestJson, AddNamedSlotHostResponseJson, bAddNamedSlotHostSuccess));
	TestTrue(TEXT("umg.widget.add named slot host status should be success"), bAddNamedSlotHostSuccess);

	TSharedPtr<FJsonObject> AddNamedSlotHostResponseObject;
	TestTrue(TEXT("Parse umg.widget.add named slot host response"), ParseJsonObject(AddNamedSlotHostResponseJson, AddNamedSlotHostResponseObject));
	const TSharedPtr<FJsonObject>* AddNamedSlotHostResultObject = nullptr;
	TestTrue(TEXT("umg.widget.add named slot host has result"), AddNamedSlotHostResponseObject->TryGetObjectField(TEXT("result"), AddNamedSlotHostResultObject));
	const TSharedPtr<FJsonObject>* AddedNamedSlotHostWidgetObject = nullptr;
	TestTrue(TEXT("umg.widget.add named slot host has widget"), (*AddNamedSlotHostResultObject)->TryGetObjectField(TEXT("widget"), AddedNamedSlotHostWidgetObject));
	FString AddedNamedSlotHostName;
	TestTrue(TEXT("umg.widget.add named slot host widget has name"), (*AddedNamedSlotHostWidgetObject)->TryGetStringField(TEXT("name"), AddedNamedSlotHostName));

	FString AddNamedSlotContentResponseJson;
	bool bAddNamedSlotContentSuccess = false;
	const FString AddNamedSlotContentParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_class_path\":\"/Script/UMG.TextBlock\",\"widget_name\":\"RuntimeNamedSlotContent\",\"parent_ref\":{\"name\":\"%s\"},\"named_slot_name\":\"Body\",\"replace_content\":true,\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedNamedSlotHostName);
	const FString AddNamedSlotContentRequestJson = MakeRequestEnvelope(TEXT("umg.widget.add"), AddNamedSlotContentParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.add for named slot content"), ExecuteMCPRequest(AddNamedSlotContentRequestJson, AddNamedSlotContentResponseJson, bAddNamedSlotContentSuccess));
	TestTrue(TEXT("umg.widget.add named slot content status should be success"), bAddNamedSlotContentSuccess);

	TSharedPtr<FJsonObject> AddNamedSlotContentResponseObject;
	TestTrue(TEXT("Parse umg.widget.add named slot content response"), ParseJsonObject(AddNamedSlotContentResponseJson, AddNamedSlotContentResponseObject));
	const TSharedPtr<FJsonObject>* AddNamedSlotContentResultObject = nullptr;
	TestTrue(TEXT("umg.widget.add named slot content has result"), AddNamedSlotContentResponseObject->TryGetObjectField(TEXT("result"), AddNamedSlotContentResultObject));
	const TSharedPtr<FJsonObject>* AddedNamedSlotContentWidgetObject = nullptr;
	TestTrue(TEXT("umg.widget.add named slot content has widget"), (*AddNamedSlotContentResultObject)->TryGetObjectField(TEXT("widget"), AddedNamedSlotContentWidgetObject));
	FString AddedNamedSlotContentName;
	TestTrue(TEXT("umg.widget.add named slot content widget has name"), (*AddedNamedSlotContentWidgetObject)->TryGetStringField(TEXT("name"), AddedNamedSlotContentName));
	FString AddedNamedSlotContentType;
	TestTrue(TEXT("umg.widget.add named slot content has slot_type"), (*AddedNamedSlotContentWidgetObject)->TryGetStringField(TEXT("slot_type"), AddedNamedSlotContentType));
	TestEqual(TEXT("umg.widget.add named slot content slot_type"), AddedNamedSlotContentType, FString(TEXT("NamedSlot:Body")));
	FString AddedNamedSlotNameField;
	TestTrue(TEXT("umg.widget.add named slot content has named_slot_name"), (*AddedNamedSlotContentWidgetObject)->TryGetStringField(TEXT("named_slot_name"), AddedNamedSlotNameField));
	TestEqual(TEXT("umg.widget.add named slot content named_slot_name"), AddedNamedSlotNameField, FString(TEXT("Body")));

	FString ReparentNamedSlotContentResponseJson;
	bool bReparentNamedSlotContentSuccess = false;
	const FString ReparentNamedSlotContentParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"new_parent_ref\":{\"name\":\"RootCanvas\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedNamedSlotContentName);
	const FString ReparentNamedSlotContentRequestJson = MakeRequestEnvelope(TEXT("umg.widget.reparent"), ReparentNamedSlotContentParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.reparent for named slot content"), ExecuteMCPRequest(ReparentNamedSlotContentRequestJson, ReparentNamedSlotContentResponseJson, bReparentNamedSlotContentSuccess));
	TestTrue(TEXT("umg.widget.reparent named slot content status should be success"), bReparentNamedSlotContentSuccess);

	TSharedPtr<FJsonObject> ReparentNamedSlotContentResponseObject;
	TestTrue(TEXT("Parse umg.widget.reparent named slot content response"), ParseJsonObject(ReparentNamedSlotContentResponseJson, ReparentNamedSlotContentResponseObject));
	const TSharedPtr<FJsonObject>* ReparentNamedSlotContentResultObject = nullptr;
	TestTrue(TEXT("umg.widget.reparent named slot content has result"), ReparentNamedSlotContentResponseObject->TryGetObjectField(TEXT("result"), ReparentNamedSlotContentResultObject));
	bool bNamedSlotMoved = false;
	TestTrue(TEXT("umg.widget.reparent named slot content has moved"), (*ReparentNamedSlotContentResultObject)->TryGetBoolField(TEXT("moved"), bNamedSlotMoved));
	TestTrue(TEXT("umg.widget.reparent named slot content moved should be true"), bNamedSlotMoved);

	FString RemoveNamedSlotContentResponseJson;
	bool bRemoveNamedSlotContentSuccess = false;
	const FString RemoveNamedSlotContentParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedNamedSlotContentName);
	const FString RemoveNamedSlotContentRequestJson = MakeRequestEnvelope(TEXT("umg.widget.remove"), RemoveNamedSlotContentParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.remove named slot content request"), ExecuteMCPRequest(RemoveNamedSlotContentRequestJson, RemoveNamedSlotContentResponseJson, bRemoveNamedSlotContentSuccess));
	TestTrue(TEXT("umg.widget.remove named slot content status should be success"), bRemoveNamedSlotContentSuccess);

	FString RemoveNamedSlotHostResponseJson;
	bool bRemoveNamedSlotHostSuccess = false;
	const FString RemoveNamedSlotHostParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedNamedSlotHostName);
	const FString RemoveNamedSlotHostRequestJson = MakeRequestEnvelope(TEXT("umg.widget.remove"), RemoveNamedSlotHostParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.remove named slot host request"), ExecuteMCPRequest(RemoveNamedSlotHostRequestJson, RemoveNamedSlotHostResponseJson, bRemoveNamedSlotHostSuccess));
	TestTrue(TEXT("umg.widget.remove named slot host status should be success"), bRemoveNamedSlotHostSuccess);

	FString RemoveButtonResponseJson;
	bool bRemoveButtonSuccess = false;
	const FString RemoveButtonParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedButtonName);
	const FString RemoveButtonRequestJson = MakeRequestEnvelope(TEXT("umg.widget.remove"), RemoveButtonParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.remove button request"), ExecuteMCPRequest(RemoveButtonRequestJson, RemoveButtonResponseJson, bRemoveButtonSuccess));
	TestTrue(TEXT("umg.widget.remove button status should be success"), bRemoveButtonSuccess);

	FString RemoveContainerResponseJson;
	bool bRemoveContainerSuccess = false;
	const FString RemoveContainerParamsJson = FString::Printf(
		TEXT("{\"object_path\":\"%s\",\"widget_ref\":{\"name\":\"%s\"},\"compile_on_success\":false}"),
		*WidgetBlueprintPath,
		*AddedContainerName);
	const FString RemoveContainerRequestJson = MakeRequestEnvelope(TEXT("umg.widget.remove"), RemoveContainerParamsJson, false);
	TestTrue(TEXT("Execute umg.widget.remove container request"), ExecuteMCPRequest(RemoveContainerRequestJson, RemoveContainerResponseJson, bRemoveContainerSuccess));
	TestTrue(TEXT("umg.widget.remove container status should be success"), bRemoveContainerSuccess);

	FString VerifyTreeResponseJson;
	bool bVerifyTreeSuccess = false;
	const FString VerifyTreeRequestJson = MakeRequestEnvelope(TEXT("umg.tree.get"), TreeParamsJson);
	TestTrue(TEXT("Execute umg.tree.get verify request"), ExecuteMCPRequest(VerifyTreeRequestJson, VerifyTreeResponseJson, bVerifyTreeSuccess));
	TestTrue(TEXT("umg.tree.get verify status should be success"), bVerifyTreeSuccess);

	TSharedPtr<FJsonObject> VerifyTreeResponseObject;
	TestTrue(TEXT("Parse verify umg.tree.get response"), ParseJsonObject(VerifyTreeResponseJson, VerifyTreeResponseObject));
	const TSharedPtr<FJsonObject>* VerifyTreeResultObject = nullptr;
	TestTrue(TEXT("verify umg.tree.get has result"), VerifyTreeResponseObject->TryGetObjectField(TEXT("result"), VerifyTreeResultObject));
	const TArray<TSharedPtr<FJsonValue>>* VerifyNodes = nullptr;
	TestTrue(TEXT("verify umg.tree.get has nodes"), (*VerifyTreeResultObject)->TryGetArrayField(TEXT("nodes"), VerifyNodes));
	bool bFoundRemovedContainer = false;
	bool bFoundRemovedButton = false;
	bool bFoundRemovedNamedSlotHost = false;
	bool bFoundRemovedNamedSlotContent = false;
	if (VerifyNodes != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *VerifyNodes)
		{
			if (!NodeValue.IsValid() || NodeValue->Type != EJson::Object)
			{
				continue;
			}
			FString NodeName;
			NodeValue->AsObject()->TryGetStringField(TEXT("name"), NodeName);
			bFoundRemovedContainer |= NodeName == AddedContainerName;
			bFoundRemovedButton |= NodeName == AddedButtonName;
			bFoundRemovedNamedSlotHost |= NodeName == AddedNamedSlotHostName;
			bFoundRemovedNamedSlotContent |= NodeName == AddedNamedSlotContentName;
		}
	}
	TestFalse(TEXT("Removed container should not be present"), bFoundRemovedContainer);
	TestFalse(TEXT("Removed button should not be present"), bFoundRemovedButton);
	TestFalse(TEXT("Removed named slot host should not be present"), bFoundRemovedNamedSlotHost);
	TestFalse(TEXT("Removed named slot content should not be present"), bFoundRemovedNamedSlotContent);
	return true;
}

#endif
