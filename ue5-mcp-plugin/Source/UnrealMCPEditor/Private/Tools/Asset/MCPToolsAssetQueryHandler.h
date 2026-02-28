#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

class UPackage;

struct FMCPToolsAssetQueryHandler
{
	using FResolvePackageByNameFn = TFunctionRef<UPackage*(const FString&)>;
	using FNormalizePackageNameFn = TFunctionRef<FString(const FString&)>;

	static bool HandleFind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSave(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FResolvePackageByNameFn ResolvePackageByName,
		FNormalizePackageNameFn NormalizePackageNameFromInput);
};
