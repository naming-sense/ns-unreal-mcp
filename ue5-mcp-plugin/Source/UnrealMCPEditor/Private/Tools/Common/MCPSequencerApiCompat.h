#pragma once

#include "CoreMinimal.h"

class UClass;
class UMovieScene;
class UMovieSceneTrack;
struct FFrameNumber;
struct FGuid;
struct FMovieSceneBinding;
struct FMovieSceneBoolChannel;

namespace MCPSequencerApiCompat
{
	const TArray<UMovieSceneTrack*>& GetGlobalTracks(const UMovieScene* MovieScene);
	const TArray<FMovieSceneBinding>& GetBindingsConst(const UMovieScene* MovieScene);

	bool IsSpawnableBinding(UMovieScene* MovieScene, const FGuid& BindingGuid);
	bool IsPossessableBinding(UMovieScene* MovieScene, const FGuid& BindingGuid);
	FString ResolveBindingDisplayName(UMovieScene* MovieScene, const FMovieSceneBinding& Binding);

	UMovieSceneTrack* AddGlobalTrack(UMovieScene* MovieScene, UClass* TrackClass);
	bool RemoveTrackSafe(UMovieScene* MovieScene, UMovieSceneTrack* Track);
	bool RemoveBindingSafe(UMovieScene* MovieScene, const FGuid& BindingGuid);

	bool AddBoolKeySafe(FMovieSceneBoolChannel* Channel, const FFrameNumber FrameNumber, bool bValue);
}
