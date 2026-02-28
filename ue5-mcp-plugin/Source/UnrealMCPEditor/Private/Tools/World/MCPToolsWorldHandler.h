#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsWorldHandler
{
	static bool HandleOutlinerList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSelectionGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSelectionSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
