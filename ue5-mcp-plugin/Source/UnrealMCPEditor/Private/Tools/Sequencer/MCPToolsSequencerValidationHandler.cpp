#include "Tools/Sequencer/MCPToolsSequencerValidationHandler.h"

#include "MCPErrorCodes.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPSequencerApiCompat.h"
#include "Tools/Common/MCPToolSequencerUtils.h"
#include "Dom/JsonObject.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"

namespace
{
	using MCPToolCommonJson::ToJsonStringArray;

	bool ResolveSequenceFromRequest(
		const FMCPRequestEnvelope& Request,
		ULevelSequence*& OutSequence,
		UMovieScene*& OutMovieScene,
		FMCPToolExecutionResult& OutResult)
	{
		FString ObjectPath;
		if (Request.Params.IsValid())
		{
			Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		}

		FMCPDiagnostic Diagnostic;
		OutSequence = MCPToolSequencerUtils::LoadLevelSequenceByPath(ObjectPath, Diagnostic);
		if (OutSequence == nullptr)
		{
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		OutMovieScene = MCPToolSequencerUtils::ResolveMovieScene(OutSequence, Diagnostic);
		if (OutMovieScene == nullptr)
		{
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		return true;
	}

	void AddDiagnostic(
		FMCPToolExecutionResult& OutResult,
		const FString& Code,
		const FString& Severity,
		const FString& Message,
		const FString& Detail,
		const bool bRetriable = false)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = Code;
		Diagnostic.Severity = Severity;
		Diagnostic.Message = Message;
		Diagnostic.Detail = Detail;
		Diagnostic.bRetriable = bRetriable;
		OutResult.Diagnostics.Add(Diagnostic);
	}
}

bool FMCPToolsSequencerValidationHandler::HandleValidate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	int32 BindingCount = 0;
	int32 TrackCount = 0;
	int32 SectionCount = 0;

	const TArray<FMovieSceneBinding>& Bindings = MCPSequencerApiCompat::GetBindingsConst(MovieScene);
	for (const FMovieSceneBinding& Binding : Bindings)
	{
		++BindingCount;
		const FGuid BindingGuid = Binding.GetObjectGuid();
		const bool bHasSpawnable = MCPSequencerApiCompat::IsSpawnableBinding(MovieScene, BindingGuid);
		const bool bHasPossessable = MCPSequencerApiCompat::IsPossessableBinding(MovieScene, BindingGuid);
		if (!bHasSpawnable && !bHasPossessable)
		{
			AddDiagnostic(
				OutResult,
				MCPErrorCodes::OBJECT_NOT_FOUND,
				TEXT("warning"),
				TEXT("Binding does not resolve to spawnable/possessable."),
				MCPToolSequencerUtils::BuildBindingId(BindingGuid),
				true);
			++WarningCount;
		}

		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (Track == nullptr)
			{
				continue;
			}
			++TrackCount;
			const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
			if (Sections.Num() == 0)
			{
				AddDiagnostic(
					OutResult,
					MCPErrorCodes::SCHEMA_INVALID_PARAMS,
					TEXT("warning"),
					TEXT("Track has no sections."),
					MCPToolSequencerUtils::BuildTrackId(Track),
					false);
				++WarningCount;
			}

			for (UMovieSceneSection* Section : Sections)
			{
				if (Section == nullptr)
				{
					continue;
				}
				++SectionCount;
				const TRange<FFrameNumber> Range = Section->GetRange();
				if (Range.GetLowerBound().IsClosed() && Range.GetUpperBound().IsClosed() && Range.GetUpperBoundValue() <= Range.GetLowerBoundValue())
				{
					AddDiagnostic(
						OutResult,
						MCPErrorCodes::SCHEMA_INVALID_PARAMS,
						TEXT("error"),
						TEXT("Section has invalid frame range (end <= start)."),
						MCPToolSequencerUtils::BuildSectionId(Section),
						false);
					++ErrorCount;
				}
			}
		}
	}

	for (UMovieSceneTrack* Track : MCPSequencerApiCompat::GetGlobalTracks(MovieScene))
	{
		if (Track == nullptr)
		{
			continue;
		}
		++TrackCount;
		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		if (Sections.Num() == 0)
		{
			AddDiagnostic(
				OutResult,
				MCPErrorCodes::SCHEMA_INVALID_PARAMS,
				TEXT("warning"),
				TEXT("Track has no sections."),
				MCPToolSequencerUtils::BuildTrackId(Track),
				false);
			++WarningCount;
		}

		for (UMovieSceneSection* Section : Sections)
		{
			if (Section == nullptr)
			{
				continue;
			}
			++SectionCount;
			const TRange<FFrameNumber> Range = Section->GetRange();
			if (Range.GetLowerBound().IsClosed() && Range.GetUpperBound().IsClosed() && Range.GetUpperBoundValue() <= Range.GetLowerBoundValue())
			{
				AddDiagnostic(
					OutResult,
					MCPErrorCodes::SCHEMA_INVALID_PARAMS,
					TEXT("error"),
					TEXT("Section has invalid frame range (end <= start)."),
					MCPToolSequencerUtils::BuildSectionId(Section),
					false);
				++ErrorCount;
			}
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutResult.ResultObject->SetNumberField(TEXT("binding_count"), BindingCount);
	OutResult.ResultObject->SetNumberField(TEXT("track_count"), TrackCount);
	OutResult.ResultObject->SetNumberField(TEXT("section_count"), SectionCount);
	OutResult.ResultObject->SetNumberField(TEXT("error_count"), ErrorCount);
	OutResult.ResultObject->SetNumberField(TEXT("warning_count"), WarningCount);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));

	if (ErrorCount > 0)
	{
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.Status = WarningCount > 0 ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	return true;
}
