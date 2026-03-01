#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

struct FMCPToolsSequencerReadHandler
{
	static bool HandleAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleTrackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleSectionList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
	static bool HandleChannelList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult);
};
