#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class UMCPToolRegistrySubsystem;

struct FMCPToolsCoreHandler
{
	static bool HandleToolsList(
		const UMCPToolRegistrySubsystem& Registry,
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult);
	static bool HandleSystemHealth(
		const UMCPToolRegistrySubsystem& Registry,
		int32 RegisteredToolCount,
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult);
	static bool HandleEditorLiveCodingCompile(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
