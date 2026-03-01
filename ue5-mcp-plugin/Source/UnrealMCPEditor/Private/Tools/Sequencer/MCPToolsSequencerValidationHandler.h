#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsSequencerValidationHandler
{
	static bool HandleValidate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
