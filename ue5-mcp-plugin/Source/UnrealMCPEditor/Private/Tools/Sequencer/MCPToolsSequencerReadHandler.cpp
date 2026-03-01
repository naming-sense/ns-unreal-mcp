#include "Tools/Sequencer/MCPToolsSequencerReadHandler.h"

#include "MCPErrorCodes.h"
#include "Tools/Asset/MCPToolsAssetLifecycleHandler.h"
#include "Tools/Common/MCPToolAssetUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPSequencerApiCompat.h"
#include "Tools/Common/MCPToolSequencerUtils.h"
#include "Tools/Common/MCPToolSettingsUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "ObjectTools.h"
#include "Sections/MovieSceneFloatSection.h"
#include "UObject/Package.h"

#if __has_include("Sections/MovieSceneDoubleSection.h")
#include "Sections/MovieSceneDoubleSection.h"
#define MCP_HAS_DOUBLE_SECTION 1
#else
#define MCP_HAS_DOUBLE_SECTION 0
#endif

#if __has_include("Sections/MovieSceneBoolSection.h")
#include "Sections/MovieSceneBoolSection.h"
#define MCP_HAS_BOOL_SECTION 1
#else
#define MCP_HAS_BOOL_SECTION 0
#endif

namespace
{
	using MCPToolCommonJson::ToJsonStringArray;

	template <typename TMovieScene>
	auto SetTickResolutionCompat(TMovieScene* MovieScene, const FFrameRate& TickResolution, int)
		-> decltype(MovieScene->SetTickResolutionDirectly(TickResolution), void())
	{
		MovieScene->SetTickResolutionDirectly(TickResolution);
	}

	template <typename TMovieScene>
	auto SetTickResolutionCompat(TMovieScene* MovieScene, const FFrameRate& TickResolution, long)
		-> decltype(MovieScene->SetTickResolution(TickResolution), void())
	{
		MovieScene->SetTickResolution(TickResolution);
	}

	bool ParseSaveAutoOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		return MCPToolSettingsUtils::ParseAutoSaveOption(Params, bOutAutoSave);
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

	void AddChannelEntry(
		const FString& SectionId,
		const FString& TrackId,
		const FString& ChannelType,
		const int32 ChannelIndex,
		TArray<TSharedPtr<FJsonValue>>& OutChannels)
	{
		TSharedRef<FJsonObject> ChannelObject = MakeShared<FJsonObject>();
		ChannelObject->SetStringField(TEXT("channel_id"), MCPToolSequencerUtils::BuildChannelId(SectionId, ChannelType, ChannelIndex));
		ChannelObject->SetStringField(TEXT("section_id"), SectionId);
		ChannelObject->SetStringField(TEXT("track_id"), TrackId);
		ChannelObject->SetStringField(TEXT("channel_type"), ChannelType);
		ChannelObject->SetNumberField(TEXT("channel_index"), ChannelIndex);
		OutChannels.Add(MakeShared<FJsonValueObject>(ChannelObject));
	}

	void AppendSectionChannels(UMovieSceneSection* Section, TArray<TSharedPtr<FJsonValue>>& OutChannels)
	{
		if (Section == nullptr)
		{
			return;
		}

		const FString SectionId = MCPToolSequencerUtils::BuildSectionId(Section);
		const UMovieSceneTrack* OwningTrack = Section->GetTypedOuter<UMovieSceneTrack>();
		const FString TrackId = MCPToolSequencerUtils::BuildTrackId(OwningTrack);

		if (Cast<UMovieSceneFloatSection>(Section) != nullptr)
		{
			AddChannelEntry(SectionId, TrackId, TEXT("float"), 0, OutChannels);
		}

#if MCP_HAS_DOUBLE_SECTION
		if (Cast<UMovieSceneDoubleSection>(Section) != nullptr)
		{
			AddChannelEntry(SectionId, TrackId, TEXT("double"), 0, OutChannels);
		}
#endif

#if MCP_HAS_BOOL_SECTION
		if (Cast<UMovieSceneBoolSection>(Section) != nullptr)
		{
			AddChannelEntry(SectionId, TrackId, TEXT("bool"), 0, OutChannels);
		}
#endif
	}

}

