#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"
#include "Tools/Settings/MCPToolsSettingsProjectHandler.h"

class UBlueprint;
class UClass;
class UObject;

struct FMCPComposeBlueprintRequest
{
	FString PackagePath;
	FString AssetName;
	UClass* ParentClass = nullptr;
	bool bOverwrite = false;
	bool bDryRun = false;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	FString TransactionLabel;
};

struct FMCPComposeBlueprintResult
{
	bool bCreatedNew = false;
	UBlueprint* BlueprintAsset = nullptr;
	FString ObjectPath;
	FString PackageName;
	FString ParentClassPath;
	FString GeneratedClassPath;
	bool bSaved = true;
};

struct FMCPToolsSettingsComposeHandler
{
	using FParseAutoSaveOptionFn = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, bool&)>;
	using FParseSettingsSaveOptionsFn = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, FMCPToolSettingsSaveOptions&)>;
	using FParseTransactionLabelFn = TFunctionRef<FString(const TSharedPtr<FJsonObject>&, const FString&)>;
	using FIsValidAssetDestinationFn = TFunctionRef<bool(const FString&, const FString&)>;
	using FResolveClassByPathFn = TFunctionRef<UClass*(const FString&)>;
	using FEnsureBlueprintAssetFn =
		TFunctionRef<bool(const FMCPComposeBlueprintRequest&, FMCPToolExecutionResult&, FMCPComposeBlueprintResult&, FMCPDiagnostic&)>;
	using FResolveClassPathWithBaseFn = TFunctionRef<bool(const FString&, UClass*, FString&, UClass*&, FMCPDiagnostic&)>;
	using FSetClassPropertyOnObjectFn = TFunctionRef<bool(UObject*, const FString&, UClass*, FMCPDiagnostic&)>;
	using FSavePackageByNameFn = TFunctionRef<bool(const FString&, FMCPToolExecutionResult&)>;
	using FHandleSettingsGameModeSetDefaultFn = TFunctionRef<bool(const FMCPRequestEnvelope&, FMCPToolExecutionResult&)>;
	using FHandleSettingsGameModeSetMapOverrideFn = TFunctionRef<bool(const FMCPRequestEnvelope&, FMCPToolExecutionResult&)>;
	using FParseStringArrayFieldFn = TFunctionRef<bool(const TSharedPtr<FJsonObject>&, const FString&, TArray<FString>&)>;

	static bool HandleGameModeCompose(
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
		FParseStringArrayFieldFn ParseStringArrayField);
};
