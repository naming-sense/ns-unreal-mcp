#include "Tools/Sequencer/MCPToolsSequencerKeyHandler.h"

#include "MCPErrorCodes.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPSequencerApiCompat.h"
#include "Tools/Common/MCPToolSequencerUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "KeyParams.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"

#if __has_include("Sections/MovieSceneDoubleSection.h")
#include "Sections/MovieSceneDoubleSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#define MCP_HAS_SEQ_DOUBLE_CHANNEL 1
#else
#define MCP_HAS_SEQ_DOUBLE_CHANNEL 0
#endif

#if __has_include("Sections/MovieSceneBoolSection.h")
#include "Sections/MovieSceneBoolSection.h"
#include "Channels/MovieSceneBoolChannel.h"
#define MCP_HAS_SEQ_BOOL_CHANNEL 1
#else
#define MCP_HAS_SEQ_BOOL_CHANNEL 0
#endif

namespace
{
	using MCPToolCommonJson::ToJsonStringArray;

	enum class ESeqChannelKind : uint8
	{
		None,
		Float,
		Double,
		Bool
	};

	struct FResolvedChannel
	{
		ESeqChannelKind Kind = ESeqChannelKind::None;
		UMovieSceneSection* Section = nullptr;
		UMovieScene* MovieScene = nullptr;
		FMovieSceneFloatChannel* FloatChannel = nullptr;
#if MCP_HAS_SEQ_DOUBLE_CHANNEL
		FMovieSceneDoubleChannel* DoubleChannel = nullptr;
#endif
#if MCP_HAS_SEQ_BOOL_CHANNEL
		FMovieSceneBoolChannel* BoolChannel = nullptr;
#endif
		FString ChannelId;
	};

