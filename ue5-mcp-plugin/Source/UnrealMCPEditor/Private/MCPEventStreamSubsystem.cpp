#include "MCPEventStreamSubsystem.h"

#include "Misc/Guid.h"

TSharedRef<FJsonObject> FMCPStreamEvent::ToJson() const
{
	TSharedRef<FJsonObject> EventObject = MakeShared<FJsonObject>();
	EventObject->SetStringField(TEXT("event_id"), EventId);
	EventObject->SetStringField(TEXT("event_type"), EventType);
	EventObject->SetStringField(TEXT("request_id"), RequestId);
	EventObject->SetNumberField(TEXT("timestamp_ms"), static_cast<double>(TimestampMs));
	EventObject->SetObjectField(TEXT("payload"), Payload.IsValid() ? Payload.ToSharedRef() : MakeShared<FJsonObject>());
	return EventObject;
}

void UMCPEventStreamSubsystem::EmitProgress(const FString& RequestId, const double Percent, const FString& Phase)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("percent"), FMath::Clamp(Percent, 0.0, 100.0));
	Payload->SetStringField(TEXT("phase"), Phase);
	EmitEvent(TEXT("event.progress"), RequestId, Payload);
}

void UMCPEventStreamSubsystem::EmitLog(
	const FString& RequestId,
	const FString& Level,
	const FString& Message,
	const TSharedPtr<FJsonObject>& Detail)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("level"), Level);
	Payload->SetStringField(TEXT("message"), Message);
	if (Detail.IsValid())
	{
		Payload->SetObjectField(TEXT("detail"), Detail.ToSharedRef());
	}
	EmitEvent(TEXT("event.log"), RequestId, Payload);
}

void UMCPEventStreamSubsystem::EmitArtifact(const FString& RequestId, const FString& ObjectPath, const FString& Action)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("object_path"), ObjectPath);
	Payload->SetStringField(TEXT("action"), Action);
	EmitEvent(TEXT("event.artifact"), RequestId, Payload);
}

void UMCPEventStreamSubsystem::EmitJobStatus(
	const FString& RequestId,
	const FString& JobId,
	const FString& Status,
	const double Progress,
	const FString& StartedAtIso8601,
	const FString& UpdatedAtIso8601)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("job_id"), JobId);
	Payload->SetStringField(TEXT("status"), Status);
	Payload->SetNumberField(TEXT("progress"), FMath::Clamp(Progress, 0.0, 100.0));
	Payload->SetStringField(TEXT("started_at"), StartedAtIso8601);
	Payload->SetStringField(TEXT("updated_at"), UpdatedAtIso8601);
	EmitEvent(TEXT("event.job.status"), RequestId, Payload);
}

void UMCPEventStreamSubsystem::EmitChangeSetCreated(const FString& RequestId, const FString& ChangeSetId, const FString& Path)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("changeset_id"), ChangeSetId);
	Payload->SetStringField(TEXT("path"), Path);
	EmitEvent(TEXT("event.changeset.created"), RequestId, Payload);
}

TArray<TSharedPtr<FJsonObject>> UMCPEventStreamSubsystem::GetRecentEvents(const int32 Limit) const
{
	TArray<TSharedPtr<FJsonObject>> OutEvents;
	const int32 SafeLimit = FMath::Clamp(Limit, 0, MaxBufferedEvents);
	if (SafeLimit <= 0)
	{
		return OutEvents;
	}

	FScopeLock ScopeLock(&EventGuard);
	const int32 Count = FMath::Min(SafeLimit, EventBuffer.Num());
	OutEvents.Reserve(Count);
	for (int32 Index = EventBuffer.Num() - Count; Index < EventBuffer.Num(); ++Index)
	{
		OutEvents.Add(EventBuffer[Index].ToJson());
	}
	return OutEvents;
}

TSharedRef<FJsonObject> UMCPEventStreamSubsystem::BuildSnapshot(const int32 RecentLimit) const
{
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetBoolField(TEXT("supported"), true);

	TArray<TSharedPtr<FJsonObject>> RecentEvents;
	int32 BufferedEventCount = 0;
	int64 TotalEmitted = 0;
	int64 Dropped = 0;
	{
		FScopeLock ScopeLock(&EventGuard);
		BufferedEventCount = EventBuffer.Num();
		TotalEmitted = TotalEmittedEvents;
		Dropped = DroppedEvents;

		const int32 Count = FMath::Min(FMath::Clamp(RecentLimit, 0, MaxBufferedEvents), EventBuffer.Num());
		RecentEvents.Reserve(Count);
		for (int32 Index = EventBuffer.Num() - Count; Index < EventBuffer.Num(); ++Index)
		{
			RecentEvents.Add(EventBuffer[Index].ToJson());
		}
	}

	Snapshot->SetNumberField(TEXT("buffered_event_count"), static_cast<double>(BufferedEventCount));
	Snapshot->SetNumberField(TEXT("total_emitted_event_count"), static_cast<double>(TotalEmitted));
	Snapshot->SetNumberField(TEXT("dropped_event_count"), static_cast<double>(Dropped));

	TArray<TSharedPtr<FJsonValue>> RecentEventValues;
	RecentEventValues.Reserve(RecentEvents.Num());
	for (const TSharedPtr<FJsonObject>& EventObject : RecentEvents)
	{
		if (EventObject.IsValid())
		{
			RecentEventValues.Add(MakeShared<FJsonValueObject>(EventObject.ToSharedRef()));
		}
	}
	Snapshot->SetArrayField(TEXT("recent_events"), RecentEventValues);
	return Snapshot;
}

int32 UMCPEventStreamSubsystem::GetBufferedEventCount() const
{
	FScopeLock ScopeLock(&EventGuard);
	return EventBuffer.Num();
}

int64 UMCPEventStreamSubsystem::GetTotalEmittedEventCount() const
{
	FScopeLock ScopeLock(&EventGuard);
	return TotalEmittedEvents;
}

int64 UMCPEventStreamSubsystem::GetDroppedEventCount() const
{
	FScopeLock ScopeLock(&EventGuard);
	return DroppedEvents;
}

FMCPStreamEventDelegate& UMCPEventStreamSubsystem::OnEventEmitted()
{
	return EventDelegate;
}

void UMCPEventStreamSubsystem::EmitEvent(const FString& EventType, const FString& RequestId, const TSharedRef<FJsonObject>& Payload)
{
	FMCPStreamEvent Event;
	Event.EventId = FString::Printf(TEXT("evt-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Event.EventType = EventType;
	Event.RequestId = RequestId;
	Event.TimestampMs = GetCurrentUnixTimestampMs();
	Event.Payload = Payload;

	{
		FScopeLock ScopeLock(&EventGuard);
		++TotalEmittedEvents;
		if (EventBuffer.Num() >= MaxBufferedEvents)
		{
			EventBuffer.RemoveAt(0);
			++DroppedEvents;
		}
		EventBuffer.Add(Event);
	}

	EventDelegate.Broadcast(Event);
}

int64 UMCPEventStreamSubsystem::GetCurrentUnixTimestampMs()
{
	return FDateTime::UtcNow().ToUnixTimestamp() * 1000LL;
}

