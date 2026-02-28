#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"
#include "Tools/Settings/MCPToolsSettingsProjectHandler.h"

class UObject;

struct FMCPToolsSettingsGameModeHandler
{
	using FResolveGameModeClassPathFn = TFunctionRef<bool(const FString&, FString&, FMCPDiagnostic&)>;
	using FGetSettingsStringPropertyFn = TFunctionRef<FString(UObject*, const FString&)>;
	using FGetGameModeMapOverrideRawArrayFn = TFunctionRef<bool(UObject*, TArray<TSharedPtr<FJsonValue>>&)>;
	using FTryGetMapOverrideGameModeClassPathFn = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, FString&)>;
	using FBuildSoftClassPathJsonValueFn = TFunctionRef<TSharedRef<FJsonValueObject>(const FString&)>;
	using FBuildMapOverrideEntriesFromRawFn = TFunctionRef<TArray<TSharedPtr<FJsonValue>>(const TArray<TSharedPtr<FJsonValue>>&)>;
	using FParseTopLevelPatchPropertiesFn =
		TFunctionRef<bool(const TArray<TSharedPtr<FJsonValue>>*, UObject*, TArray<FString>&, FMCPDiagnostic&)>;
	using FPersistConfigObjectFn =
		TFunctionRef<bool(UObject*, const FMCPToolSettingsSaveOptions&, TArray<FString>&, bool&, FMCPToolExecutionResult&)>;

	static bool HandleGameModeGet(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FGetSettingsStringPropertyFn GetSettingsStringProperty,
		FGetGameModeMapOverrideRawArrayFn GetGameModeMapOverrideRawArray,
		FBuildMapOverrideEntriesFromRawFn BuildMapOverrideEntriesFromRaw);

	static bool HandleGameModeSetDefault(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FResolveGameModeClassPathFn ResolveGameModeClassPath,
		FGetSettingsStringPropertyFn GetSettingsStringProperty,
		FPersistConfigObjectFn PersistConfigObject);

	static bool HandleGameModeSetMapOverride(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FResolveGameModeClassPathFn ResolveGameModeClassPath,
		FGetGameModeMapOverrideRawArrayFn GetGameModeMapOverrideRawArray,
		FTryGetMapOverrideGameModeClassPathFn TryGetMapOverrideGameModeClassPath,
		FBuildSoftClassPathJsonValueFn BuildSoftClassPathJsonValue,
		FBuildMapOverrideEntriesFromRawFn BuildMapOverrideEntriesFromRaw,
		FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
		FPersistConfigObjectFn PersistConfigObject);

	static bool HandleGameModeRemoveMapOverride(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FGetGameModeMapOverrideRawArrayFn GetGameModeMapOverrideRawArray,
		FBuildMapOverrideEntriesFromRawFn BuildMapOverrideEntriesFromRaw,
		FParseTopLevelPatchPropertiesFn ParseTopLevelPatchProperties,
		FPersistConfigObjectFn PersistConfigObject);
};
