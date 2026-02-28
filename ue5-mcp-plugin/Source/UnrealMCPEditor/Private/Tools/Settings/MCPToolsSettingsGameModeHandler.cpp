#include "Tools/Settings/MCPToolsSettingsGameModeHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "GameMapsSettings.h"
#include "ScopedTransaction.h"

namespace
{
	bool ParseSettingsSaveOptions(const TSharedPtr<FJsonObject>& Params, FMCPToolSettingsSaveOptions& OutOptions)
	{
		OutOptions = FMCPToolSettingsSaveOptions();
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("save_config"), OutOptions.bSaveConfig);
			(*SaveObject)->TryGetBoolField(TEXT("flush_ini"), OutOptions.bFlushIni);
			(*SaveObject)->TryGetBoolField(TEXT("reload_verify"), OutOptions.bReloadVerify);
		}

		return true;
	}

	FString ParseTransactionLabel(const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel)
	{
		if (!Params.IsValid())
		{
			return DefaultLabel;
		}

		FString TransactionLabel = DefaultLabel;
		const TSharedPtr<FJsonObject>* TransactionObject = nullptr;
		if (Params->TryGetObjectField(TEXT("transaction"), TransactionObject) && TransactionObject != nullptr && TransactionObject->IsValid())
		{
			(*TransactionObject)->TryGetStringField(TEXT("label"), TransactionLabel);
		}
		return TransactionLabel;
	}
}

