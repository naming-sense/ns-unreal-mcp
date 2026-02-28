#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsObjectHandler
{
	static bool HandleInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandlePatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandlePatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
