#include "Tools/Common/MCPSequencerApiCompat.h"

#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "Channels/MovieSceneBoolChannel.h"

namespace
{
	template <typename TMovieScene>
	auto GetGlobalTracksImpl(const TMovieScene* MovieScene, int)
		-> decltype(MovieScene->GetTracks())
	{
		return MovieScene->GetTracks();
	}

	template <typename TMovieScene>
	auto GetGlobalTracksImpl(const TMovieScene* MovieScene, long)
		-> decltype(MovieScene->GetMasterTracks())
	{
		return MovieScene->GetMasterTracks();
	}

	template <typename TMovieScene>
	auto GetBindingsConstImpl(const TMovieScene* MovieScene, int)
		-> decltype(MovieScene->GetBindings())
	{
		return MovieScene->GetBindings();
	}

	template <typename TMovieScene>
	const TArray<FMovieSceneBinding>& GetBindingsConstImpl(const TMovieScene* MovieScene, long)
	{
		return const_cast<TMovieScene*>(MovieScene)->GetBindings();
	}

	template <typename TMovieScene>
	auto AddGlobalTrackImpl(TMovieScene* MovieScene, UClass* TrackClass, int)
		-> decltype(MovieScene->AddTrack(TrackClass))
	{
		return MovieScene->AddTrack(TrackClass);
	}

	template <typename TMovieScene>
	auto AddGlobalTrackImpl(TMovieScene* MovieScene, UClass* TrackClass, long)
		-> decltype(MovieScene->AddMasterTrack(TrackClass))
	{
		return MovieScene->AddMasterTrack(TrackClass);
	}

	template <typename TMovieScene>
	auto RemoveTrackImpl(TMovieScene* MovieScene, UMovieSceneTrack* Track, int)
		-> decltype(MovieScene->RemoveTrack(*Track), bool())
	{
		return Track != nullptr ? MovieScene->RemoveTrack(*Track) : false;
	}

	template <typename TMovieScene>
	auto RemoveTrackImpl(TMovieScene* MovieScene, UMovieSceneTrack* Track, long)
		-> decltype(MovieScene->RemoveMasterTrack(*Track), bool())
	{
		return Track != nullptr ? MovieScene->RemoveMasterTrack(*Track) : false;
	}

	template <typename TBoolChannel>
	auto AddBoolKeyImpl(TBoolChannel* Channel, const FFrameNumber FrameNumber, const bool bValue, int)
		-> decltype(Channel->GetData().AddKey(FrameNumber, bValue), bool())
	{
		Channel->GetData().AddKey(FrameNumber, bValue);
		return true;
	}

	template <typename TBoolChannel>
	auto AddBoolKeyImpl(TBoolChannel* Channel, const FFrameNumber FrameNumber, const bool bValue, long)
		-> decltype(Channel->AddKey(FrameNumber, bValue), bool())
	{
		Channel->AddKey(FrameNumber, bValue);
		return true;
	}
}

namespace MCPSequencerApiCompat
{
	const TArray<UMovieSceneTrack*>& GetGlobalTracks(const UMovieScene* MovieScene)
	{
		static const TArray<UMovieSceneTrack*> EmptyTracks;
		if (MovieScene == nullptr)
		{
			return EmptyTracks;
		}
		return GetGlobalTracksImpl(MovieScene, 0);
	}

	const TArray<FMovieSceneBinding>& GetBindingsConst(const UMovieScene* MovieScene)
	{
		static const TArray<FMovieSceneBinding> EmptyBindings;
		if (MovieScene == nullptr)
		{
			return EmptyBindings;
		}
		return GetBindingsConstImpl(MovieScene, 0);
	}

	bool IsSpawnableBinding(UMovieScene* MovieScene, const FGuid& BindingGuid)
	{
		return MovieScene != nullptr && MovieScene->FindSpawnable(BindingGuid) != nullptr;
	}

	bool IsPossessableBinding(UMovieScene* MovieScene, const FGuid& BindingGuid)
	{
		return MovieScene != nullptr && MovieScene->FindPossessable(BindingGuid) != nullptr;
	}

	FString ResolveBindingDisplayName(UMovieScene* MovieScene, const FMovieSceneBinding& Binding)
	{
		if (MovieScene == nullptr)
		{
			return FString();
		}

		const FGuid BindingGuid = Binding.GetObjectGuid();
		if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
		{
			return Spawnable->GetName();
		}
		if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid))
		{
			return Possessable->GetName();
		}

		return BindingGuid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	UMovieSceneTrack* AddGlobalTrack(UMovieScene* MovieScene, UClass* TrackClass)
	{
		if (MovieScene == nullptr || TrackClass == nullptr)
		{
			return nullptr;
		}
		return AddGlobalTrackImpl(MovieScene, TrackClass, 0);
	}

	bool RemoveTrackSafe(UMovieScene* MovieScene, UMovieSceneTrack* Track)
	{
		if (MovieScene == nullptr || Track == nullptr)
		{
			return false;
		}
		return RemoveTrackImpl(MovieScene, Track, 0);
	}

	bool RemoveBindingSafe(UMovieScene* MovieScene, const FGuid& BindingGuid)
	{
		if (MovieScene == nullptr || !BindingGuid.IsValid())
		{
			return false;
		}

		if (IsSpawnableBinding(MovieScene, BindingGuid))
		{
			return MovieScene->RemoveSpawnable(BindingGuid);
		}

		if (IsPossessableBinding(MovieScene, BindingGuid))
		{
			return MovieScene->RemovePossessable(BindingGuid);
		}

		FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
		if (Binding == nullptr)
		{
			return false;
		}

		bool bAnyTrackRemoved = false;
		TArray<UMovieSceneTrack*> TracksToRemove = Binding->GetTracks();
		for (UMovieSceneTrack* Track : TracksToRemove)
		{
			bAnyTrackRemoved = RemoveTrackSafe(MovieScene, Track) || bAnyTrackRemoved;
		}

		return bAnyTrackRemoved || MovieScene->FindBinding(BindingGuid) == nullptr;
	}

	bool AddBoolKeySafe(FMovieSceneBoolChannel* Channel, const FFrameNumber FrameNumber, const bool bValue)
	{
		if (Channel == nullptr)
		{
			return false;
		}

		return AddBoolKeyImpl(Channel, FrameNumber, bValue, 0);
	}
}
