#pragma once

#include "CoreMinimal.h"
#if __has_include("Subsystems/EditorSubsystem.h")
#include "Subsystems/EditorSubsystem.h"
#elif __has_include("EditorSubsystem.h")
#include "EditorSubsystem.h"
#else
#error "EditorSubsystem header not found. Check UnrealEd dependency."
#endif
#include "Dom/JsonObject.h"
#include "MCPEventStreamSubsystem.generated.h"

struct FMCPStreamEvent
{
	FString EventId;
	FString EventType;
	FString RequestId;
	int64 TimestampMs = 0;
	TSharedPtr<FJsonObject> Payload;

	TSharedRef<FJsonObject> ToJson() const;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FMCPStreamEventDelegate, const FMCPStreamEvent&);

UCLASS()
class UNREALMCPEDITOR_API UMCPEventStreamSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	void EmitProgress(const FString& RequestId, double Percent, const FString& Phase);
	void EmitLog(const FString& RequestId, const FString& Level, const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr);
	void EmitArtifact(const FString& RequestId, const FString& ObjectPath, const FString& Action);
	void EmitJobStatus(
		const FString& RequestId,
		const FString& JobId,
		const FString& Status,
		double Progress,
		const FString& StartedAtIso8601,
		const FString& UpdatedAtIso8601);
	void EmitChangeSetCreated(const FString& RequestId, const FString& ChangeSetId, const FString& Path);

	TArray<TSharedPtr<FJsonObject>> GetRecentEvents(int32 Limit) const;
	TSharedRef<FJsonObject> BuildSnapshot(int32 RecentLimit) const;

	int32 GetBufferedEventCount() const;
	int64 GetTotalEmittedEventCount() const;
	int64 GetDroppedEventCount() const;

	FMCPStreamEventDelegate& OnEventEmitted();

private:
	void EmitEvent(const FString& EventType, const FString& RequestId, const TSharedRef<FJsonObject>& Payload);
	static int64 GetCurrentUnixTimestampMs();

private:
	mutable FCriticalSection EventGuard;
	TArray<FMCPStreamEvent> EventBuffer;
	int32 MaxBufferedEvents = 256;
	int64 TotalEmittedEvents = 0;
	int64 DroppedEvents = 0;
	FMCPStreamEventDelegate EventDelegate;
};

