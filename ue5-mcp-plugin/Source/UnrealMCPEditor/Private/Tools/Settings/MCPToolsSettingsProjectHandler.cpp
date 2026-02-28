#include "Tools/Settings/MCPToolsSettingsProjectHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

namespace
{
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
}

bool FMCPToolsSettingsProjectHandler::HandleProjectGet(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FResolveSettingsClassFn ResolveSettingsClass)
{
	FString ClassPath;
	const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
	int32 Depth = 2;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("class_path"), ClassPath);
		Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);
	}

	if (ClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("class_path is required for settings.project.get.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* SettingsClass = nullptr;
	UObject* ConfigObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveSettingsClass(ClassPath, SettingsClass, ConfigObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Properties;
	MCPObjectUtils::InspectObject(ConfigObject, (FiltersObject != nullptr) ? *FiltersObject : nullptr, Depth, Properties);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("class_path"), SettingsClass->GetClassPathName().ToString());
	OutResult.ResultObject->SetStringField(TEXT("config_name"), SettingsClass->ClassConfigName.ToString());
	OutResult.ResultObject->SetStringField(TEXT("config_file"), ConfigObject->GetDefaultConfigFilename());
	OutResult.ResultObject->SetArrayField(TEXT("properties"), Properties);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSettingsProjectHandler::HandleProjectPatch(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FResolveSettingsClassFn ResolveSettingsClass,
	FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
	FBuildSettingsPatchSignatureFn BuildSettingsPatchSignature,
	FBuildSettingsConfirmationTokenFn BuildSettingsConfirmationToken,
	FConsumeSettingsConfirmationTokenFn ConsumeSettingsConfirmationToken,
	FPersistConfigObjectFn PersistConfigObject)
{
	FString ClassPath;
	FString Mode = TEXT("preview");
	FString ConfirmToken;
	FMCPToolSettingsSaveOptions SaveOptions;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("class_path"), ClassPath);
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetStringField(TEXT("confirm_token"), ConfirmToken);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	if (ClassPath.IsEmpty() || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("class_path and patch are required for settings.project.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	Mode = Mode.ToLower();
	if (!Mode.Equals(TEXT("preview")) && !Mode.Equals(TEXT("apply")))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("mode must be preview or apply.");
		Diagnostic.Detail = Mode;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* SettingsClass = nullptr;
	UObject* ConfigObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveSettingsClass(ClassPath, SettingsClass, ConfigObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> PreviewChangedProperties;
	FMCPDiagnostic ParsePatchDiagnostic;
	if (!ParseTopLevelPatchProperties(PatchOperations, ConfigObject, PreviewChangedProperties, ParsePatchDiagnostic))
	{
		OutResult.Diagnostics.Add(ParsePatchDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString Signature = BuildSettingsPatchSignature(SettingsClass->GetClassPathName().ToString(), PatchOperations, SaveOptions);
	if (Mode.Equals(TEXT("apply")) && !ConsumeSettingsConfirmationToken(ConfirmToken, Signature))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CONFIRM_TOKEN_INVALID;
		Diagnostic.Message = TEXT("Invalid or expired confirm_token.");
		Diagnostic.Suggestion = TEXT("Run settings.project.patch in preview mode and retry with returned confirm_token.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString PreviewToken;
	TArray<FString> ChangedProperties = PreviewChangedProperties;
	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;

	if (Mode.Equals(TEXT("preview")))
	{
		PreviewToken = BuildSettingsConfirmationToken(Signature);
	}
	else if (!Request.Context.bDryRun)
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP Project Settings Patch"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		ConfigObject->Modify();

		FMCPDiagnostic PatchDiagnostic;
		if (!MCPObjectUtils::ApplyPatch(ConfigObject, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		ConfigObject->PostEditChange();
		ConfigObject->MarkPackageDirty();
		bPersistSucceeded = PersistConfigObject(ConfigObject, SaveOptions, SavedConfigFiles, bVerified, OutResult);
	}

	MCPObjectUtils::AppendTouchedPackage(ConfigObject, OutResult.TouchedPackages);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("mode"), Mode);
	OutResult.ResultObject->SetStringField(TEXT("class_path"), SettingsClass->GetClassPathName().ToString());
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetStringField(TEXT("confirm_token"), PreviewToken);
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), MCPToolCommonJson::ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));

	if (!Mode.Equals(TEXT("preview")) && SaveOptions.bReloadVerify && SaveOptions.bSaveConfig && !bVerified)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Message = TEXT("Settings reload verification did not run.");
		Diagnostic.Detail = SettingsClass->GetClassPathName().ToString();
		OutResult.Diagnostics.Add(Diagnostic);
	}

	if (Mode.Equals(TEXT("preview")))
	{
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsSettingsProjectHandler::HandleProjectApply(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FResolveSettingsClassFn ResolveSettingsClass,
	FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
	FBuildSettingsPatchSignatureFn BuildSettingsPatchSignature,
	FBuildSettingsConfirmationTokenFn BuildSettingsConfirmationToken,
	FConsumeSettingsConfirmationTokenFn ConsumeSettingsConfirmationToken,
	FPersistConfigObjectFn PersistConfigObject)
{
	if (!Request.Params.IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("params are required for settings.project.apply.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPRequestEnvelope ApplyRequest = Request;
	ApplyRequest.Params = MakeShared<FJsonObject>(*Request.Params);
	ApplyRequest.Params->SetStringField(TEXT("mode"), TEXT("apply"));
	return HandleProjectPatch(
		ApplyRequest,
		OutResult,
		ResolveSettingsClass,
		ParseTopLevelPatchProperties,
		BuildSettingsPatchSignature,
		BuildSettingsConfirmationToken,
		ConsumeSettingsConfirmationToken,
		PersistConfigObject);
}
