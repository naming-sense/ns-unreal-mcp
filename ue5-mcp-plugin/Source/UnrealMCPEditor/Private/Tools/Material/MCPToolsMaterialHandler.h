#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsMaterialHandler
{
	static bool HandleInstanceParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleInstanceParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
