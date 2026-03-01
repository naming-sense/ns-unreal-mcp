#include "Tools/Common/MCPToolSequencerUtils.h"

#include "MCPErrorCodes.h"
#include "Tools/Common/MCPToolAssetUtils.h"
#include "Dom/JsonObject.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	bool ParseFrameRateString(const FString& Value, FFrameRate& OutFrameRate)
	{
		FString Trimmed = Value;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		int32 SlashIndex = INDEX_NONE;
		if (Trimmed.FindChar(TEXT('/'), SlashIndex))
		{
			const FString Left = Trimmed.Left(SlashIndex);
			const FString Right = Trimmed.Mid(SlashIndex + 1);
			const int32 Numerator = FCString::Atoi(*Left);
			const int32 Denominator = FCString::Atoi(*Right);
			if (Numerator > 0 && Denominator > 0)
			{
				OutFrameRate = FFrameRate(Numerator, Denominator);
				return true;
			}
			return false;
		}

		const int32 Numerator = FCString::Atoi(*Trimmed);
		if (Numerator > 0)
		{
			OutFrameRate = FFrameRate(Numerator, 1);
			return true;
		}

		return false;
	}
}

namespace MCPToolSequencerUtils
{
	bool ParseFrameRateField(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		const FFrameRate& DefaultValue,
		FFrameRate& OutFrameRate,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutFrameRate = DefaultValue;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* RateObject = nullptr;
		if (Params->TryGetObjectField(FieldName, RateObject) && RateObject != nullptr && RateObject->IsValid())
		{
			double NumeratorValue = 0.0;
			double DenominatorValue = 1.0;
			(*RateObject)->TryGetNumberField(TEXT("numerator"), NumeratorValue);
			(*RateObject)->TryGetNumberField(TEXT("denominator"), DenominatorValue);
			const int32 Numerator = static_cast<int32>(NumeratorValue);
			const int32 Denominator = static_cast<int32>(DenominatorValue);
			if (Numerator > 0 && Denominator > 0)
			{
				OutFrameRate = FFrameRate(Numerator, Denominator);
				return true;
			}

			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = FString::Printf(TEXT("%s requires numerator/denominator > 0."), *FieldName);
			return false;
		}

		FString RateString;
		if (Params->TryGetStringField(FieldName, RateString))
		{
			if (ParseFrameRateString(RateString, OutFrameRate))
			{
				return true;
			}

			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = FString::Printf(TEXT("%s must be a valid frame rate string (e.g. 30/1)."), *FieldName);
			OutDiagnostic.Detail = RateString;
			return false;
		}

		double RateNumber = 0.0;
		if (Params->TryGetNumberField(FieldName, RateNumber))
		{
			const int32 Numerator = static_cast<int32>(RateNumber);
			if (Numerator > 0)
			{
				OutFrameRate = FFrameRate(Numerator, 1);
				return true;
			}

			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = FString::Printf(TEXT("%s must be > 0 when provided as number."), *FieldName);
			return false;
		}

		return true;
	}

	bool ParseFrameOrSeconds(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FrameField,
		const FString& SecondsField,
		const FFrameRate& TickResolution,
		FFrameNumber& OutFrame,
		const bool bRequired,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutFrame = FFrameNumber(0);
		if (!Params.IsValid())
		{
			if (!bRequired)
			{
				return true;
			}
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = FString::Printf(TEXT("%s or %s is required."), *FrameField, *SecondsField);
			return false;
		}

		double FrameNumberValue = 0.0;
		if (Params->TryGetNumberField(FrameField, FrameNumberValue))
		{
			OutFrame = FFrameNumber(static_cast<int32>(FrameNumberValue));
			return true;
		}

		double SecondsValue = 0.0;
		if (Params->TryGetNumberField(SecondsField, SecondsValue))
		{
			OutFrame = TickResolution.AsFrameNumber(SecondsValue);
			return true;
		}

		if (bRequired)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = FString::Printf(TEXT("%s or %s is required."), *FrameField, *SecondsField);
			return false;
		}

