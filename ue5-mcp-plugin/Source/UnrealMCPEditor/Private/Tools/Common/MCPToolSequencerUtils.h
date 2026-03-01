#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class FJsonObject;
class ULevelSequence;
class UMovieScene;
class UMovieSceneTrack;
class UMovieSceneSection;
struct FMovieSceneBinding;

namespace MCPToolSequencerUtils
{
	bool ParseFrameRateField(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		const FFrameRate& DefaultValue,
		FFrameRate& OutFrameRate,
		FMCPDiagnostic& OutDiagnostic);

	bool ParseFrameOrSeconds(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FrameField,
		const FString& SecondsField,
		const FFrameRate& TickResolution,
		FFrameNumber& OutFrame,
		bool bRequired,
		FMCPDiagnostic& OutDiagnostic);

	double FrameToSeconds(const FFrameNumber Frame, const FFrameRate& TickResolution);

	ULevelSequence* LoadLevelSequenceByPath(const FString& ObjectPath, FMCPDiagnostic& OutDiagnostic);
	UMovieScene* ResolveMovieScene(ULevelSequence* Sequence, FMCPDiagnostic& OutDiagnostic);

	FString BuildBindingId(const FGuid& BindingGuid);
	bool ParseBindingId(const FString& BindingId, FGuid& OutBindingGuid);
	const FMovieSceneBinding* FindBindingById(const UMovieScene* MovieScene, const FString& BindingId, FGuid* OutBindingGuid = nullptr);
	FMovieSceneBinding* FindBindingById(UMovieScene* MovieScene, const FString& BindingId, FGuid* OutBindingGuid = nullptr);

	FString BuildTrackId(const UMovieSceneTrack* Track);
	UMovieSceneTrack* ResolveTrackById(const FString& TrackId);

	FString BuildSectionId(const UMovieSceneSection* Section);
	UMovieSceneSection* ResolveSectionById(const FString& SectionId);

	FString BuildChannelId(const FString& SectionId, const FString& ChannelType, int32 ChannelIndex);
	bool ParseChannelId(
		const FString& ChannelId,
		FString& OutSectionId,
		FString& OutChannelType,
		int32& OutChannelIndex,
		FMCPDiagnostic& OutDiagnostic);

	void AppendTouchedSequencePackage(const ULevelSequence* Sequence, TArray<FString>& OutTouchedPackages);
}
