#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

class UClass;
class UObject;

struct FMCPToolSettingsSaveOptions
{
	bool bSaveConfig = true;
	bool bFlushIni = true;
	bool bReloadVerify = true;
};

struct FMCPToolsSettingsProjectHandler
{
	using FResolveSettingsClassFn = TFunctionRef<bool(const FString&, UClass*&, UObject*&, FMCPDiagnostic&)>;
	using FParseTopLevelPatchPropertiesFn =
		TFunctionRef<bool(const TArray<TSharedPtr<FJsonValue>>*, UObject*, TArray<FString>&, FMCPDiagnostic&)>;
	using FBuildSettingsPatchSignatureFn =
		TFunctionRef<FString(const FString&, const TArray<TSharedPtr<FJsonValue>>*, const FMCPToolSettingsSaveOptions&)>;
	using FBuildSettingsConfirmationTokenFn = TFunctionRef<FString(const FString&)>;
	using FConsumeSettingsConfirmationTokenFn = TFunctionRef<bool(const FString&, const FString&)>;
	using FPersistConfigObjectFn =
		TFunctionRef<bool(UObject*, const FMCPToolSettingsSaveOptions&, TArray<FString>&, bool&, FMCPToolExecutionResult&)>;

	static bool HandleProjectGet(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FResolveSettingsClassFn ResolveSettingsClass);

	static bool HandleProjectPatch(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FResolveSettingsClassFn ResolveSettingsClass,
		FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
		FBuildSettingsPatchSignatureFn BuildSettingsPatchSignature,
		FBuildSettingsConfirmationTokenFn BuildSettingsConfirmationToken,
		FConsumeSettingsConfirmationTokenFn ConsumeSettingsConfirmationToken,
		FPersistConfigObjectFn PersistConfigObject);

	static bool HandleProjectApply(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FResolveSettingsClassFn ResolveSettingsClass,
		FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
		FBuildSettingsPatchSignatureFn BuildSettingsPatchSignature,
		FBuildSettingsConfirmationTokenFn BuildSettingsConfirmationToken,
		FConsumeSettingsConfirmationTokenFn ConsumeSettingsConfirmationToken,
		FPersistConfigObjectFn PersistConfigObject);
};
