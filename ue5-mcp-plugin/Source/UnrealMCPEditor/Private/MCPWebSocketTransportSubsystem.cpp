#include "MCPWebSocketTransportSubsystem.h"

#include "Editor.h"
#include "INetworkingWebSocket.h"
#include "IWebSocketNetworkingModule.h"
#include "IWebSocketServer.h"
#include "MCPCommandRouterSubsystem.h"
#include "MCPEventStreamSubsystem.h"
#include "MCPLog.h"
#include "MCPTypes.h"
#include "Modules/ModuleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString SerializeJsonObject(const TSharedRef<FJsonObject>& JsonObject)
	{
		FString SerializedJson;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SerializedJson);
		FJsonSerializer::Serialize(JsonObject, Writer);
		return SerializedJson;
	}

	FString NormalizeConnectHost(const FString& BindAddress)
	{
		if (BindAddress.IsEmpty()
			|| BindAddress.Equals(TEXT("0.0.0.0"))
			|| BindAddress.Equals(TEXT("::"))
			|| BindAddress.Equals(TEXT("[::]"))
			|| BindAddress.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
		{
			return TEXT("127.0.0.1");
		}

		return BindAddress;
	}

	template<typename TWebSocketServer>
	auto InitWebSocketServerCompatImpl(
		TWebSocketServer* InServer,
		const uint32 InPort,
		const FWebSocketClientConnectedCallBack& InClientConnectedCallback,
		const FString& InBindAddress,
		int)
		-> decltype(InServer->Init(InPort, InClientConnectedCallback, InBindAddress), bool())
	{
		return InServer->Init(InPort, InClientConnectedCallback, InBindAddress);
	}

	template<typename TWebSocketServer>
	bool InitWebSocketServerCompatImpl(
		TWebSocketServer* InServer,
		const uint32 InPort,
		const FWebSocketClientConnectedCallBack& InClientConnectedCallback,
		const FString& InBindAddress,
		...)
	{
		(void)InBindAddress;
		return InServer->Init(InPort, InClientConnectedCallback);
	}

	bool InitWebSocketServerCompat(
		IWebSocketServer* InServer,
		const uint32 InPort,
		const FWebSocketClientConnectedCallBack& InClientConnectedCallback,
		const FString& InBindAddress)
	{
		if (InServer == nullptr)
		{
			return false;
		}

		return InitWebSocketServerCompatImpl(InServer, InPort, InClientConnectedCallback, InBindAddress, 0);
	}
}

void UMCPWebSocketTransportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LoadSettings();

	if (GEditor != nullptr)
	{
		if (UMCPEventStreamSubsystem* EventStreamSubsystem = GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>())
		{
			EventDelegateHandle = EventStreamSubsystem->OnEventEmitted().AddUObject(this, &UMCPWebSocketTransportSubsystem::HandleStreamEvent);
		}
	}

	if (bEnabled)
	{
		StartServer();
	}
}

void UMCPWebSocketTransportSubsystem::Deinitialize()
{
	if (GEditor != nullptr)
	{
		if (UMCPEventStreamSubsystem* EventStreamSubsystem = GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>())
		{
			if (EventDelegateHandle.IsValid())
			{
				EventStreamSubsystem->OnEventEmitted().Remove(EventDelegateHandle);
			}
		}
	}
	EventDelegateHandle.Reset();

	StopServer();
	Super::Deinitialize();
}

bool UMCPWebSocketTransportSubsystem::IsEnabled() const
{
	return bEnabled;
}

bool UMCPWebSocketTransportSubsystem::IsListening() const
{
	return bListening;
}

uint16 UMCPWebSocketTransportSubsystem::GetListenPort() const
{
	return ListeningPort;
}

FString UMCPWebSocketTransportSubsystem::GetBindAddress() const
{
	return BindAddress;
}

int32 UMCPWebSocketTransportSubsystem::GetClientCount() const
{
	FScopeLock ScopeLock(&ConnectionGuard);
	return Connections.Num();
}

