#include "Tools/Settings/MCPToolsSettingsComposeHandler.h"

#include "MCPErrorCodes.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SpectatorPawn.h"
#include "ObjectTools.h"
#include "UObject/UnrealType.h"
#include "Tools/Common/MCPToolCommonJson.h"

namespace
{
	struct FGameModeSlotSpec
	{
		FString JsonKey;
		FString PropertyName;
		FString DefaultNameSuffix;
		UClass* RequiredBaseClass = nullptr;
	};

	struct FGameModeSlotState
	{
		FString JsonKey;
		FString PropertyName;
		FString ClassPath;
		FString ObjectPath;
		FString PackageName;
		FString Source = TEXT("none");
		UClass* ResolvedClass = nullptr;
		bool bCreated = false;
		bool bApplied = false;
	};
}

bool FMCPToolsSettingsComposeHandler::HandleGameModeCompose(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FParseAutoSaveOptionFn ParseAutoSaveOption,
	FParseSettingsSaveOptionsFn ParseSettingsSaveOptions,
	FParseTransactionLabelFn ParseTransactionLabel,
	FIsValidAssetDestinationFn IsValidAssetDestination,
	FResolveClassByPathFn ResolveClassByPath,
	FEnsureBlueprintAssetFn EnsureBlueprintAsset,
	FResolveClassPathWithBaseFn ResolveClassPathWithBase,
	FSetClassPropertyOnObjectFn SetClassPropertyOnObject,
	FSavePackageByNameFn SavePackageByName,
	FHandleSettingsGameModeSetDefaultFn HandleSettingsGameModeSetDefault,
	FHandleSettingsGameModeSetMapOverrideFn HandleSettingsGameModeSetMapOverride,
	FParseStringArrayFieldFn ParseStringArrayField)
{
	FString PackagePath;
	FString GameModeAssetName;
	FString GameModeParentClassPath = TEXT("/Script/Engine.GameModeBase");
	FString MapKey;
	bool bCreateMissingClasses = true;
	bool bSetAsDefault = false;
	bool bOverwrite = false;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	FMCPToolSettingsSaveOptions SettingsSaveOptions;
	const TSharedPtr<FJsonObject>* ClassPathsObject = nullptr;
	const TSharedPtr<FJsonObject>* ClassNamesObject = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("package_path"), PackagePath);
		Request.Params->TryGetStringField(TEXT("game_mode_asset_name"), GameModeAssetName);
		Request.Params->TryGetStringField(TEXT("game_mode_parent_class_path"), GameModeParentClassPath);
		Request.Params->TryGetStringField(TEXT("map_key"), MapKey);
		Request.Params->TryGetBoolField(TEXT("create_missing_classes"), bCreateMissingClasses);
		Request.Params->TryGetBoolField(TEXT("set_as_default"), bSetAsDefault);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetObjectField(TEXT("class_paths"), ClassPathsObject);
		Request.Params->TryGetObjectField(TEXT("class_names"), ClassNamesObject);
		ParseAutoSaveOption(Request.Params, bAutoSave);
		ParseSettingsSaveOptions(Request.Params, SettingsSaveOptions);
	}

	GameModeAssetName = ObjectTools::SanitizeObjectName(GameModeAssetName);
	MapKey.TrimStartAndEndInline();

	if (PackagePath.IsEmpty() || GameModeAssetName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("package_path and game_mode_asset_name are required for settings.gamemode.compose.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(PackagePath, GameModeAssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid package_path or game_mode_asset_name for settings.gamemode.compose.");
		Diagnostic.Detail = FString::Printf(TEXT("package_path=%s game_mode_asset_name=%s"), *PackagePath, *GameModeAssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* GameModeParentClass = ResolveClassByPath(GameModeParentClassPath);
	if (GameModeParentClass == nullptr || !GameModeParentClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::GAMEMODE_CLASS_INVALID;
		Diagnostic.Message = TEXT("game_mode_parent_class_path is invalid.");
		Diagnostic.Detail = GameModeParentClassPath;
		Diagnostic.Suggestion = TEXT("Provide an AGameModeBase subclass path.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(GameModeParentClass))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("Blueprint cannot be created from game_mode_parent_class_path.");
		Diagnostic.Detail = GameModeParentClass->GetPathName();
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Compose"));

	FMCPComposeBlueprintRequest GameModeCreateRequest;
	GameModeCreateRequest.PackagePath = PackagePath;
	GameModeCreateRequest.AssetName = GameModeAssetName;
	GameModeCreateRequest.ParentClass = GameModeParentClass;
	GameModeCreateRequest.bOverwrite = bOverwrite;
	GameModeCreateRequest.bDryRun = Request.Context.bDryRun;
	GameModeCreateRequest.bCompileOnSuccess = false;
	GameModeCreateRequest.bAutoSave = false;
	GameModeCreateRequest.TransactionLabel = TransactionLabel;

	FMCPComposeBlueprintResult GameModeCreateResult;
	FMCPDiagnostic GameModeCreateDiagnostic;
	if (!EnsureBlueprintAsset(GameModeCreateRequest, OutResult, GameModeCreateResult, GameModeCreateDiagnostic))
	{
		OutResult.Diagnostics.Add(GameModeCreateDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const TArray<FGameModeSlotSpec> SlotSpecs{
		{ TEXT("default_pawn_class"), TEXT("DefaultPawnClass"), TEXT("Pawn"), APawn::StaticClass() },
		{ TEXT("hud_class"), TEXT("HUDClass"), TEXT("HUD"), AHUD::StaticClass() },
		{ TEXT("player_controller_class"), TEXT("PlayerControllerClass"), TEXT("PlayerController"), APlayerController::StaticClass() },
		{ TEXT("game_state_class"), TEXT("GameStateClass"), TEXT("GameState"), AGameStateBase::StaticClass() },
		{ TEXT("player_state_class"), TEXT("PlayerStateClass"), TEXT("PlayerState"), APlayerState::StaticClass() },
		{ TEXT("spectator_class"), TEXT("SpectatorClass"), TEXT("SpectatorPawn"), ASpectatorPawn::StaticClass() }
	};

	UClass* GameModeGeneratedClass = ResolveClassByPath(GameModeCreateResult.GeneratedClassPath);
	if (!Request.Context.bDryRun && GameModeGeneratedClass == nullptr && GameModeCreateResult.BlueprintAsset != nullptr)
	{
		FKismetEditorUtilities::CompileBlueprint(GameModeCreateResult.BlueprintAsset);
		GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
	}

	UObject* GameModeCDO = (GameModeGeneratedClass != nullptr) ? GameModeGeneratedClass->GetDefaultObject() : nullptr;
	TArray<FString> ChangedProperties;
	TArray<FGameModeSlotState> SlotStates;
	SlotStates.Reserve(SlotSpecs.Num());

	auto ResolveSlotClassFromCDO = [&](const FString& PropertyName, UClass* RequiredBaseClass, FGameModeSlotState& InOutSlotState) -> void
	{
		if (GameModeCDO == nullptr)
		{
			return;
		}

		FClassProperty* ClassProperty = FindFProperty<FClassProperty>(GameModeCDO->GetClass(), FName(*PropertyName));
		if (ClassProperty == nullptr)
		{
			return;
		}

		UClass* CurrentClass = Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(GameModeCDO));
		if (CurrentClass == nullptr || (RequiredBaseClass != nullptr && !CurrentClass->IsChildOf(RequiredBaseClass)))
		{
			return;
		}

		InOutSlotState.Source = TEXT("existing");
		InOutSlotState.ResolvedClass = CurrentClass;
		InOutSlotState.ClassPath = CurrentClass->GetPathName();
	};

	for (const FGameModeSlotSpec& SlotSpec : SlotSpecs)
	{
		FGameModeSlotState SlotState;
		SlotState.JsonKey = SlotSpec.JsonKey;
		SlotState.PropertyName = SlotSpec.PropertyName;

		FString RequestedClassPath;
		if (ClassPathsObject != nullptr && ClassPathsObject->IsValid())
		{
			(*ClassPathsObject)->TryGetStringField(SlotSpec.JsonKey, RequestedClassPath);
		}
		RequestedClassPath.TrimStartAndEndInline();

		if (!RequestedClassPath.IsEmpty())
		{
			FMCPDiagnostic ResolveDiagnostic;
			if (!ResolveClassPathWithBase(
				RequestedClassPath,
				SlotSpec.RequiredBaseClass,
				SlotState.ClassPath,
				SlotState.ResolvedClass,
				ResolveDiagnostic))
			{
				OutResult.Diagnostics.Add(ResolveDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotState.Source = TEXT("provided");
		}
		else if (bCreateMissingClasses)
		{
			FString SlotAssetName;
			if (ClassNamesObject != nullptr && ClassNamesObject->IsValid())
			{
				(*ClassNamesObject)->TryGetStringField(SlotSpec.JsonKey, SlotAssetName);
			}

			SlotAssetName = ObjectTools::SanitizeObjectName(SlotAssetName);
			if (SlotAssetName.IsEmpty())
			{
				SlotAssetName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s"), *GameModeAssetName, *SlotSpec.DefaultNameSuffix));
			}

			if (SlotAssetName.IsEmpty())
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
				Diagnostic.Message = TEXT("Failed to resolve a valid asset name for compose slot.");
				Diagnostic.Detail = SlotSpec.JsonKey;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			FMCPComposeBlueprintRequest SlotCreateRequest;
			SlotCreateRequest.PackagePath = PackagePath;
			SlotCreateRequest.AssetName = SlotAssetName;
			SlotCreateRequest.ParentClass = SlotSpec.RequiredBaseClass;
			SlotCreateRequest.bOverwrite = bOverwrite;
			SlotCreateRequest.bDryRun = Request.Context.bDryRun;
			SlotCreateRequest.bCompileOnSuccess = bCompileOnSuccess;
			SlotCreateRequest.bAutoSave = false;
			SlotCreateRequest.TransactionLabel = FString::Printf(TEXT("%s - %s"), *TransactionLabel, *SlotSpec.JsonKey);

			FMCPComposeBlueprintResult SlotCreateResult;
			FMCPDiagnostic SlotCreateDiagnostic;
			if (!EnsureBlueprintAsset(SlotCreateRequest, OutResult, SlotCreateResult, SlotCreateDiagnostic))
			{
				OutResult.Diagnostics.Add(SlotCreateDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotState.Source = SlotCreateResult.bCreatedNew ? TEXT("created") : TEXT("reused");
			SlotState.bCreated = SlotCreateResult.bCreatedNew;
			SlotState.ObjectPath = SlotCreateResult.ObjectPath;
			SlotState.PackageName = SlotCreateResult.PackageName;
			SlotState.ClassPath = SlotCreateResult.GeneratedClassPath;

			FMCPDiagnostic ResolveDiagnostic;
			FString ResolvedSlotClassPath;
			UClass* ResolvedSlotClass = nullptr;
			if (!ResolveClassPathWithBase(
				SlotState.ClassPath,
				SlotSpec.RequiredBaseClass,
				ResolvedSlotClassPath,
				ResolvedSlotClass,
				ResolveDiagnostic))
			{
				if (!Request.Context.bDryRun)
				{
					OutResult.Diagnostics.Add(ResolveDiagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}

				SlotState.ResolvedClass = nullptr;
			}
			else
			{
				SlotState.ClassPath = ResolvedSlotClassPath;
				SlotState.ResolvedClass = ResolvedSlotClass;
			}
		}
		else
		{
			ResolveSlotClassFromCDO(SlotSpec.PropertyName, SlotSpec.RequiredBaseClass, SlotState);
			if (SlotState.Source.Equals(TEXT("none"), ESearchCase::CaseSensitive))
			{
				SlotState.Source = TEXT("unchanged");
			}
		}

		SlotStates.Add(MoveTemp(SlotState));
	}

	if (!Request.Context.bDryRun)
	{
		if (GameModeCreateResult.BlueprintAsset == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			Diagnostic.Message = TEXT("GameMode blueprint asset could not be resolved.");
			Diagnostic.Detail = GameModeCreateResult.ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (GameModeGeneratedClass == nullptr)
		{
			GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
		}
		if (GameModeGeneratedClass == nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(GameModeCreateResult.BlueprintAsset);
			GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
		}
		if (GameModeGeneratedClass == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			Diagnostic.Message = TEXT("Failed to resolve generated class for GameMode blueprint.");
			Diagnostic.Detail = GameModeCreateResult.ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		GameModeCDO = GameModeGeneratedClass->GetDefaultObject();
		if (GameModeCDO == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			Diagnostic.Message = TEXT("Failed to resolve default object for composed GameMode.");
			Diagnostic.Detail = GameModeGeneratedClass->GetPathName();
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		GameModeCreateResult.BlueprintAsset->Modify();
		for (FGameModeSlotState& SlotState : SlotStates)
		{
			if (SlotState.ResolvedClass == nullptr)
			{
				continue;
			}

			FMCPDiagnostic SetPropertyDiagnostic;
			if (!SetClassPropertyOnObject(GameModeCDO, SlotState.PropertyName, SlotState.ResolvedClass, SetPropertyDiagnostic))
			{
				OutResult.Diagnostics.Add(SetPropertyDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotState.bApplied = true;
			ChangedProperties.AddUnique(SlotState.PropertyName);
		}

		GameModeCDO->MarkPackageDirty();
		GameModeCreateResult.BlueprintAsset->MarkPackageDirty();

		if (bCompileOnSuccess)
		{
			FKismetEditorUtilities::CompileBlueprint(GameModeCreateResult.BlueprintAsset);
		}

		if (GameModeCreateResult.BlueprintAsset->GeneratedClass != nullptr)
		{
			GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
		}
	}

	const FString GameModeGeneratedClassPath = GameModeGeneratedClass != nullptr
		? GameModeGeneratedClass->GetPathName()
		: GameModeCreateResult.GeneratedClassPath;

	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			if (!PackageName.StartsWith(TEXT("/Game"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	auto CreateSettingsRequest = [&](const FString& ToolName) -> FMCPRequestEnvelope
	{
		FMCPRequestEnvelope SettingsRequest = Request;
		SettingsRequest.Tool = ToolName;
		SettingsRequest.Params = MakeShared<FJsonObject>();

		TSharedRef<FJsonObject> SaveObject = MakeShared<FJsonObject>();
		SaveObject->SetBoolField(TEXT("save_config"), SettingsSaveOptions.bSaveConfig);
		SaveObject->SetBoolField(TEXT("flush_ini"), SettingsSaveOptions.bFlushIni);
		SaveObject->SetBoolField(TEXT("reload_verify"), SettingsSaveOptions.bReloadVerify);
		SettingsRequest.Params->SetObjectField(TEXT("save"), SaveObject);

		TSharedRef<FJsonObject> TransactionObject = MakeShared<FJsonObject>();
		TransactionObject->SetStringField(TEXT("label"), TransactionLabel);
		SettingsRequest.Params->SetObjectField(TEXT("transaction"), TransactionObject);
		return SettingsRequest;
	};

	auto MergeToolResult = [&](const FMCPToolExecutionResult& ToolResult, bool& bOutVerified, TArray<FString>& OutSavedConfigFiles, bool& bOutHadPartialStatus) -> void
	{
		for (const FMCPDiagnostic& Diagnostic : ToolResult.Diagnostics)
		{
			OutResult.Diagnostics.Add(Diagnostic);
		}
		for (const FString& TouchedPackage : ToolResult.TouchedPackages)
		{
			OutResult.TouchedPackages.AddUnique(TouchedPackage);
		}

		if (ToolResult.ResultObject.IsValid())
		{
			TArray<FString> SavedFiles;
			ParseStringArrayField(ToolResult.ResultObject, TEXT("saved_config_files"), SavedFiles);
			for (const FString& SavedFile : SavedFiles)
			{
				OutSavedConfigFiles.AddUnique(SavedFile);
			}

			TArray<FString> ToolChangedProperties;
			ParseStringArrayField(ToolResult.ResultObject, TEXT("changed_properties"), ToolChangedProperties);
			for (const FString& PropertyName : ToolChangedProperties)
			{
				ChangedProperties.AddUnique(PropertyName);
			}

			bool bToolVerified = true;
			if (ToolResult.ResultObject->TryGetBoolField(TEXT("verified"), bToolVerified))
			{
				bOutVerified &= bToolVerified;
			}
		}

		bOutHadPartialStatus |= ToolResult.Status == EMCPResponseStatus::Partial;
	};

	TArray<FString> SavedConfigFiles;
	bool bVerified = true;
	bool bSettingsSetDefaultApplied = false;
	bool bSettingsMapOverrideApplied = false;
	bool bHadPartialStatus = !bAllSaved;

	if (bSetAsDefault)
	{
		FMCPRequestEnvelope SettingsRequest = CreateSettingsRequest(TEXT("settings.gamemode.set_default"));
		SettingsRequest.Params->SetStringField(TEXT("game_mode_class_path"), GameModeGeneratedClassPath);

		FMCPToolExecutionResult SettingsResult;
		if (!HandleSettingsGameModeSetDefault(SettingsRequest, SettingsResult))
		{
			MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
		bSettingsSetDefaultApplied = true;
	}

	if (!MapKey.IsEmpty())
	{
		FMCPRequestEnvelope SettingsRequest = CreateSettingsRequest(TEXT("settings.gamemode.set_map_override"));
		SettingsRequest.Params->SetStringField(TEXT("map_key"), MapKey);
		SettingsRequest.Params->SetStringField(TEXT("game_mode_class_path"), GameModeGeneratedClassPath);

		FMCPToolExecutionResult SettingsResult;
		if (!HandleSettingsGameModeSetMapOverride(SettingsRequest, SettingsResult))
		{
			MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
		bSettingsMapOverrideApplied = true;
	}

	TArray<TSharedPtr<FJsonValue>> SlotEntries;
	SlotEntries.Reserve(SlotStates.Num());
	for (const FGameModeSlotState& SlotState : SlotStates)
	{
		TSharedRef<FJsonObject> SlotObject = MakeShared<FJsonObject>();
		SlotObject->SetStringField(TEXT("slot"), SlotState.JsonKey);
		SlotObject->SetStringField(TEXT("property"), SlotState.PropertyName);
		SlotObject->SetStringField(TEXT("class_path"), SlotState.ClassPath);
		SlotObject->SetStringField(TEXT("source"), SlotState.Source);
		SlotObject->SetBoolField(TEXT("created"), SlotState.bCreated);
		SlotObject->SetBoolField(TEXT("applied"), SlotState.bApplied);
		if (!SlotState.ObjectPath.IsEmpty())
		{
			SlotObject->SetStringField(TEXT("object_path"), SlotState.ObjectPath);
		}
		if (!SlotState.PackageName.IsEmpty())
		{
			SlotObject->SetStringField(TEXT("package"), SlotState.PackageName);
		}
		SlotEntries.Add(MakeShared<FJsonValueObject>(SlotObject));
	}

	TSharedRef<FJsonObject> SettingsAppliedObject = MakeShared<FJsonObject>();
	SettingsAppliedObject->SetBoolField(TEXT("set_default"), bSettingsSetDefaultApplied);
	SettingsAppliedObject->SetBoolField(TEXT("map_override"), bSettingsMapOverrideApplied);
	SettingsAppliedObject->SetStringField(TEXT("map_key"), MapKey);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("game_mode_object_path"), GameModeCreateResult.ObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("game_mode_class_path"), GameModeGeneratedClassPath);
	OutResult.ResultObject->SetStringField(TEXT("game_mode_parent_class_path"), GameModeParentClass->GetPathName());
	OutResult.ResultObject->SetArrayField(TEXT("slots"), SlotEntries);
	OutResult.ResultObject->SetObjectField(TEXT("settings_applied"), SettingsAppliedObject);
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), MCPToolCommonJson::ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bHadPartialStatus ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	return true;
}
