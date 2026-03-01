#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsSequencerStructureHandler
{
	static bool HandleBindingAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleBindingRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleTrackAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleTrackRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSectionAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSectionPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSectionRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandlePlaybackPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