void UMCPWebSocketTransportSubsystem::LoadSettings()
{
	const TCHAR* Section = TEXT("UnrealMCP.WebSocketTransport");

	bool bConfiguredEnabled = bEnabled;
	if (GConfig->GetBool(Section, TEXT("bEnabled"), bConfiguredEnabled, GEditorPerProjectIni))
	{
		bEnabled = bConfiguredEnabled;
	}

	int32 ConfiguredPort = static_cast<int32>(PreferredPort);
	if (GConfig->GetInt(Section, TEXT("Port"), ConfiguredPort, GEditorPerProjectIni))
	{
		PreferredPort = static_cast<uint16>(FMath::Clamp(ConfiguredPort, 1024, 65535));
	}

	FString ConfiguredBindAddress;
	if (GConfig->GetString(Section, TEXT("BindAddress"), ConfiguredBindAddress, GEditorPerProjectIni) && !ConfiguredBindAddress.IsEmpty())
	{
		BindAddress = ConfiguredBindAddress;
	}

	int32 ConfiguredPortScan = MaxPortScan;
	if (GConfig->GetInt(Section, TEXT("MaxPortScan"), ConfiguredPortScan, GEditorPerProjectIni))
	{
		MaxPortScan = FMath::Clamp(ConfiguredPortScan, 1, 128);
	}
}

void UMCPWebSocketTransportSubsystem::StartServer()
{
	if (bListening || !bEnabled)
	{
		return;
	}

	if (!FModuleManager::Get().ModuleExists(TEXT("WebSocketNetworking")))
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("WebSocketNetworking module is not available. WS event push disabled."));
		return;
	}

	IWebSocketNetworkingModule& WebSocketModule = FModuleManager::LoadModuleChecked<IWebSocketNetworkingModule>(TEXT("WebSocketNetworking"));

	const uint32 StartPort = static_cast<uint32>(PreferredPort);
	for (int32 Attempt = 0; Attempt < MaxPortScan; ++Attempt)
	{
		const uint32 CandidatePort = StartPort + static_cast<uint32>(Attempt);
		TUniquePtr<IWebSocketServer> CandidateServer = WebSocketModule.CreateServer();
		if (!CandidateServer.IsValid())
		{
			continue;
		}

		ClientConnectedCallback.Unbind();
		ClientConnectedCallback.BindUObject(this, &UMCPWebSocketTransportSubsystem::OnClientConnected);
			if (InitWebSocketServerCompat(CandidateServer.Get(), CandidatePort, ClientConnectedCallback, BindAddress))
			{
			Server = MoveTemp(CandidateServer);
			bListening = true;
			ListeningPort = static_cast<uint16>(CandidatePort);
			break;
		}
	}

	if (!bListening || !Server.IsValid())
	{
		Server.Reset();
		ClientConnectedCallback.Unbind();
		UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to start MCP WS transport server. bind=%s start_port=%d"), *BindAddress, PreferredPort);
		return;
	}

	if (!TickHandle.IsValid())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UMCPWebSocketTransportSubsystem::HandleTicker), 0.0f);
	}

	WriteConnectionInfoFile();
	UE_LOG(LogUnrealMCP, Log, TEXT("Started MCP WS event transport at ws://%s:%d"), *BindAddress, ListeningPort);
}

void UMCPWebSocketTransportSubsystem::StopServer()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	ClientConnectedCallback.Unbind();

	TArray<INetworkingWebSocket*> SocketsToDelete;
	{
		FScopeLock ScopeLock(&ConnectionGuard);
		Connections.GenerateValueArray(SocketsToDelete);
		Connections.Reset();
	}

	for (INetworkingWebSocket* Socket : SocketsToDelete)
	{
		delete Socket;
	}

	Server.Reset();
	bListening = false;
	ListeningPort = 0;
}

bool UMCPWebSocketTransportSubsystem::HandleTicker(float DeltaSeconds)
{
	(void)DeltaSeconds;

	if (Server.IsValid())
	{
		Server->Tick();
		return true;
	}

	return false;
}

void UMCPWebSocketTransportSubsystem::HandleStreamEvent(const FMCPStreamEvent& Event)
{
	if (!bListening)
	{
		return;
	}

	BroadcastToClients(SerializeJsonObject(Event.ToJson()));
}

void UMCPWebSocketTransportSubsystem::OnClientConnected(INetworkingWebSocket* Socket)
{
	if (Socket == nullptr)
	{
		return;
	}

	const uint16 ConnectionId = ++NextConnectionId;
	{
		FScopeLock ScopeLock(&ConnectionGuard);
		Connections.Add(ConnectionId, Socket);
	}

	FWebSocketPacketReceivedCallBack ReceiveCallback;
	ReceiveCallback.BindUObject(this, &UMCPWebSocketTransportSubsystem::OnClientPacketReceived, ConnectionId);
	Socket->SetReceiveCallBack(ReceiveCallback);

	FWebSocketInfoCallBack ClosedCallback;
	ClosedCallback.BindUObject(this, &UMCPWebSocketTransportSubsystem::OnClientClosed, ConnectionId);
	Socket->SetSocketClosedCallBack(ClosedCallback);

	SendToConnection(ConnectionId, BuildWelcomePayload(ConnectionId));
	UE_LOG(LogUnrealMCP, Log, TEXT("MCP WS client connected. id=%d remote=%s"), ConnectionId, *Socket->RemoteEndPoint(true));
}

