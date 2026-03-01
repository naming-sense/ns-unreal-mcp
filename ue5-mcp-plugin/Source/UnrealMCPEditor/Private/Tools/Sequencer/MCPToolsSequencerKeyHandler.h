#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsSequencerKeyHandler
{
	static bool HandleKeySet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleKeyRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleKeyBulkSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
