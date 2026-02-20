#include "Modules/ModuleManager.h"
#include "MCPLog.h"

class FUnrealMCPEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCPEditor module started."));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCPEditor module stopped."));
	}
};

IMPLEMENT_MODULE(FUnrealMCPEditorModule, UnrealMCPEditor);
