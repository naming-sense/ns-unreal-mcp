#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

struct FMCPToolsUMGBindingHandler
{
	using FSavePackageByNameFn = TFunctionRef<bool(const FString&, FMCPToolExecutionResult&)>;

	static bool HandleBindingSet(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleBindingClear(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleWidgetEventBind(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleWidgetEventUnbind(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
};
