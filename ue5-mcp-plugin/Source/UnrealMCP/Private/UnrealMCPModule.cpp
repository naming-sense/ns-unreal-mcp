#include "MCPLog.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

class FUnrealMCPModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP runtime module started."));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP runtime module stopped."));
	}
};

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP);