void UMCPWebSocketTransportSubsystem::OnClientPacketReceived(void* Data, int32 Size, uint16 ConnectionId)
{
	if (Data == nullptr || Size <= 0)
	{
		return;
	}

	const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Data), Size);
	const FString Message(Converter.Length(), Converter.Get());

	TSharedPtr<FJsonObject> RequestObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, RequestObject) || !RequestObject.IsValid())
	{
		SendToConnection(ConnectionId, BuildErrorPayload(TEXT("MCP.SCHEMA.INVALID_PARAMS"), TEXT("Invalid websocket message JSON.")));
		return;
	}

	FString MessageType;
	RequestObject->TryGetStringField(TEXT("type"), MessageType);

	if (MessageType.Equals(TEXT("ping"), ESearchCase::IgnoreCase))
	{
		TSharedRef<FJsonObject> PongObject = MakeShared<FJsonObject>();
		PongObject->SetStringField(TEXT("type"), TEXT("pong"));
		PongObject->SetNumberField(TEXT("timestamp_ms"), static_cast<double>(GetCurrentUnixTimestampMs()));
		SendToConnection(ConnectionId, SerializeJsonObject(PongObject));
		return;
	}

	if (!MessageType.Equals(TEXT("mcp.request"), ESearchCase::IgnoreCase))
	{
		SendToConnection(ConnectionId, BuildErrorPayload(TEXT("MCP.TOOL.NOT_FOUND"), TEXT("Unsupported websocket message type.")));
		return;
	}

	UMCPCommandRouterSubsystem* Router = GEditor ? GEditor->GetEditorSubsystem<UMCPCommandRouterSubsystem>() : nullptr;
	if (Router == nullptr)
	{
		SendToConnection(ConnectionId, BuildErrorPayload(TEXT("MCP.INTERNAL.EXCEPTION"), TEXT("Command router subsystem is unavailable.")));
		return;
	}

	FString RequestJson;
	RequestObject->TryGetStringField(TEXT("request_json"), RequestJson);
	if (RequestJson.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* RequestEnvelope = nullptr;
		if (RequestObject->TryGetObjectField(TEXT("request"), RequestEnvelope) && RequestEnvelope != nullptr && RequestEnvelope->IsValid())
		{
			RequestJson = MCPJson::SerializeJsonObject(*RequestEnvelope);
		}
	}

	if (RequestJson.IsEmpty())
	{
		SendToConnection(ConnectionId, BuildErrorPayload(TEXT("MCP.SCHEMA.INVALID_PARAMS"), TEXT("mcp.request requires request_json or request object.")));
		return;
	}

	bool bSuccess = false;
	const FString ResponseJson = Router->ExecuteRequestJson(RequestJson, bSuccess);

	TSharedRef<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
	ResponseObject->SetStringField(TEXT("type"), TEXT("mcp.response"));
	ResponseObject->SetBoolField(TEXT("ok"), bSuccess);
	ResponseObject->SetStringField(TEXT("response_json"), ResponseJson);
	SendToConnection(ConnectionId, SerializeJsonObject(ResponseObject));
}

void UMCPWebSocketTransportSubsystem::OnClientClosed(uint16 ConnectionId)
{
	INetworkingWebSocket* SocketToDelete = nullptr;
	{
		FScopeLock ScopeLock(&ConnectionGuard);
		if (INetworkingWebSocket** FoundSocket = Connections.Find(ConnectionId))
		{
			SocketToDelete = *FoundSocket;
			Connections.Remove(ConnectionId);
		}
	}

	if (SocketToDelete != nullptr)
	{
		delete SocketToDelete;
	}

	UE_LOG(LogUnrealMCP, Log, TEXT("MCP WS client disconnected. id=%d"), ConnectionId);
}

bool UMCPWebSocketTransportSubsystem::SendToConnection(uint16 ConnectionId, const FString& MessageJson)
{
	INetworkingWebSocket* Socket = nullptr;
	{
		FScopeLock ScopeLock(&ConnectionGuard);
		if (INetworkingWebSocket** FoundSocket = Connections.Find(ConnectionId))
		{
			Socket = *FoundSocket;
		}
	}

	if (Socket == nullptr)
	{
		return false;
	}

	const FTCHARToUTF8 Utf8Message(*MessageJson);
	return Socket->Send(reinterpret_cast<const uint8*>(Utf8Message.Get()), static_cast<uint32>(Utf8Message.Length()), false);
}