		return true;
	}

	double FrameToSeconds(const FFrameNumber Frame, const FFrameRate& TickResolution)
	{
		return TickResolution.AsSeconds(Frame);
	}

	ULevelSequence* LoadLevelSequenceByPath(const FString& ObjectPath, FMCPDiagnostic& OutDiagnostic)
	{
		if (ObjectPath.IsEmpty())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("object_path is required.");
			return nullptr;
		}

		UObject* Object = MCPToolAssetUtils::ResolveObjectByPath(ObjectPath);
		ULevelSequence* Sequence = Cast<ULevelSequence>(Object);
		if (Sequence == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			OutDiagnostic.Message = TEXT("LevelSequence asset could not be resolved.");
			OutDiagnostic.Detail = ObjectPath;
			OutDiagnostic.Suggestion = TEXT("Verify object_path points to a valid LevelSequence asset.");
		}
		return Sequence;
	}

	UMovieScene* ResolveMovieScene(ULevelSequence* Sequence, FMCPDiagnostic& OutDiagnostic)
	{
		if (Sequence == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			OutDiagnostic.Message = TEXT("LevelSequence is null.");
			return nullptr;
		}

		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (MovieScene == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("LevelSequence has no MovieScene.");
			OutDiagnostic.Detail = Sequence->GetPathName();
		}
		return MovieScene;
	}

	FString BuildBindingId(const FGuid& BindingGuid)
	{
		return BindingGuid.ToString(EGuidFormats::Digits);
	}

	bool ParseBindingId(const FString& BindingId, FGuid& OutBindingGuid)
	{
		if (BindingId.IsEmpty())
		{
			OutBindingGuid.Invalidate();
			return false;
		}
		return FGuid::Parse(BindingId, OutBindingGuid) || FGuid::ParseExact(BindingId, EGuidFormats::DigitsWithHyphens, OutBindingGuid);
	}

	const FMovieSceneBinding* FindBindingById(const UMovieScene* MovieScene, const FString& BindingId, FGuid* OutBindingGuid)
	{
		if (MovieScene == nullptr)
		{
			return nullptr;
		}

		FGuid BindingGuid;
		if (!ParseBindingId(BindingId, BindingGuid))
		{
			return nullptr;
		}

		if (OutBindingGuid != nullptr)
		{
			*OutBindingGuid = BindingGuid;
		}

		return MovieScene->FindBinding(BindingGuid);
	}

	FMovieSceneBinding* FindBindingById(UMovieScene* MovieScene, const FString& BindingId, FGuid* OutBindingGuid)
	{
		if (MovieScene == nullptr)
		{
			return nullptr;
		}

		FGuid BindingGuid;
		if (!ParseBindingId(BindingId, BindingGuid))
		{
			return nullptr;
		}

		if (OutBindingGuid != nullptr)
		{
			*OutBindingGuid = BindingGuid;
		}

		return MovieScene->FindBinding(BindingGuid);
	}

	FString BuildTrackId(const UMovieSceneTrack* Track)
	{
		return Track ? Track->GetPathName() : FString();
	}

	UMovieSceneTrack* ResolveTrackById(const FString& TrackId)
	{
		if (TrackId.IsEmpty())
		{
			return nullptr;
		}

		UMovieSceneTrack* Track = FindObject<UMovieSceneTrack>(nullptr, *TrackId);
		if (Track == nullptr)
		{
			Track = LoadObject<UMovieSceneTrack>(nullptr, *TrackId);
		}
		return Track;
	}

	FString BuildSectionId(const UMovieSceneSection* Section)
	{
		return Section ? Section->GetPathName() : FString();
	}

	UMovieSceneSection* ResolveSectionById(const FString& SectionId)
	{
		if (SectionId.IsEmpty())
		{
			return nullptr;
		}

		UMovieSceneSection* Section = FindObject<UMovieSceneSection>(nullptr, *SectionId);
		if (Section == nullptr)
		{
			Section = LoadObject<UMovieSceneSection>(nullptr, *SectionId);
		}
		return Section;
	}

	FString BuildChannelId(const FString& SectionId, const FString& ChannelType, const int32 ChannelIndex)
	{
		return FString::Printf(TEXT("%s|%s|%d"), *SectionId, *ChannelType, ChannelIndex);
	}

	bool ParseChannelId(
		const FString& ChannelId,
		FString& OutSectionId,
		FString& OutChannelType,
		int32& OutChannelIndex,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutSectionId.Reset();
		OutChannelType.Reset();
		OutChannelIndex = INDEX_NONE;

		if (ChannelId.IsEmpty())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("channel_id is required.");
			return false;
		}

		TArray<FString> Tokens;
		ChannelId.ParseIntoArray(Tokens, TEXT("|"), false);
		if (Tokens.Num() < 3)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("channel_id format is invalid.");
			OutDiagnostic.Detail = ChannelId;
			OutDiagnostic.Suggestion = TEXT("Use channel_id returned by seq.channel.list.");
			return false;
		}

		OutSectionId = Tokens[0];
		for (int32 Index = 1; Index < Tokens.Num() - 2; ++Index)
		{
			OutSectionId += TEXT("|");
			OutSectionId += Tokens[Index];
		}

		OutChannelType = Tokens[Tokens.Num() - 2];
		OutChannelIndex = FCString::Atoi(*Tokens[Tokens.Num() - 1]);
		return true;
	}

	void AppendTouchedSequencePackage(const ULevelSequence* Sequence, TArray<FString>& OutTouchedPackages)
	{
		if (Sequence == nullptr)
		{
			return;
		}

		if (const UPackage* Package = Sequence->GetOutermost())
		{
			OutTouchedPackages.AddUnique(Package->GetName());
		}
	}
}