bool FMCPToolsSequencerReadHandler::HandleAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString PackagePath;
	FString AssetName;
	FString FactoryClassPath;
	bool bOverwrite = false;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("package_path"), PackagePath);
		Request.Params->TryGetStringField(TEXT("asset_name"), AssetName);
		Request.Params->TryGetStringField(TEXT("factory_class_path"), FactoryClassPath);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		ParseSaveAutoOption(Request.Params, bAutoSave);
	}

	AssetName = ObjectTools::SanitizeObjectName(AssetName);
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("package_path and asset_name are required for seq.asset.create.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!MCPToolAssetUtils::IsValidAssetDestination(PackagePath, AssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid package_path or asset_name for seq.asset.create.");
		Diagnostic.Detail = FString::Printf(TEXT("package_path=%s asset_name=%s"), *PackagePath, *AssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString ObjectPath = MCPToolAssetUtils::BuildObjectPath(PackagePath, AssetName);
	const FFrameRate DefaultDisplayRate(30, 1);
	const FFrameRate DefaultTickResolution(24000, 1);
	FFrameRate DisplayRate = DefaultDisplayRate;
	FFrameRate TickResolution = DefaultTickResolution;
	if (Request.Params.IsValid())
	{
		FMCPDiagnostic RateDiagnostic;
		if (!MCPToolSequencerUtils::ParseFrameRateField(Request.Params, TEXT("display_rate"), DefaultDisplayRate, DisplayRate, RateDiagnostic))
		{
			OutResult.Diagnostics.Add(RateDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
		if (!MCPToolSequencerUtils::ParseFrameRateField(Request.Params, TEXT("tick_resolution"), DefaultTickResolution, TickResolution, RateDiagnostic))
		{
			OutResult.Diagnostics.Add(RateDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}

	TSharedRef<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("package_path"), PackagePath);
	CreateParams->SetStringField(TEXT("asset_name"), AssetName);
	CreateParams->SetStringField(TEXT("asset_class_path"), TEXT("/Script/LevelSequence.LevelSequence"));
	CreateParams->SetBoolField(TEXT("overwrite"), bOverwrite);

	if (!FactoryClassPath.IsEmpty())
	{
		CreateParams->SetStringField(TEXT("factory_class_path"), FactoryClassPath);
	}
	else if (MCPToolAssetUtils::ResolveClassByPath(TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew")) != nullptr)
	{
		CreateParams->SetStringField(TEXT("factory_class_path"), TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"));
	}

	if (bAutoSave)
	{
		TSharedRef<FJsonObject> SaveObject = MakeShared<FJsonObject>();
		SaveObject->SetBoolField(TEXT("auto_save"), true);
		CreateParams->SetObjectField(TEXT("save"), SaveObject);
	}

	FMCPRequestEnvelope CreateRequest = Request;
	CreateRequest.Params = CreateParams;

	if (!FMCPToolsAssetLifecycleHandler::HandleCreate(
			CreateRequest,
			OutResult,
			[](const FString& InPackagePath, const FString& InAssetName)
			{
				return MCPToolAssetUtils::IsValidAssetDestination(InPackagePath, InAssetName);
			},
			[](const FString& InPackagePath, const FString& InAssetName)
			{
				return MCPToolAssetUtils::BuildPackageName(InPackagePath, InAssetName);
			},
			[](const FString& InPackagePath, const FString& InAssetName)
			{
				return MCPToolAssetUtils::BuildObjectPath(InPackagePath, InAssetName);
			},
			[](const FString& InObjectPath)
			{
				return MCPToolAssetUtils::ResolveObjectByPath(InObjectPath);
			},
			[](const FString& InClassPath)
			{
				return MCPToolAssetUtils::ResolveClassByPath(InClassPath);
			},
			[](const FString& InPackageName, FMCPToolExecutionResult& InOutResult)
			{
				return MCPToolAssetUtils::SavePackageByName(InPackageName, InOutResult);
			}))
	{
		return false;
	}

	if (!Request.Context.bDryRun)
	{
		FMCPDiagnostic LoadDiagnostic;
		ULevelSequence* Sequence = MCPToolSequencerUtils::LoadLevelSequenceByPath(ObjectPath, LoadDiagnostic);
		if (Sequence != nullptr)
		{
			UMovieScene* MovieScene = MCPToolSequencerUtils::ResolveMovieScene(Sequence, LoadDiagnostic);
			if (MovieScene != nullptr)
			{
				MovieScene->Modify();
				MovieScene->SetDisplayRate(DisplayRate);
				SetTickResolutionCompat(MovieScene, TickResolution, 0);
				MovieScene->MarkPackageDirty();
				Sequence->MarkPackageDirty();
			}
		}
	}

	if (!OutResult.ResultObject.IsValid())
	{
		OutResult.ResultObject = MakeShared<FJsonObject>();
	}

	OutResult.ResultObject->SetStringField(TEXT("object_path"), ObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("class_path"), TEXT("/Script/LevelSequence.LevelSequence"));
	OutResult.ResultObject->SetStringField(TEXT("display_rate"), DisplayRate.ToPrettyText().ToString());
	OutResult.ResultObject->SetStringField(TEXT("tick_resolution"), TickResolution.ToPrettyText().ToString());
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = OutResult.Status == EMCPResponseStatus::Error ? EMCPResponseStatus::Error : OutResult.Status;
	return OutResult.Status != EMCPResponseStatus::Error;
}

bool FMCPToolsSequencerReadHandler::HandleAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	}

	FMCPDiagnostic Diagnostic;
	ULevelSequence* Sequence = MCPToolSequencerUtils::LoadLevelSequenceByPath(ObjectPath, Diagnostic);
	if (Sequence == nullptr)
	{
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.ResultObject = MakeShared<FJsonObject>();
		OutResult.ResultObject->SetBoolField(TEXT("loaded"), false);
		OutResult.ResultObject->SetStringField(TEXT("object_path"), ObjectPath);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("loaded"), true);
	OutResult.ResultObject->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutResult.ResultObject->SetStringField(TEXT("class_path"), Sequence->GetClass()->GetPathName());
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerReadHandler::HandleInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();

	int32 PlaybackStartFrame = 0;
	int32 PlaybackEndFrame = 0;
	if (PlaybackRange.GetLowerBound().IsClosed())
	{
		PlaybackStartFrame = PlaybackRange.GetLowerBoundValue().Value;
	}
	if (PlaybackRange.GetUpperBound().IsClosed())
	{
		PlaybackEndFrame = PlaybackRange.GetUpperBoundValue().Value;
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutResult.ResultObject->SetStringField(TEXT("display_rate"), DisplayRate.ToPrettyText().ToString());
	OutResult.ResultObject->SetStringField(TEXT("tick_resolution"), TickResolution.ToPrettyText().ToString());
	OutResult.ResultObject->SetNumberField(TEXT("playback_start_frame"), PlaybackStartFrame);
	OutResult.ResultObject->SetNumberField(TEXT("playback_end_frame"), PlaybackEndFrame);
	OutResult.ResultObject->SetNumberField(TEXT("playback_start_seconds"), MCPToolSequencerUtils::FrameToSeconds(FFrameNumber(PlaybackStartFrame), TickResolution));
	OutResult.ResultObject->SetNumberField(TEXT("playback_end_seconds"), MCPToolSequencerUtils::FrameToSeconds(FFrameNumber(PlaybackEndFrame), TickResolution));
	OutResult.ResultObject->SetNumberField(TEXT("master_track_count"), MCPSequencerApiCompat::GetGlobalTracks(MovieScene).Num());
	OutResult.ResultObject->SetNumberField(TEXT("binding_count"), MCPSequencerApiCompat::GetBindingsConst(MovieScene).Num());
	OutResult.ResultObject->SetNumberField(TEXT("spawnable_count"), MovieScene->GetSpawnableCount());
	OutResult.ResultObject->SetNumberField(TEXT("possessable_count"), MovieScene->GetPossessableCount());
	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerReadHandler::HandleBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> BindingValues;
	const TArray<FMovieSceneBinding>& Bindings = MCPSequencerApiCompat::GetBindingsConst(MovieScene);
	for (const FMovieSceneBinding& Binding : Bindings)
	{
		const FGuid BindingGuid = Binding.GetObjectGuid();
		const bool bIsSpawnable = MCPSequencerApiCompat::IsSpawnableBinding(MovieScene, BindingGuid);
		const bool bIsPossessable = MCPSequencerApiCompat::IsPossessableBinding(MovieScene, BindingGuid);

		TSharedRef<FJsonObject> BindingObject = MakeShared<FJsonObject>();
		BindingObject->SetStringField(TEXT("binding_id"), MCPToolSequencerUtils::BuildBindingId(BindingGuid));
		BindingObject->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		BindingObject->SetStringField(TEXT("display_name"), MCPSequencerApiCompat::ResolveBindingDisplayName(MovieScene, Binding));
		BindingObject->SetBoolField(TEXT("spawnable"), bIsSpawnable);
		BindingObject->SetBoolField(TEXT("possessable"), bIsPossessable);
		BindingObject->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());
		BindingValues.Add(MakeShared<FJsonValueObject>(BindingObject));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("bindings"), BindingValues);
	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerReadHandler::HandleTrackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
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

	TArray<TSharedPtr<FJsonValue>> TrackValues;

	if (BindingId.IsEmpty())
	{
		for (UMovieSceneTrack* MasterTrack : MCPSequencerApiCompat::GetGlobalTracks(MovieScene))
		{
			if (MasterTrack == nullptr)
			{
				continue;
			}

			TSharedRef<FJsonObject> TrackObject = MakeShared<FJsonObject>();
			TrackObject->SetStringField(TEXT("track_id"), MCPToolSequencerUtils::BuildTrackId(MasterTrack));
			TrackObject->SetStringField(TEXT("binding_id"), TEXT(""));
			TrackObject->SetStringField(TEXT("class_path"), MasterTrack->GetClass()->GetPathName());
			TrackObject->SetStringField(TEXT("display_name"), MasterTrack->GetDisplayName().ToString());
			TrackObject->SetNumberField(TEXT("section_count"), MasterTrack->GetAllSections().Num());
			TrackValues.Add(MakeShared<FJsonValueObject>(TrackObject));
		}

		const TArray<FMovieSceneBinding>& Bindings = MCPSequencerApiCompat::GetBindingsConst(MovieScene);
		for (const FMovieSceneBinding& Binding : Bindings)
		{
			const FString CurrentBindingId = MCPToolSequencerUtils::BuildBindingId(Binding.GetObjectGuid());
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				if (Track == nullptr)
				{
					continue;
				}

				TSharedRef<FJsonObject> TrackObject = MakeShared<FJsonObject>();
				TrackObject->SetStringField(TEXT("track_id"), MCPToolSequencerUtils::BuildTrackId(Track));
				TrackObject->SetStringField(TEXT("binding_id"), CurrentBindingId);
				TrackObject->SetStringField(TEXT("class_path"), Track->GetClass()->GetPathName());
				TrackObject->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
				TrackObject->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
				TrackValues.Add(MakeShared<FJsonValueObject>(TrackObject));
			}
		}
	}
	else
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

		for (UMovieSceneTrack* Track : Binding->GetTracks())
		{
			if (Track == nullptr)
			{
				continue;
			}
			TSharedRef<FJsonObject> TrackObject = MakeShared<FJsonObject>();
			TrackObject->SetStringField(TEXT("track_id"), MCPToolSequencerUtils::BuildTrackId(Track));
			TrackObject->SetStringField(TEXT("binding_id"), MCPToolSequencerUtils::BuildBindingId(BindingGuid));
			TrackObject->SetStringField(TEXT("class_path"), Track->GetClass()->GetPathName());
			TrackObject->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
			TrackObject->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
			TrackValues.Add(MakeShared<FJsonValueObject>(TrackObject));
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("tracks"), TrackValues);
	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerReadHandler::HandleSectionList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	FString TrackId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("track_id"), TrackId);
	}

	TArray<UMovieSceneTrack*> TracksToInspect;
	if (TrackId.IsEmpty())
	{
		TracksToInspect.Append(MCPSequencerApiCompat::GetGlobalTracks(MovieScene));
		const TArray<FMovieSceneBinding>& Bindings = MCPSequencerApiCompat::GetBindingsConst(MovieScene);
		for (const FMovieSceneBinding& Binding : Bindings)
		{
			TracksToInspect.Append(Binding.GetTracks());
		}
	}
	else
	{
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
		TracksToInspect.Add(Track);
	}

	TArray<TSharedPtr<FJsonValue>> SectionValues;
	for (UMovieSceneTrack* Track : TracksToInspect)
	{
		if (Track == nullptr)
		{
			continue;
		}

		const FString CurrentTrackId = MCPToolSequencerUtils::BuildTrackId(Track);
		for (UMovieSceneSection* Section : Track->GetAllSections())
		{
			if (Section == nullptr)
			{
				continue;
			}

			const TRange<FFrameNumber> Range = Section->GetRange();
			const int32 StartFrame = Range.GetLowerBound().IsClosed() ? Range.GetLowerBoundValue().Value : 0;
			const int32 EndFrame = Range.GetUpperBound().IsClosed() ? Range.GetUpperBoundValue().Value : 0;

			TSharedRef<FJsonObject> SectionObject = MakeShared<FJsonObject>();
			SectionObject->SetStringField(TEXT("section_id"), MCPToolSequencerUtils::BuildSectionId(Section));
			SectionObject->SetStringField(TEXT("track_id"), CurrentTrackId);
			SectionObject->SetStringField(TEXT("class_path"), Section->GetClass()->GetPathName());
			SectionObject->SetNumberField(TEXT("start_frame"), StartFrame);
			SectionObject->SetNumberField(TEXT("end_frame"), EndFrame);
			SectionObject->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
			SectionObject->SetBoolField(TEXT("is_active"), Section->IsActive());
			SectionValues.Add(MakeShared<FJsonValueObject>(SectionObject));
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("sections"), SectionValues);
	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsSequencerReadHandler::HandleChannelList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	ULevelSequence* Sequence = nullptr;
	UMovieScene* MovieScene = nullptr;
	if (!ResolveSequenceFromRequest(Request, Sequence, MovieScene, OutResult))
	{
		return false;
	}

	FString SectionId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("section_id"), SectionId);
	}

	TArray<TSharedPtr<FJsonValue>> ChannelValues;
	if (!SectionId.IsEmpty())
	{
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
		AppendSectionChannels(Section, ChannelValues);
	}
	else
	{
		for (UMovieSceneTrack* MasterTrack : MCPSequencerApiCompat::GetGlobalTracks(MovieScene))
		{
			if (MasterTrack == nullptr)
			{
				continue;
			}
			for (UMovieSceneSection* Section : MasterTrack->GetAllSections())
			{
				AppendSectionChannels(Section, ChannelValues);
			}
		}

		const TArray<FMovieSceneBinding>& Bindings = MCPSequencerApiCompat::GetBindingsConst(MovieScene);
		for (const FMovieSceneBinding& Binding : Bindings)
		{
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				if (Track == nullptr)
				{
					continue;
				}
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					AppendSectionChannels(Section, ChannelValues);
				}
			}
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("channels"), ChannelValues);
	MCPToolSequencerUtils::AppendTouchedSequencePackage(Sequence, OutResult.TouchedPackages);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}
