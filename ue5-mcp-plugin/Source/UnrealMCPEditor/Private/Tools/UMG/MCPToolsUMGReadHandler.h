#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsUMGReadHandler
{
	static bool HandleWidgetClassList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleTreeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleWidgetInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSlotInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleGraphSummary(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