void UMCPWebSocketTransportSubsystem::BroadcastToClients(const FString& MessageJson)
{
	TArray<uint16> ConnectionIds;
	{
		FScopeLock ScopeLock(&ConnectionGuard);
		Connections.GetKeys(ConnectionIds);
	}

	TArray<uint16> FailedConnectionIds;
	for (const uint16 ConnectionId : ConnectionIds)
	{
		if (!SendToConnection(ConnectionId, MessageJson))
		{
			FailedConnectionIds.Add(ConnectionId);
		}
	}

	for (const uint16 FailedConnectionId : FailedConnectionIds)
	{
		OnClientClosed(FailedConnectionId);
	}
}

void UMCPWebSocketTransportSubsystem::WriteConnectionInfoFile() const
{
	if (!bListening || ListeningPort == 0)
	{
		return;
	}

	const FString ConnectionDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP"));
	if (!IFileManager::Get().MakeDirectory(*ConnectionDir, true))
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to prepare connection info directory: %s"), *ConnectionDir);
		return;
	}

	const FString ConnectionFilePath = FPaths::Combine(ConnectionDir, TEXT("connection.json"));
	const FString ConnectHost = ResolveConnectHost();
	const FString ConnectUrl = FString::Printf(TEXT("ws://%s:%d"), *ConnectHost, ListeningPort);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1.0);
	Root->SetStringField(TEXT("source"), TEXT("UnrealMCP.WebSocketTransport"));
	Root->SetNumberField(TEXT("updated_at_ms"), static_cast<double>(GetCurrentUnixTimestampMs()));
	Root->SetStringField(TEXT("ws_url"), ConnectUrl);
	Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Root->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Root->SetStringField(TEXT("saved_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
	Root->SetNumberField(TEXT("process_id"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));

	TSharedRef<FJsonObject> Transport = MakeShared<FJsonObject>();
	Transport->SetStringField(TEXT("protocol"), TEXT("ws"));
	Transport->SetStringField(TEXT("bind_address"), BindAddress);
	Transport->SetNumberField(TEXT("port"), static_cast<double>(ListeningPort));
	Transport->SetStringField(TEXT("ws_url"), ConnectUrl);
	Root->SetObjectField(TEXT("transport"), Transport);

	const FString Payload = SerializeJsonObject(Root);
	if (!FFileHelper::SaveStringToFile(Payload, *ConnectionFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to write WS connection info file: %s"), *ConnectionFilePath);
		return;
	}

	UE_LOG(LogUnrealMCP, Verbose, TEXT("Wrote WS connection info file: %s"), *ConnectionFilePath);
}

FString UMCPWebSocketTransportSubsystem::ResolveConnectHost() const
{
	return NormalizeConnectHost(BindAddress);
}

FString UMCPWebSocketTransportSubsystem::BuildWelcomePayload(const uint16 ConnectionId) const
{
	TSharedRef<FJsonObject> WelcomeObject = MakeShared<FJsonObject>();
	WelcomeObject->SetStringField(TEXT("type"), TEXT("mcp.transport.connected"));
	WelcomeObject->SetNumberField(TEXT("connection_id"), static_cast<double>(ConnectionId));
	WelcomeObject->SetStringField(TEXT("bind_address"), BindAddress);
	WelcomeObject->SetNumberField(TEXT("port"), static_cast<double>(ListeningPort));
	WelcomeObject->SetNumberField(TEXT("timestamp_ms"), static_cast<double>(GetCurrentUnixTimestampMs()));
	return SerializeJsonObject(WelcomeObject);
}

FString UMCPWebSocketTransportSubsystem::BuildErrorPayload(const FString& Code, const FString& Message) const
{
	TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
	ErrorObject->SetStringField(TEXT("type"), TEXT("mcp.transport.error"));
	ErrorObject->SetStringField(TEXT("code"), Code);
	ErrorObject->SetStringField(TEXT("message"), Message);
	ErrorObject->SetNumberField(TEXT("timestamp_ms"), static_cast<double>(GetCurrentUnixTimestampMs()));
	return SerializeJsonObject(ErrorObject);
}

int64 UMCPWebSocketTransportSubsystem::GetCurrentUnixTimestampMs()
{
	return FDateTime::UtcNow().ToUnixTimestamp() * 1000LL;
}
