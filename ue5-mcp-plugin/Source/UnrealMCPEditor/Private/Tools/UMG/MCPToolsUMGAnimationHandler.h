#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

struct FMCPToolsUMGAnimationHandler
{
	using FSavePackageByNameFn = TFunctionRef<bool(const FString&, FMCPToolExecutionResult&)>;

	static bool HandleAnimationList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleAnimationCreate(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleAnimationRemove(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleAnimationTrackAdd(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleAnimationKeySet(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleAnimationKeyRemove(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FSavePackageByNameFn SavePackageByName);
};
