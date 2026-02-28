#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

class UObject;
class UClass;

struct FMCPToolsAssetLifecycleHandler
{
	using FIsValidAssetDestinationFn = TFunctionRef<bool(const FString&, const FString&)>;
	using FBuildPackageNameFn = TFunctionRef<FString(const FString&, const FString&)>;
	using FBuildObjectPathFn = TFunctionRef<FString(const FString&, const FString&)>;
	using FNormalizePackageNameFromInputFn = TFunctionRef<FString(const FString&)>;
	using FResolveObjectByPathFn = TFunctionRef<UObject*(const FString&)>;
	using FResolveClassByPathFn = TFunctionRef<UClass*(const FString&)>;
	using FSavePackageByNameFn = TFunctionRef<bool(const FString&, FMCPToolExecutionResult&)>;
	using FBuildDeleteConfirmationTokenFn = TFunctionRef<FString(const TArray<FString>&, bool)>;
	using FConsumeDeleteConfirmationTokenFn = TFunctionRef<bool(const FString&, const TArray<FString>&, bool)>;

	static bool HandleDuplicate(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FIsValidAssetDestinationFn IsValidAssetDestination,
		FBuildPackageNameFn BuildPackageName,
		FBuildObjectPathFn BuildObjectPath,
		FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
		FResolveObjectByPathFn ResolveObjectByPath,
		FSavePackageByNameFn SavePackageByName);

	static bool HandleImport(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FIsValidAssetDestinationFn IsValidAssetDestination,
		FBuildPackageNameFn BuildPackageName,
		FBuildObjectPathFn BuildObjectPath,
		FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
		FResolveObjectByPathFn ResolveObjectByPath,
		FSavePackageByNameFn SavePackageByName);

	static bool HandleCreate(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FIsValidAssetDestinationFn IsValidAssetDestination,
		FBuildPackageNameFn BuildPackageName,
		FBuildObjectPathFn BuildObjectPath,
		FResolveObjectByPathFn ResolveObjectByPath,
		FResolveClassByPathFn ResolveClassByPath,
		FSavePackageByNameFn SavePackageByName);

	static bool HandleRename(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FIsValidAssetDestinationFn IsValidAssetDestination,
		FBuildPackageNameFn BuildPackageName,
		FBuildObjectPathFn BuildObjectPath,
		FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
		FResolveObjectByPathFn ResolveObjectByPath,
		FSavePackageByNameFn SavePackageByName);

	static bool HandleDelete(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
		FResolveObjectByPathFn ResolveObjectByPath,
		FBuildDeleteConfirmationTokenFn BuildDeleteConfirmationToken,
		FConsumeDeleteConfirmationTokenFn ConsumeDeleteConfirmationToken);
};
