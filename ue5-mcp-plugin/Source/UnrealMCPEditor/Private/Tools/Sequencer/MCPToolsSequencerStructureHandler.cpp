#include "Tools/Sequencer/MCPToolsSequencerStructureHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolAssetUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPSequencerApiCompat.h"
#include "Tools/Common/MCPToolSequencerUtils.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "ObjectTools.h"

namespace
{
	using MCPToolCommonJson::ToJsonStringArray;

	template <typename TMovieScene>
	auto SetPlaybackRangeCompat(TMovieScene* MovieScene, const FFrameNumber StartFrame, const FFrameNumber EndFrame, int)
		-> decltype(MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame)), void())
	{
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));
	}

	template <typename TMovieScene>
	auto SetPlaybackRangeCompat(TMovieScene* MovieScene, const FFrameNumber StartFrame, const FFrameNumber EndFrame, long)
		-> decltype(MovieScene->SetPlaybackRange(StartFrame.Value, EndFrame.Value - StartFrame.Value), void())
	{
		MovieScene->SetPlaybackRange(StartFrame.Value, EndFrame.Value - StartFrame.Value);
	}

	template <typename TMovieScene>
	auto SetViewRangeCompat(TMovieScene* MovieScene, const double StartSeconds, const double EndSeconds, int)
		-> decltype(MovieScene->SetViewRange(StartSeconds, EndSeconds), void())
	{
		MovieScene->SetViewRange(StartSeconds, EndSeconds);
	}

	template <typename TMovieScene>
	void SetViewRangeCompat(TMovieScene*, const double, const double, long)
	{
	}

	template <typename TMovieScene>
	auto SetWorkRangeCompat(TMovieScene* MovieScene, const double StartSeconds, const double EndSeconds, int)
		-> decltype(MovieScene->SetWorkingRange(StartSeconds, EndSeconds), void())
	{
		MovieScene->SetWorkingRange(StartSeconds, EndSeconds);
	}

	template <typename TMovieScene>
	void SetWorkRangeCompat(TMovieScene*, const double, const double, long)
	{
	}

	AActor* FindActorByReference(UWorld* World, const FString& ActorReference)
	{
		if (World == nullptr || ActorReference.IsEmpty())
		{
			return nullptr;
		}

		FString RequestedLabel = ActorReference;
		int32 DotIndex = INDEX_NONE;
		if (ActorReference.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < ActorReference.Len())
		{
			RequestedLabel = ActorReference.Mid(DotIndex + 1);
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}

			if (MCPObjectUtils::BuildActorPath(Actor).Equals(ActorReference, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(ActorReference, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(ActorReference, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(RequestedLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	UWorld* GetEditorWorld()
	{
		if (GEditor == nullptr)
		{
			return nullptr;
		}
		return GEditor->GetEditorWorldContext().World();
	}

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

	UClass* ResolveTrackClassFromParams(const TSharedPtr<FJsonObject>& Params, FMCPDiagnostic& OutDiagnostic)
	{
		FString TrackClassPath;
		FString TrackType;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("track_class_path"), TrackClassPath);
			Params->TryGetStringField(TEXT("track_type"), TrackType);
		}

		if (TrackClassPath.IsEmpty() && !TrackType.IsEmpty())
		{
			const FString NormalizedType = TrackType.ToLower();
			if (NormalizedType == TEXT("camera_cut"))
			{
				TrackClassPath = TEXT("/Script/MovieSceneTracks.MovieSceneCameraCutTrack");
			}
			else if (NormalizedType == TEXT("float"))
			{
				TrackClassPath = TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack");
			}
			else if (NormalizedType == TEXT("bool"))
			{
				TrackClassPath = TEXT("/Script/MovieSceneTracks.MovieSceneBoolTrack");
			}
			else if (NormalizedType == TEXT("audio"))
			{
				TrackClassPath = TEXT("/Script/MovieSceneTracks.MovieSceneAudioTrack");
			}
			else if (NormalizedType == TEXT("event"))
			{
				TrackClassPath = TEXT("/Script/MovieSceneTracks.MovieSceneEventTrack");
			}
			else if (NormalizedType == TEXT("transform") || NormalizedType == TEXT("3d_transform"))
			{
				TrackClassPath = TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack");
			}
		}

		if (TrackClassPath.IsEmpty())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("track_class_path or track_type is required.");
			return nullptr;
		}

		UClass* TrackClass = MCPToolAssetUtils::ResolveClassByPath(TrackClassPath);
		if (TrackClass == nullptr || !TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("track_class_path could not be resolved to UMovieSceneTrack.");
			OutDiagnostic.Detail = TrackClassPath;
			return nullptr;
		}

		return TrackClass;
	}

	void AppendTouchedPackages(UMovieScene* MovieScene, TArray<FString>& OutTouchedPackages)
	{
		if (MovieScene == nullptr)
		{
			return;
		}

		const ULevelSequence* Sequence = MovieScene->GetTypedOuter<ULevelSequence>();
		MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutTouchedPackages);
	}

}

bool FMCPToolsSequencerStructureHandler::HandleBindingAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	FString Mode = TEXT("possessable");
	FString ObjectPath;
	FString ActorPath;
	FString ClassPath;
	FString DisplayName;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("actor_path"), ActorPath);
		Request.Params->TryGetStringField(TEXT("class_path"), ClassPath);
		Request.Params->TryGetStringField(TEXT("display_name"), DisplayName);
	}
	Mode = Mode.ToLower();

	UObject* TargetObject = nullptr;
	if (!ObjectPath.IsEmpty())
	{
		TargetObject = MCPToolAssetUtils::ResolveObjectByPath(ObjectPath);
	}

	AActor* TargetActor = nullptr;
	if (!ActorPath.IsEmpty())
	{
		TargetActor = FindActorByReference(GetEditorWorld(), ActorPath);
		if (TargetActor != nullptr && TargetObject == nullptr)
		{
			TargetObject = TargetActor;
		}
	}

	UClass* TargetClass = nullptr;
	if (TargetObject != nullptr)
	{
		TargetClass = TargetObject->GetClass();
	}
	if (TargetClass == nullptr && !ClassPath.IsEmpty())
	{
		TargetClass = MCPToolAssetUtils::ResolveClassByPath(ClassPath);
	}

	if (TargetClass == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("binding target class could not be resolved. Provide object_path, actor_path or class_path.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (DisplayName.IsEmpty())
	{
		DisplayName = TargetObject != nullptr ? TargetObject->GetName() : TargetClass->GetName();
	}

	FGuid BindingGuid;
	bool bSpawnable = false;
	if (!Request.Context.bDryRun)
	{
		MovieScene->Modify();
		if (Mode == TEXT("spawnable"))
		{
			if (TargetObject == nullptr)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("spawnable mode requires object_path or actor_path template object.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			BindingGuid = MovieScene->AddSpawnable(DisplayName, *TargetObject);
			bSpawnable = true;
		}
		else
		{
			BindingGuid = MovieScene->AddPossessable(DisplayName, TargetClass);
		}

		if (!BindingGuid.IsValid())
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to add sequence binding.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MovieScene->MarkPackageDirty();
		Sequence->MarkPackageDirty();
	}
	else
	{
		BindingGuid = FGuid::NewGuid();
		bSpawnable = Mode == TEXT("spawnable");
	}

	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("added"), true);
	OutResult.ResultObject->SetStringField(TEXT("binding_id"), MCPToolSequencerUtils::BuildBindingId(BindingGuid));
	OutResult.ResultObject->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	OutResult.ResultObject->SetStringField(TEXT("display_name"), DisplayName);
	OutResult.ResultObject->SetBoolField(TEXT("spawnable"), bSpawnable);
	OutResult.ResultObject->SetBoolField(TEXT("possessable"), !bSpawnable);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleBindingRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	FString BindingId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("binding_id"), BindingId);
	}

	FGuid BindingGuid;
	FMovieSceneBinding* Binding = MCPToolSequencerUtils::FindBindingById(MovieScene, BindingId, &BindingGuid);
	if (Binding == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("binding_id was not found.");
		Diagnostic.Detail = BindingId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bRemoved = true;
	if (!Request.Context.bDryRun)
	{
		MovieScene->Modify();
		bRemoved = MCPSequencerApiCompat::RemoveBindingSafe(MovieScene, BindingGuid);
		if (bRemoved)
		{
			MovieScene->MarkPackageDirty();
			Sequence->MarkPackageDirty();
		}
	}

	if (!bRemoved)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Failed to remove binding.");
		Diagnostic.Detail = BindingId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("removed"), true);
	OutResult.ResultObject->SetStringField(TEXT("binding_id"), BindingId);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleTrackAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	FMCPDiagnostic TrackClassDiagnostic;
	UClass* TrackClass = ResolveTrackClassFromParams(Request.Params, TrackClassDiagnostic);
	if (TrackClass == nullptr)
	{
		OutResult.Diagnostics.Add(TrackClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString BindingId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("binding_id"), BindingId);
	}

	UMovieSceneTrack* AddedTrack = nullptr;
	if (!Request.Context.bDryRun)
	{
		MovieScene->Modify();
		if (!BindingId.IsEmpty())
		{
			FGuid BindingGuid;
			const FMovieSceneBinding* Binding = MCPToolSequencerUtils::FindBindingById(MovieScene, BindingId, &BindingGuid);
			if (Binding == nullptr)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
				Diagnostic.Message = TEXT("binding_id was not found.");
				Diagnostic.Detail = BindingId;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			AddedTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
		}
		else
		{
			AddedTrack = MCPSequencerApiCompat::AddGlobalTrack(MovieScene, TrackClass);
		}

		if (AddedTrack == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to add track.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MovieScene->MarkPackageDirty();
		Sequence->MarkPackageDirty();
	}

	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("added"), true);
	OutResult.ResultObject->SetStringField(TEXT("track_id"), AddedTrack ? MCPToolSequencerUtils::BuildTrackId(AddedTrack) : FString());
	OutResult.ResultObject->SetStringField(TEXT("class_path"), TrackClass->GetPathName());
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleTrackRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString TrackId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("track_id"), TrackId);
	}

	UMovieSceneTrack* Track = MCPToolSequencerUtils::ResolveTrackById(TrackId);
	if (Track == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("track_id was not found.");
		Diagnostic.Detail = TrackId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
	if (MovieScene == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("track_id does not belong to a valid MovieScene.");
		Diagnostic.Detail = TrackId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bRemoved = true;
	if (!Request.Context.bDryRun)
	{
		MovieScene->Modify();
		bRemoved = MCPSequencerApiCompat::RemoveTrackSafe(MovieScene, Track);
		if (bRemoved)
		{
			MovieScene->MarkPackageDirty();
		}
	}

	if (!bRemoved)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Failed to remove track.");
		Diagnostic.Detail = TrackId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	AppendTouchedPackages(MovieScene, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("removed"), true);
	OutResult.ResultObject->SetStringField(TEXT("track_id"), TrackId);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleSectionAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString TrackId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("track_id"), TrackId);
	}

	UMovieSceneTrack* Track = MCPToolSequencerUtils::ResolveTrackById(TrackId);
	if (Track == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("track_id was not found.");
		Diagnostic.Detail = TrackId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
	if (MovieScene == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("track_id does not belong to a valid MovieScene.");
		Diagnostic.Detail = TrackId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FFrameNumber StartFrame;
	FFrameNumber EndFrame;
	FMCPDiagnostic TimeDiagnostic;
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("start_frame"), TEXT("start_seconds"), MovieScene->GetTickResolution(), StartFrame, true, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("end_frame"), TEXT("end_seconds"), MovieScene->GetTickResolution(), EndFrame, true, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (EndFrame <= StartFrame)
	{
		EndFrame = StartFrame + 1;
	}

	UMovieSceneSection* Section = nullptr;
	if (!Request.Context.bDryRun)
	{
		Track->Modify();
		Section = Track->CreateNewSection();
		if (Section == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to create section.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
		Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
		Track->AddSection(*Section);
		Track->MarkPackageDirty();
	}

	AppendTouchedPackages(MovieScene, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("added"), true);
	OutResult.ResultObject->SetStringField(TEXT("section_id"), Section ? MCPToolSequencerUtils::BuildSectionId(Section) : FString());
	OutResult.ResultObject->SetStringField(TEXT("track_id"), TrackId);
	OutResult.ResultObject->SetNumberField(TEXT("start_frame"), StartFrame.Value);
	OutResult.ResultObject->SetNumberField(TEXT("end_frame"), EndFrame.Value);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleSectionPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString SectionId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("section_id"), SectionId);
	}

	UMovieSceneSection* Section = MCPToolSequencerUtils::ResolveSectionById(SectionId);
	if (Section == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("section_id was not found.");
		Diagnostic.Detail = SectionId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieSceneTrack* Track = Section->GetTypedOuter<UMovieSceneTrack>();
	UMovieScene* MovieScene = Section->GetTypedOuter<UMovieScene>();
	if (Track == nullptr || MovieScene == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("section_id has invalid ownership.");
		Diagnostic.Detail = SectionId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FFrameNumber StartFrame;
	FFrameNumber EndFrame;
	const TRange<FFrameNumber> CurrentRange = Section->GetRange();
	StartFrame = CurrentRange.GetLowerBound().IsClosed() ? CurrentRange.GetLowerBoundValue() : FFrameNumber(0);
	EndFrame = CurrentRange.GetUpperBound().IsClosed() ? CurrentRange.GetUpperBoundValue() : (StartFrame + 1);

	FMCPDiagnostic TimeDiagnostic;
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("start_frame"), TEXT("start_seconds"), MovieScene->GetTickResolution(), StartFrame, false, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("end_frame"), TEXT("end_seconds"), MovieScene->GetTickResolution(), EndFrame, false, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}
	if (EndFrame <= StartFrame)
	{
		EndFrame = StartFrame + 1;
	}

	int32 RowIndex = Section->GetRowIndex();
	bool bHasRowIndex = false;
	bool bIsActive = Section->IsActive();
	bool bHasActive = false;
	if (Request.Params.IsValid())
	{
		double RowValue = static_cast<double>(RowIndex);
		if (Request.Params->TryGetNumberField(TEXT("row_index"), RowValue))
		{
			RowIndex = static_cast<int32>(RowValue);
			bHasRowIndex = true;
		}
		if (Request.Params->TryGetBoolField(TEXT("is_active"), bIsActive))
		{
			bHasActive = true;
		}
	}

	if (!Request.Context.bDryRun)
	{
		Section->Modify();
		Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
		if (bHasRowIndex)
		{
			Section->SetRowIndex(RowIndex);
		}
		if (bHasActive)
		{
			Section->SetIsActive(bIsActive);
		}
		Section->MarkPackageDirty();
	}

	AppendTouchedPackages(MovieScene, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("patched"), true);
	OutResult.ResultObject->SetStringField(TEXT("section_id"), SectionId);
	OutResult.ResultObject->SetNumberField(TEXT("start_frame"), StartFrame.Value);
	OutResult.ResultObject->SetNumberField(TEXT("end_frame"), EndFrame.Value);
	OutResult.ResultObject->SetNumberField(TEXT("row_index"), RowIndex);
	OutResult.ResultObject->SetBoolField(TEXT("is_active"), bIsActive);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleSectionRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString SectionId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("section_id"), SectionId);
	}

	UMovieSceneSection* Section = MCPToolSequencerUtils::ResolveSectionById(SectionId);
	if (Section == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("section_id was not found.");
		Diagnostic.Detail = SectionId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieSceneTrack* Track = Section->GetTypedOuter<UMovieSceneTrack>();
	if (Track == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("section has no owning track.");
		Diagnostic.Detail = SectionId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!Request.Context.bDryRun)
	{
		Track->Modify();
		Track->RemoveSection(*Section);
		Track->MarkPackageDirty();
	}

	UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
	AppendTouchedPackages(MovieScene, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("removed"), true);
	OutResult.ResultObject->SetStringField(TEXT("section_id"), SectionId);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandlePlaybackPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	const TRange<FFrameNumber> CurrentRange = MovieScene->GetPlaybackRange();
	FFrameNumber StartFrame = CurrentRange.GetLowerBound().IsClosed() ? CurrentRange.GetLowerBoundValue() : FFrameNumber(0);
	FFrameNumber EndFrame = CurrentRange.GetUpperBound().IsClosed() ? CurrentRange.GetUpperBoundValue() : (StartFrame + 1);

	FMCPDiagnostic TimeDiagnostic;
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("start_frame"), TEXT("start_seconds"), MovieScene->GetTickResolution(), StartFrame, false, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}
	if (!MCPToolSequencerUtils::ParseFrameOrSeconds(Request.Params, TEXT("end_frame"), TEXT("end_seconds"), MovieScene->GetTickResolution(), EndFrame, false, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (EndFrame <= StartFrame)
	{
		EndFrame = StartFrame + 1;
	}

	if (!Request.Context.bDryRun)
	{
		MovieScene->Modify();
		SetPlaybackRangeCompat(MovieScene, StartFrame, EndFrame, 0);

		const double StartSeconds = MCPToolSequencerUtils::FrameToSeconds(StartFrame, MovieScene->GetTickResolution());
		const double EndSeconds = MCPToolSequencerUtils::FrameToSeconds(EndFrame, MovieScene->GetTickResolution());
		SetViewRangeCompat(MovieScene, StartSeconds, EndSeconds, 0);
		SetWorkRangeCompat(MovieScene, StartSeconds, EndSeconds, 0);
		MovieScene->MarkPackageDirty();
		Sequence->MarkPackageDirty();
	}

	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("patched"), true);
	OutResult.ResultObject->SetNumberField(TEXT("playback_start_frame"), StartFrame.Value);
	OutResult.ResultObject->SetNumberField(TEXT("playback_end_frame"), EndFrame.Value);
	OutResult.ResultObject->SetNumberField(TEXT("playback_start_seconds"), MCPToolSequencerUtils::FrameToSeconds(StartFrame, MovieScene->GetTickResolution()));
	OutResult.ResultObject->SetNumberField(TEXT("playback_end_seconds"), MCPToolSequencerUtils::FrameToSeconds(EndFrame, MovieScene->GetTickResolution()));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerStructureHandler::HandleSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	if (OutResult.TouchedPackages.Num() == 0)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SAVE_FAILED;
		Diagnostic.Message = TEXT("No package to save for sequence.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bSaved = true;
	if (!Request.Context.bDryRun)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			if (!MCPToolAssetUtils::SavePackageByName(PackageName, OutResult))
			{
				bSaved = false;
			}
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("saved"), bSaved || Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}