	bool ParseInterpolation(const TSharedPtr<FJsonObject>& Params, EMovieSceneKeyInterpolation& OutInterpolation)
	{
		OutInterpolation = EMovieSceneKeyInterpolation::Auto;
		if (!Params.IsValid())
		{
			return true;
		}

		FString InterpString;
		if (!Params->TryGetStringField(TEXT("interp"), InterpString))
		{
			return true;
		}

		InterpString = InterpString.ToLower();
		if (InterpString == TEXT("auto") || InterpString == TEXT("smart_auto"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Auto;
			return true;
		}
		if (InterpString == TEXT("linear"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Linear;
			return true;
		}
		if (InterpString == TEXT("constant"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Constant;
			return true;
		}
		if (InterpString == TEXT("user"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::User;
			return true;
		}
		if (InterpString == TEXT("break"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Break;
			return true;
		}
		return false;
	}

	void AddFloatKey(FMovieSceneFloatChannel* Channel, const FFrameNumber FrameNumber, const float Value, const EMovieSceneKeyInterpolation Interpolation)
	{
		if (Channel == nullptr)
		{
			return;
		}

		switch (Interpolation)
		{
		case EMovieSceneKeyInterpolation::Linear:
			Channel->AddLinearKey(FrameNumber, Value);
			break;
		case EMovieSceneKeyInterpolation::Constant:
			Channel->AddConstantKey(FrameNumber, Value);
			break;
		default:
			Channel->AddCubicKey(FrameNumber, Value);
			break;
		}
	}

#if MCP_HAS_SEQ_DOUBLE_CHANNEL
	void AddDoubleKey(FMovieSceneDoubleChannel* Channel, const FFrameNumber FrameNumber, const double Value, const EMovieSceneKeyInterpolation Interpolation)
	{
		if (Channel == nullptr)
		{
			return;
		}

		switch (Interpolation)
		{
		case EMovieSceneKeyInterpolation::Linear:
			Channel->AddLinearKey(FrameNumber, Value);
			break;
		case EMovieSceneKeyInterpolation::Constant:
			Channel->AddConstantKey(FrameNumber, Value);
			break;
		default:
			Channel->AddCubicKey(FrameNumber, Value);
			break;
		}
	}
#endif

	void AppendTouchedFromSection(const UMovieSceneSection* Section, TArray<FString>& OutTouchedPackages)
	{
		if (Section == nullptr)
		{
			return;
		}
		const UMovieScene* MovieScene = Section->GetTypedOuter<UMovieScene>();
		const ULevelSequence* Sequence = MovieScene ? MovieScene->GetTypedOuter<ULevelSequence>() : nullptr;
		MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutTouchedPackages);
	}

	bool ResolveChannelFromRequest(const FMCPRequestEnvelope& Request, FResolvedChannel& OutChannel, FMCPDiagnostic& OutDiagnostic)
	{
		OutChannel = FResolvedChannel();

		FString ChannelId;
		FString SectionId;
		FString ChannelType;
		int32 ChannelIndex = 0;

		if (Request.Params.IsValid())
		{
			Request.Params->TryGetStringField(TEXT("channel_id"), ChannelId);
		}

		if (ChannelId.IsEmpty() && Request.Params.IsValid())
		{
			const TSharedPtr<FJsonObject>* ChannelRefObject = nullptr;
			if (Request.Params->TryGetObjectField(TEXT("channel_ref"), ChannelRefObject) && ChannelRefObject != nullptr && ChannelRefObject->IsValid())
			{
				(*ChannelRefObject)->TryGetStringField(TEXT("section_id"), SectionId);
				(*ChannelRefObject)->TryGetStringField(TEXT("channel_type"), ChannelType);
				double IndexValue = 0.0;
				(*ChannelRefObject)->TryGetNumberField(TEXT("channel_index"), IndexValue);
				ChannelIndex = static_cast<int32>(IndexValue);
				if (!SectionId.IsEmpty() && !ChannelType.IsEmpty())
				{
					ChannelId = MCPToolSequencerUtils::BuildChannelId(SectionId, ChannelType, ChannelIndex);
				}
			}
		}

		if (!MCPToolSequencerUtils::ParseChannelId(ChannelId, SectionId, ChannelType, ChannelIndex, OutDiagnostic))
		{
			return false;
		}

		UMovieSceneSection* Section = MCPToolSequencerUtils::ResolveSectionById(SectionId);
		if (Section == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			OutDiagnostic.Message = TEXT("section_id from channel_id could not be resolved.");
			OutDiagnostic.Detail = SectionId;
			return false;
		}

		UMovieScene* MovieScene = Section->GetTypedOuter<UMovieScene>();
		if (MovieScene == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("Channel section does not belong to MovieScene.");
			OutDiagnostic.Detail = SectionId;
			return false;
		}

		if (ChannelIndex != 0)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Current sequencer key tools only support channel_index=0 per section type.");
			OutDiagnostic.Detail = ChannelId;
			return false;
		}

		const FString NormalizedType = ChannelType.ToLower();
		if (NormalizedType == TEXT("float"))
		{
			if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
			{
				OutChannel.Kind = ESeqChannelKind::Float;
				OutChannel.Section = Section;
				OutChannel.MovieScene = MovieScene;
				OutChannel.FloatChannel = &FloatSection->GetChannel();
				OutChannel.ChannelId = ChannelId;
				return true;
			}
		}

#if MCP_HAS_SEQ_DOUBLE_CHANNEL
		if (NormalizedType == TEXT("double"))
		{
			if (UMovieSceneDoubleSection* DoubleSection = Cast<UMovieSceneDoubleSection>(Section))
			{
				OutChannel.Kind = ESeqChannelKind::Double;
				OutChannel.Section = Section;
				OutChannel.MovieScene = MovieScene;
				OutChannel.DoubleChannel = &DoubleSection->GetChannel();
				OutChannel.ChannelId = ChannelId;
				return true;
			}
		}
#endif

#if MCP_HAS_SEQ_BOOL_CHANNEL
		if (NormalizedType == TEXT("bool"))
		{
			if (UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section))
			{
				OutChannel.Kind = ESeqChannelKind::Bool;
				OutChannel.Section = Section;
				OutChannel.MovieScene = MovieScene;
				OutChannel.BoolChannel = &BoolSection->GetChannel();
				OutChannel.ChannelId = ChannelId;
				return true;
			}
		}
#endif

		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("channel_id type does not match section type or is unsupported.");
		OutDiagnostic.Detail = ChannelId;
		OutDiagnostic.Suggestion = TEXT("Use channel_id returned by seq.channel.list.");
		return false;
	}

	int32 RemoveFloatKeysAtFrame(FMovieSceneFloatChannel* Channel, const FFrameNumber FrameNumber)
	{
		if (Channel == nullptr)
		{
			return 0;
		}
		TArray<FFrameNumber> KeyTimes;
		TArray<FKeyHandle> KeyHandles;
		Channel->GetKeys(TRange<FFrameNumber>(FrameNumber, FrameNumber + 1), &KeyTimes, &KeyHandles);
		TArray<FKeyHandle> HandlesToRemove;
		for (int32 Index = 0; Index < KeyTimes.Num() && Index < KeyHandles.Num(); ++Index)
		{
			if (KeyTimes[Index] == FrameNumber)
			{
				HandlesToRemove.Add(KeyHandles[Index]);
			}
		}
		if (HandlesToRemove.Num() > 0)
		{
			Channel->DeleteKeys(HandlesToRemove);
		}
		return HandlesToRemove.Num();
	}

#if MCP_HAS_SEQ_DOUBLE_CHANNEL
	int32 RemoveDoubleKeysAtFrame(FMovieSceneDoubleChannel* Channel, const FFrameNumber FrameNumber)
	{
		if (Channel == nullptr)
		{
			return 0;
		}
		TArray<FFrameNumber> KeyTimes;
		TArray<FKeyHandle> KeyHandles;
		Channel->GetKeys(TRange<FFrameNumber>(FrameNumber, FrameNumber + 1), &KeyTimes, &KeyHandles);
		TArray<FKeyHandle> HandlesToRemove;
		for (int32 Index = 0; Index < KeyTimes.Num() && Index < KeyHandles.Num(); ++Index)
		{
			if (KeyTimes[Index] == FrameNumber)
			{
				HandlesToRemove.Add(KeyHandles[Index]);
			}
		}
		if (HandlesToRemove.Num() > 0)
		{
			Channel->DeleteKeys(HandlesToRemove);
		}
		return HandlesToRemove.Num();
	}
#endif

#if MCP_HAS_SEQ_BOOL_CHANNEL
	int32 RemoveBoolKeysAtFrame(FMovieSceneBoolChannel* Channel, const FFrameNumber FrameNumber)
	{
		if (Channel == nullptr)
		{
			return 0;
		}
		TArray<FFrameNumber> KeyTimes;
		TArray<FKeyHandle> KeyHandles;
		Channel->GetKeys(TRange<FFrameNumber>(FrameNumber, FrameNumber + 1), &KeyTimes, &KeyHandles);
		TArray<FKeyHandle> HandlesToRemove;
		for (int32 Index = 0; Index < KeyTimes.Num() && Index < KeyHandles.Num(); ++Index)
		{
			if (KeyTimes[Index] == FrameNumber)
			{
				HandlesToRemove.Add(KeyHandles[Index]);
			}
		}
		if (HandlesToRemove.Num() > 0)
		{
			Channel->DeleteKeys(HandlesToRemove);
		}
		return HandlesToRemove.Num();
	}
#endif
}

bool FMCPToolsSequencerKeyHandler::HandleKeySet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FResolvedChannel Channel;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveChannelFromRequest(Request, Channel, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FFrameNumber FrameNumber;
	FMCPDiagnostic TimeDiagnostic;
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("frame"), TEXT("time_seconds"), Channel.MovieScene->GetTickResolution(), FrameNumber, true, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	EMovieSceneKeyInterpolation Interpolation = EMovieSceneKeyInterpolation::Auto;
	if (!ParseInterpolation(Request.Params, Interpolation))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("interp must be one of auto, linear, constant, user, break.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bAdded = true;
	if (!Request.Context.bDryRun)
	{
		Channel.Section->Modify();
		double NumericValue = 0.0;
		bool BoolValue = false;

		switch (Channel.Kind)
		{
		case ESeqChannelKind::Float:
			if (!Request.Params.IsValid() || !Request.Params->TryGetNumberField(TEXT("value"), NumericValue))
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("value(number) is required for float channel.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			AddFloatKey(Channel.FloatChannel, FrameNumber, static_cast<float>(NumericValue), Interpolation);
			break;
#if MCP_HAS_SEQ_DOUBLE_CHANNEL
		case ESeqChannelKind::Double:
			if (!Request.Params.IsValid() || !Request.Params->TryGetNumberField(TEXT("value"), NumericValue))
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("value(number) is required for double channel.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			AddDoubleKey(Channel.DoubleChannel, FrameNumber, NumericValue, Interpolation);
			break;
#endif
#if MCP_HAS_SEQ_BOOL_CHANNEL
		case ESeqChannelKind::Bool:
			if (!Request.Params.IsValid() || !Request.Params->TryGetBoolField(TEXT("value"), BoolValue))
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("value(bool) is required for bool channel.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			MCPSequencerApiCompat::AddBoolKeySafe(Channel.BoolChannel, FrameNumber, BoolValue);
			break;
#endif
		default:
			bAdded = false;
			break;
		}

		Channel.Section->MarkPackageDirty();
	}

	AppendTouchedFromSection(Channel.Section, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("key_set"), bAdded);
	OutResult.ResultObject->SetStringField(TEXT("channel_id"), Channel.ChannelId);
	OutResult.ResultObject->SetNumberField(TEXT("frame"), FrameNumber.Value);
	OutResult.ResultObject->SetNumberField(TEXT("time_seconds"), MCPToolSequencerUtils::FrameToSeconds(FrameNumber, Channel.MovieScene->GetTickResolution()));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAdded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Error;
	return bAdded;
}

bool FMCPToolsSequencerKeyHandler::HandleKeyRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FResolvedChannel Channel;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveChannelFromRequest(Request, Channel, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FFrameNumber FrameNumber;
	FMCPDiagnostic TimeDiagnostic;
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("frame"), TEXT("time_seconds"), Channel.MovieScene->GetTickResolution(), FrameNumber, true, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	int32 RemovedCount = 0;
	if (!Request.Context.bDryRun)
	{
		Channel.Section->Modify();
		switch (Channel.Kind)
		{
		case ESeqChannelKind::Float:
			RemovedCount = RemoveFloatKeysAtFrame(Channel.FloatChannel, FrameNumber);
			break;
#if MCP_HAS_SEQ_DOUBLE_CHANNEL
		case ESeqChannelKind::Double:
			RemovedCount = RemoveDoubleKeysAtFrame(Channel.DoubleChannel, FrameNumber);
			break;
#endif
#if MCP_HAS_SEQ_BOOL_CHANNEL
		case ESeqChannelKind::Bool:
			RemovedCount = RemoveBoolKeysAtFrame(Channel.BoolChannel, FrameNumber);
			break;
#endif
		default:
			RemovedCount = 0;
			break;
		}
		Channel.Section->MarkPackageDirty();
	}

	AppendTouchedFromSection(Channel.Section, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemovedCount);
	OutResult.ResultObject->SetStringField(TEXT("channel_id"), Channel.ChannelId);
	OutResult.ResultObject->SetNumberField(TEXT("frame"), FrameNumber.Value);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerKeyHandler::HandleKeyBulkSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FResolvedChannel Channel;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveChannelFromRequest(Request, Channel, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetArrayField(TEXT("keys"), Keys);
	}

	if (Keys == nullptr || Keys->Num() == 0)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("keys array is required for seq.key.bulk_set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	int32 AddedCount = 0;
	if (!Request.Context.bDryRun)
	{
		Channel.Section->Modify();
	}

	for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
	{
		if (!KeyValue.IsValid() || KeyValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> KeyObject = KeyValue->AsObject();
		FFrameNumber FrameNumber;
		FMCPDiagnostic TimeDiagnostic;
		if (!MCPToolSequencerUtils::ParseFrameOrSeconds(KeyObject, TEXT("frame"), TEXT("time_seconds"), Channel.MovieScene->GetTickResolution(), FrameNumber, true, TimeDiagnostic))
		{
			OutResult.Diagnostics.Add(TimeDiagnostic);
			continue;
		}

		EMovieSceneKeyInterpolation Interpolation = EMovieSceneKeyInterpolation::Auto;
		if (!ParseInterpolation(KeyObject, Interpolation))
		{
			Interpolation = EMovieSceneKeyInterpolation::Auto;
		}

		if (Request.Context.bDryRun)
		{
			++AddedCount;
			continue;
		}

		double NumericValue = 0.0;
		bool BoolValue = false;
		bool bAddedThisKey = false;
		switch (Channel.Kind)
		{
		case ESeqChannelKind::Float:
			if (KeyObject->TryGetNumberField(TEXT("value"), NumericValue))
			{
				AddFloatKey(Channel.FloatChannel, FrameNumber, static_cast<float>(NumericValue), Interpolation);
				bAddedThisKey = true;
			}
			break;
#if MCP_HAS_SEQ_DOUBLE_CHANNEL
		case ESeqChannelKind::Double:
			if (KeyObject->TryGetNumberField(TEXT("value"), NumericValue))
			{
				AddDoubleKey(Channel.DoubleChannel, FrameNumber, NumericValue, Interpolation);
				bAddedThisKey = true;
			}
			break;
#endif
#if MCP_HAS_SEQ_BOOL_CHANNEL
		case ESeqChannelKind::Bool:
			if (KeyObject->TryGetBoolField(TEXT("value"), BoolValue))
			{
				MCPSequencerApiCompat::AddBoolKeySafe(Channel.BoolChannel, FrameNumber, BoolValue);
				bAddedThisKey = true;
			}
			break;
#endif
		default:
			break;
		}

		if (bAddedThisKey)
		{
			++AddedCount;
		}
	}

	if (!Request.Context.bDryRun)
	{
		Channel.Section->MarkPackageDirty();
	}

	AppendTouchedFromSection(Channel.Section, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("channel_id"), Channel.ChannelId);
	OutResult.ResultObject->SetNumberField(TEXT("added_count"), AddedCount);
	OutResult.ResultObject->SetNumberField(TEXT("requested_count"), Keys->Num());
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = AddedCount == Keys->Num() ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}
