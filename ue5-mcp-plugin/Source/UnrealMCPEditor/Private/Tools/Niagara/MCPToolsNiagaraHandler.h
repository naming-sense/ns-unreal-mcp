#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsNiagaraHandler
{
	static bool HandleParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleStackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleStackModuleSetParam(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
