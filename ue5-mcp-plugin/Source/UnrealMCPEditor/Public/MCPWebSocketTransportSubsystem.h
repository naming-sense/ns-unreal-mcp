#pragma once

#include "CoreMinimal.h"
#if __has_include("Subsystems/EditorSubsystem.h")
#include "Subsystems/EditorSubsystem.h"
#elif __has_include("EditorSubsystem.h")
#include "EditorSubsystem.h"
#else
#error "EditorSubsystem header not found. Check UnrealEd dependency."
#endif
#include "IWebSocketServer.h"
#include "WebSocketNetworkingDelegates.h"
#include "Containers/Ticker.h"
#include "MCPWebSocketTransportSubsystem.generated.h"

class INetworkingWebSocket;
struct FMCPStreamEvent;

UCLASS()
class UNREALMCPEDITOR_API UMCPWebSocketTransportSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsEnabled() const;
	bool IsListening() const;
	uint16 GetListenPort() const;
	FString GetBindAddress() const;
	int32 GetClientCount() const;

private:
	void LoadSettings();
	void StartServer();
	void StopServer();
	bool HandleTicker(float DeltaSeconds);

	void HandleStreamEvent(const FMCPStreamEvent& Event);
	void OnClientConnected(INetworkingWebSocket* Socket);
	void OnClientPacketReceived(void* Data, int32 Size, uint16 ConnectionId);
	void OnClientClosed(uint16 ConnectionId);

	bool SendToConnection(uint16 ConnectionId, const FString& MessageJson);
	void BroadcastToClients(const FString& MessageJson);
	void WriteConnectionInfoFile() const;
	FString ResolveConnectHost() const;

	FString BuildWelcomePayload(uint16 ConnectionId) const;
	FString BuildErrorPayload(const FString& Code, const FString& Message) const;
	static int64 GetCurrentUnixTimestampMs();

private:
	mutable FCriticalSection ConnectionGuard;
	TMap<uint16, INetworkingWebSocket*> Connections;
	uint16 NextConnectionId = 100;

	TUniquePtr<IWebSocketServer> Server;
	FWebSocketClientConnectedCallBack ClientConnectedCallback;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle EventDelegateHandle;

	bool bEnabled = true;
	bool bListening = false;
	uint16 PreferredPort = 19090;
	uint16 ListeningPort = 0;
	FString BindAddress = TEXT("127.0.0.1");
	int32 MaxPortScan = 20;
};
