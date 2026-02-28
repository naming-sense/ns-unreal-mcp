#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsOpsHandler
{
	static bool HandleChangeSetList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleChangeSetGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleChangeSetRollbackPreview(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleChangeSetRollbackApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);

	static bool HandleJobGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleJobCancel(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