bool FMCPToolsSettingsGameModeHandler::HandleGameModeGet(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FGetSettingsStringPropertyFn GetSettingsStringProperty,
	FGetGameModeMapOverrideRawArrayFn GetGameModeMapOverrideRawArray,
	FBuildMapOverrideEntriesFromRawFn BuildMapOverrideEntriesFromRaw)
{
	(void)Request;

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> RawMapOverrides;
	if (!GetGameModeMapOverrideRawArray(GameMapsSettings, RawMapOverrides))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Message = TEXT("Failed to inspect GameModeMapPrefixes from UGameMapsSettings.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("class_path"), UGameMapsSettings::StaticClass()->GetClassPathName().ToString());
	OutResult.ResultObject->SetStringField(TEXT("config_file"), GameMapsSettings->GetDefaultConfigFilename());
	OutResult.ResultObject->SetStringField(TEXT("global_default_game_mode"), UGameMapsSettings::GetGlobalDefaultGameMode());
	OutResult.ResultObject->SetStringField(TEXT("global_default_server_game_mode"), GetSettingsStringProperty(GameMapsSettings, TEXT("GlobalDefaultServerGameMode")));
	OutResult.ResultObject->SetArrayField(TEXT("map_overrides"), BuildMapOverrideEntriesFromRaw(RawMapOverrides));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSettingsGameModeHandler::HandleGameModeSetDefault(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FResolveGameModeClassPathFn ResolveGameModeClassPath,
	FGetSettingsStringPropertyFn GetSettingsStringProperty,
	FPersistConfigObjectFn PersistConfigObject)
{
	FString GameModeClassPath;
	FString ServerGameModeClassPath;
	FMCPToolSettingsSaveOptions SaveOptions;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("game_mode_class_path"), GameModeClassPath);
		Request.Params->TryGetStringField(TEXT("server_game_mode_class_path"), ServerGameModeClassPath);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	if (GameModeClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("game_mode_class_path is required for settings.gamemode.set_default.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPDiagnostic ClassDiagnostic;
	FString ResolvedGameModeClassPath;
	if (!ResolveGameModeClassPath(GameModeClassPath, ResolvedGameModeClassPath, ClassDiagnostic))
	{
		OutResult.Diagnostics.Add(ClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ResolvedServerGameModeClassPath;
	if (!ResolveGameModeClassPath(ServerGameModeClassPath, ResolvedServerGameModeClassPath, ClassDiagnostic))
	{
		OutResult.Diagnostics.Add(ClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	const FString CurrentDefaultGameModeBefore = UGameMapsSettings::GetGlobalDefaultGameMode();
	const FString CurrentServerGameModeBefore = GetSettingsStringProperty(GameMapsSettings, TEXT("GlobalDefaultServerGameMode"));
	const bool bDefaultChanged = !CurrentDefaultGameModeBefore.Equals(ResolvedGameModeClassPath, ESearchCase::CaseSensitive);
	if (bDefaultChanged)
	{
		ChangedProperties.Add(TEXT("GlobalDefaultGameMode"));
	}
	const bool bServerChanged =
		!ServerGameModeClassPath.IsEmpty() &&
		!CurrentServerGameModeBefore.Equals(ResolvedServerGameModeClassPath, ESearchCase::CaseSensitive);
	if (bServerChanged)
	{
		ChangedProperties.Add(TEXT("GlobalDefaultServerGameMode"));
	}

	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;

	if (!Request.Context.bDryRun && (bDefaultChanged || bServerChanged))
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Set Default"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		GameMapsSettings->Modify();
		if (bDefaultChanged)
		{
			UGameMapsSettings::SetGlobalDefaultGameMode(ResolvedGameModeClassPath);
		}

		if (bServerChanged)
		{
			TSharedRef<FJsonObject> PatchEntry = MakeShared<FJsonObject>();
			PatchEntry->SetStringField(TEXT("op"), TEXT("replace"));
			PatchEntry->SetStringField(TEXT("path"), TEXT("/GlobalDefaultServerGameMode"));
			PatchEntry->SetField(TEXT("value"), MakeShared<FJsonValueString>(ResolvedServerGameModeClassPath));
			const TArray<TSharedPtr<FJsonValue>> PatchOperations{ MakeShared<FJsonValueObject>(PatchEntry) };

			FMCPDiagnostic PatchDiagnostic;
			TArray<FString> PatchedProperties;
			if (!MCPObjectUtils::ApplyPatch(GameMapsSettings, &PatchOperations, PatchedProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		GameMapsSettings->PostEditChange();
		GameMapsSettings->MarkPackageDirty();
		bPersistSucceeded = PersistConfigObject(GameMapsSettings, SaveOptions, SavedConfigFiles, bVerified, OutResult);
	}

	MCPObjectUtils::AppendTouchedPackage(GameMapsSettings, OutResult.TouchedPackages);
	const FString CurrentDefaultGameModeAfter = UGameMapsSettings::GetGlobalDefaultGameMode();
	const FString CurrentServerGameModeAfter = GetSettingsStringProperty(GameMapsSettings, TEXT("GlobalDefaultServerGameMode"));
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetStringField(TEXT("global_default_game_mode"), CurrentDefaultGameModeAfter);
	OutResult.ResultObject->SetStringField(TEXT("global_default_server_game_mode"), CurrentServerGameModeAfter);
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), MCPToolCommonJson::ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsSettingsGameModeHandler::HandleGameModeSetMapOverride(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FResolveGameModeClassPathFn ResolveGameModeClassPath,
	FGetGameModeMapOverrideRawArrayFn GetGameModeMapOverrideRawArray,
	FTryGetMapOverrideGameModeClassPathFn TryGetMapOverrideGameModeClassPath,
	FBuildSoftClassPathJsonValueFn BuildSoftClassPathJsonValue,
	FBuildMapOverrideEntriesFromRawFn BuildMapOverrideEntriesFromRaw,
	FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
	FPersistConfigObjectFn PersistConfigObject)
{
	FString MapKey;
	FString GameModeClassPath;
	FMCPToolSettingsSaveOptions SaveOptions;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("map_key"), MapKey);
		Request.Params->TryGetStringField(TEXT("game_mode_class_path"), GameModeClassPath);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	MapKey.TrimStartAndEndInline();
	if (MapKey.IsEmpty() || GameModeClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("map_key and game_mode_class_path are required for settings.gamemode.set_map_override.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPDiagnostic ClassDiagnostic;
	FString ResolvedGameModeClassPath;
	if (!ResolveGameModeClassPath(GameModeClassPath, ResolvedGameModeClassPath, ClassDiagnostic))
	{
		OutResult.Diagnostics.Add(ClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CurrentRawArray;
	if (!GetGameModeMapOverrideRawArray(GameMapsSettings, CurrentRawArray))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Message = TEXT("Failed to inspect GameModeMapPrefixes from UGameMapsSettings.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	auto GetRawEntryName = [](const TSharedPtr<FJsonValue>& EntryValue) -> FString
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		FString Name;
		EntryObject->TryGetStringField(TEXT("Name"), Name);
		if (Name.IsEmpty())
		{
			EntryObject->TryGetStringField(TEXT("name"), Name);
		}
		return Name;
	};

	auto GetRawEntryGameMode = [&](const TSharedPtr<FJsonValue>& EntryValue) -> FString
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		FString GameMode;
		TryGetMapOverrideGameModeClassPath(EntryObject, GameMode);
		return GameMode;
	};

	TArray<TSharedPtr<FJsonValue>> UpdatedRawArray = CurrentRawArray;
	int32 ExistingIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UpdatedRawArray.Num(); ++Index)
	{
		if (GetRawEntryName(UpdatedRawArray[Index]).Equals(MapKey, ESearchCase::IgnoreCase))
		{
			ExistingIndex = Index;
			break;
		}
	}

	const bool bUpdated = ExistingIndex != INDEX_NONE;
	bool bChanged = true;
	if (ExistingIndex != INDEX_NONE)
	{
		bChanged = !GetRawEntryGameMode(UpdatedRawArray[ExistingIndex]).Equals(ResolvedGameModeClassPath, ESearchCase::CaseSensitive);
		if (bChanged)
		{
			TSharedRef<FJsonObject> UpdatedObject = MakeShared<FJsonObject>();
			UpdatedObject->SetStringField(TEXT("Name"), MapKey);
			UpdatedObject->SetField(TEXT("GameMode"), BuildSoftClassPathJsonValue(ResolvedGameModeClassPath));
			UpdatedRawArray[ExistingIndex] = MakeShared<FJsonValueObject>(UpdatedObject);
		}
	}
	else
	{
		TSharedRef<FJsonObject> NewEntry = MakeShared<FJsonObject>();
		NewEntry->SetStringField(TEXT("Name"), MapKey);
		NewEntry->SetField(TEXT("GameMode"), BuildSoftClassPathJsonValue(ResolvedGameModeClassPath));
		UpdatedRawArray.Add(MakeShared<FJsonValueObject>(NewEntry));
	}

	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;
	TArray<FString> ChangedProperties;

	if (bChanged)
	{
		TSharedRef<FJsonObject> PatchEntry = MakeShared<FJsonObject>();
		PatchEntry->SetStringField(TEXT("op"), TEXT("replace"));
		PatchEntry->SetStringField(TEXT("path"), TEXT("/GameModeMapPrefixes"));
		PatchEntry->SetField(TEXT("value"), MakeShared<FJsonValueArray>(UpdatedRawArray));
		const TArray<TSharedPtr<FJsonValue>> PatchOperations{ MakeShared<FJsonValueObject>(PatchEntry) };

		FMCPDiagnostic PatchDiagnostic;
		if (Request.Context.bDryRun)
		{
			if (!ParseTopLevelPatchProperties(&PatchOperations, GameMapsSettings, ChangedProperties, PatchDiagnostic))
			{
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}
		else
		{
			const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Set Map Override"));
			FScopedTransaction Transaction(FText::FromString(TransactionLabel));
			GameMapsSettings->Modify();

			if (!MCPObjectUtils::ApplyPatch(GameMapsSettings, &PatchOperations, ChangedProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			GameMapsSettings->PostEditChange();
			GameMapsSettings->MarkPackageDirty();
			bPersistSucceeded = PersistConfigObject(GameMapsSettings, SaveOptions, SavedConfigFiles, bVerified, OutResult);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ResultRawArray = UpdatedRawArray;
	if (!Request.Context.bDryRun && !GetGameModeMapOverrideRawArray(GameMapsSettings, ResultRawArray))
	{
		ResultRawArray = UpdatedRawArray;
	}

	MCPObjectUtils::AppendTouchedPackage(GameMapsSettings, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("map_key"), MapKey);
	OutResult.ResultObject->SetBoolField(TEXT("updated"), bUpdated);
	OutResult.ResultObject->SetStringField(TEXT("game_mode_class_path"), ResolvedGameModeClassPath);
	OutResult.ResultObject->SetArrayField(TEXT("map_overrides"), BuildMapOverrideEntriesFromRaw(ResultRawArray));
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), MCPToolCommonJson::ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsSettingsGameModeHandler::HandleGameModeRemoveMapOverride(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FGetGameModeMapOverrideRawArrayFn GetGameModeMapOverrideRawArray,
	FBuildMapOverrideEntriesFromRawFn BuildMapOverrideEntriesFromRaw,
	FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
	FPersistConfigObjectFn PersistConfigObject)
{
	FString MapKey;
	FMCPToolSettingsSaveOptions SaveOptions;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("map_key"), MapKey);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	MapKey.TrimStartAndEndInline();
	if (MapKey.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("map_key is required for settings.gamemode.remove_map_override.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CurrentRawArray;
	if (!GetGameModeMapOverrideRawArray(GameMapsSettings, CurrentRawArray))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Message = TEXT("Failed to inspect GameModeMapPrefixes from UGameMapsSettings.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	auto GetRawEntryName = [](const TSharedPtr<FJsonValue>& EntryValue) -> FString
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		FString Name;
		EntryObject->TryGetStringField(TEXT("Name"), Name);
		if (Name.IsEmpty())
		{
			EntryObject->TryGetStringField(TEXT("name"), Name);
		}
		return Name;
	};

	TArray<TSharedPtr<FJsonValue>> UpdatedRawArray;
	UpdatedRawArray.Reserve(CurrentRawArray.Num());
	int32 RemovedCount = 0;
	for (const TSharedPtr<FJsonValue>& EntryValue : CurrentRawArray)
	{
		if (GetRawEntryName(EntryValue).Equals(MapKey, ESearchCase::IgnoreCase))
		{
			RemovedCount++;
			continue;
		}
		UpdatedRawArray.Add(EntryValue);
	}

	if (RemovedCount == 0)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::GAMEMODE_OVERRIDE_NOT_FOUND;
		Diagnostic.Message = TEXT("No map override entry matched map_key.");
		Diagnostic.Detail = MapKey;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;
	TArray<FString> ChangedProperties;

	TSharedRef<FJsonObject> PatchEntry = MakeShared<FJsonObject>();
	PatchEntry->SetStringField(TEXT("op"), TEXT("replace"));
	PatchEntry->SetStringField(TEXT("path"), TEXT("/GameModeMapPrefixes"));
	PatchEntry->SetField(TEXT("value"), MakeShared<FJsonValueArray>(UpdatedRawArray));
	const TArray<TSharedPtr<FJsonValue>> PatchOperations{ MakeShared<FJsonValueObject>(PatchEntry) };

	FMCPDiagnostic PatchDiagnostic;
	if (Request.Context.bDryRun)
	{
		if (!ParseTopLevelPatchProperties(&PatchOperations, GameMapsSettings, ChangedProperties, PatchDiagnostic))
		{
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}
	else
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Remove Map Override"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		GameMapsSettings->Modify();

		if (!MCPObjectUtils::ApplyPatch(GameMapsSettings, &PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		GameMapsSettings->PostEditChange();
		GameMapsSettings->MarkPackageDirty();
		bPersistSucceeded = PersistConfigObject(GameMapsSettings, SaveOptions, SavedConfigFiles, bVerified, OutResult);
	}

	TArray<TSharedPtr<FJsonValue>> ResultRawArray = UpdatedRawArray;
	if (!Request.Context.bDryRun && !GetGameModeMapOverrideRawArray(GameMapsSettings, ResultRawArray))
	{
		ResultRawArray = UpdatedRawArray;
	}

	MCPObjectUtils::AppendTouchedPackage(GameMapsSettings, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("map_key"), MapKey);
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemovedCount);
	OutResult.ResultObject->SetArrayField(TEXT("map_overrides"), BuildMapOverrideEntriesFromRaw(ResultRawArray));
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), MCPToolCommonJson::ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}
