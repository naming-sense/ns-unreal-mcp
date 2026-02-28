#include "Tools/UMG/MCPToolsUMGAnimationHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/Widget.h"
#include "KeyParams.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include <type_traits>

namespace
{
	bool ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		bOutAutoSave = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("auto_save"), bOutAutoSave);
		}
		return true;
	}

	UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	}

	bool TryResolveWidgetAnimationArray(UWidgetBlueprint* WidgetBlueprint, FArrayProperty*& OutArrayProperty, void*& OutArrayPtr)
	{
		OutArrayProperty = nullptr;
		OutArrayPtr = nullptr;
		if (WidgetBlueprint == nullptr)
		{
			return false;
		}

		OutArrayProperty = FindFProperty<FArrayProperty>(WidgetBlueprint->GetClass(), TEXT("Animations"));
		if (OutArrayProperty == nullptr || !OutArrayProperty->Inner->IsA<FObjectPropertyBase>())
		{
			return false;
		}

		OutArrayPtr = OutArrayProperty->ContainerPtrToValuePtr<void>(WidgetBlueprint);
		return OutArrayPtr != nullptr;
	}

	template <typename T, typename = void>
	struct THasWidgetVariableGuidMap : std::false_type
	{
	};

	template <typename T>
	struct THasWidgetVariableGuidMap<T, std::void_t<decltype(std::declval<T>().WidgetVariableNameToGuidMap)>> : std::true_type
	{
	};

	template <typename T, typename = void>
	struct THasOnVariableAdded : std::false_type
	{
	};

	template <typename T>
	struct THasOnVariableAdded<T, std::void_t<decltype(std::declval<T*>()->OnVariableAdded(FName()))>> : std::true_type
	{
	};

	template <typename T, typename = void>
	struct THasOnVariableRemoved : std::false_type
	{
	};

	template <typename T>
	struct THasOnVariableRemoved<T, std::void_t<decltype(std::declval<T*>()->OnVariableRemoved(FName()))>> : std::true_type
	{
	};

	bool EnsureWidgetVariableGuidEntry(UWidgetBlueprint* WidgetBlueprint, const FName VariableName)
	{
		if (WidgetBlueprint == nullptr || VariableName.IsNone())
		{
			return false;
		}

		if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			if (WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(VariableName))
			{
				return false;
			}
		}

		if constexpr (THasOnVariableAdded<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->OnVariableAdded(VariableName);
			return true;
		}
		else if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->WidgetVariableNameToGuidMap.Add(VariableName, FGuid::NewGuid());
			return true;
		}
		else
		{
			return false;
		}
	}

	bool RemoveWidgetVariableGuidEntry(UWidgetBlueprint* WidgetBlueprint, const FName VariableName)
	{
		if (WidgetBlueprint == nullptr || VariableName.IsNone())
		{
			return false;
		}

		if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(VariableName))
			{
				return false;
			}
		}

		if constexpr (THasOnVariableRemoved<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->OnVariableRemoved(VariableName);
			return true;
		}
		else if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			return WidgetBlueprint->WidgetVariableNameToGuidMap.Remove(VariableName) > 0;
		}
		else
		{
			return false;
		}
	}

	bool EnsureAnimationMovieScene(UWidgetAnimation* Animation)
	{
		if (Animation == nullptr || Animation->MovieScene != nullptr)
		{
			return false;
		}

		Animation->Modify();
		UMovieScene* NewMovieScene = NewObject<UMovieScene>(Animation, NAME_None, RF_Transactional);
		if (NewMovieScene == nullptr)
		{
			return false;
		}

		Animation->MovieScene = NewMovieScene;
		return true;
	}

	UWidget* ResolveWidgetFromRef(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRefObject)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || WidgetRefObject == nullptr || !WidgetRefObject->IsValid())
		{
			return nullptr;
		}

		FString WidgetId;
		FString WidgetName;
		(*WidgetRefObject)->TryGetStringField(TEXT("widget_id"), WidgetId);
		(*WidgetRefObject)->TryGetStringField(TEXT("name"), WidgetName);

		TArray<FString> CandidateNames;
		if (!WidgetName.IsEmpty())
		{
			CandidateNames.AddUnique(WidgetName);
		}

		if (!WidgetId.IsEmpty())
		{
			if (WidgetId.StartsWith(TEXT("name:")))
			{
				CandidateNames.AddUnique(WidgetId.RightChop(5));
			}
			else
			{
				FGuid ParsedGuid;
				if (!FGuid::Parse(WidgetId, ParsedGuid))
				{
					CandidateNames.AddUnique(WidgetId);
				}
			}
		}

		if (CandidateNames.Num() == 0)
		{
			return nullptr;
		}

		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget == nullptr)
			{
				continue;
			}

			for (const FString& CandidateName : CandidateNames)
			{
				if (Widget->GetName().Equals(CandidateName, ESearchCase::IgnoreCase))
				{
					return Widget;
				}
			}
		}

		return nullptr;
	}

	int32 AddFloatKeyWithInterpolation(
		FMovieSceneFloatChannel* Channel,
		const FFrameNumber FrameNumber,
		const float Value,
		const EMovieSceneKeyInterpolation Interpolation)
	{
		if (Channel == nullptr)
		{
			return INDEX_NONE;
		}

		switch (Interpolation)
		{
		case EMovieSceneKeyInterpolation::Constant:
			return Channel->AddConstantKey(FrameNumber, Value);
		case EMovieSceneKeyInterpolation::Linear:
			return Channel->AddLinearKey(FrameNumber, Value);
		case EMovieSceneKeyInterpolation::Auto:
		case EMovieSceneKeyInterpolation::SmartAuto:
			return Channel->AddCubicKey(FrameNumber, Value, RCTM_Auto);
		case EMovieSceneKeyInterpolation::User:
		case EMovieSceneKeyInterpolation::Break:
			return Channel->AddCubicKey(FrameNumber, Value, RCTM_User);
		default:
			break;
		}

		return Channel->AddCubicKey(FrameNumber, Value, RCTM_Auto);
	}

	enum class EMCPUMGAnimationTrackKind : uint8
	{
		Unknown = 0,
		Float = 1,
		Transform2D = 2,
		Color = 3
	};

	enum class EMCPUMGAnimationChannel : uint8
	{
		None = 0,
		FloatValue = 1,
		TransformTranslationX = 2,
		TransformTranslationY = 3,
		TransformScaleX = 4,
		TransformScaleY = 5,
		TransformShearX = 6,
		TransformShearY = 7,
		TransformRotation = 8,
		ColorR = 9,
		ColorG = 10,
		ColorB = 11,
		ColorA = 12
	};

	struct FMCPUMGAnimationPropertySpec
	{
		EMCPUMGAnimationTrackKind TrackKind = EMCPUMGAnimationTrackKind::Unknown;
		EMCPUMGAnimationChannel Channel = EMCPUMGAnimationChannel::None;
		FName PropertyName = NAME_None;
		FString PropertyPath;
		FString CanonicalPath;
	};

	FString MCPUMGAnim_NormalizePropertyPath(const FString& InPropertyPath)
	{
		FString Normalized = InPropertyPath;
		Normalized.TrimStartAndEndInline();
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		Normalized.ReplaceInline(TEXT("["), TEXT("."));
		Normalized.ReplaceInline(TEXT("]"), TEXT(""));
		while (Normalized.Contains(TEXT("..")))
		{
			Normalized.ReplaceInline(TEXT(".."), TEXT("."));
		}
		return Normalized.ToLower();
	}

	FString MCPUMGAnim_ChannelToString(const EMCPUMGAnimationChannel Channel)
	{
		switch (Channel)
		{
		case EMCPUMGAnimationChannel::FloatValue:
			return TEXT("value");
		case EMCPUMGAnimationChannel::TransformTranslationX:
			return TEXT("translation.x");
		case EMCPUMGAnimationChannel::TransformTranslationY:
			return TEXT("translation.y");
		case EMCPUMGAnimationChannel::TransformScaleX:
			return TEXT("scale.x");
		case EMCPUMGAnimationChannel::TransformScaleY:
			return TEXT("scale.y");
		case EMCPUMGAnimationChannel::TransformShearX:
			return TEXT("shear.x");
		case EMCPUMGAnimationChannel::TransformShearY:
			return TEXT("shear.y");
		case EMCPUMGAnimationChannel::TransformRotation:
			return TEXT("angle");
		case EMCPUMGAnimationChannel::ColorR:
			return TEXT("r");
		case EMCPUMGAnimationChannel::ColorG:
			return TEXT("g");
		case EMCPUMGAnimationChannel::ColorB:
			return TEXT("b");
		case EMCPUMGAnimationChannel::ColorA:
			return TEXT("a");
		default:
			break;
		}
		return FString();
	}

	FString MCPUMGAnim_TrackKindToString(const EMCPUMGAnimationTrackKind TrackKind)
	{
		switch (TrackKind)
		{
		case EMCPUMGAnimationTrackKind::Float:
			return TEXT("float");
		case EMCPUMGAnimationTrackKind::Transform2D:
			return TEXT("transform2d");
		case EMCPUMGAnimationTrackKind::Color:
			return TEXT("color");
		default:
			break;
		}
		return TEXT("unknown");
	}

	bool MCPUMGAnim_ParsePropertySpec(
		const FString& InPropertyPath,
		const bool bRequireChannelForCompositeTrack,
		FMCPUMGAnimationPropertySpec& OutSpec,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutSpec = FMCPUMGAnimationPropertySpec();
		const FString Normalized = MCPUMGAnim_NormalizePropertyPath(InPropertyPath);
		if (Normalized.IsEmpty())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("property_path is required.");
			return false;
		}

		if (Normalized == TEXT("renderopacity"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Float;
			OutSpec.Channel = EMCPUMGAnimationChannel::FloatValue;
			OutSpec.PropertyName = TEXT("RenderOpacity");
			OutSpec.PropertyPath = TEXT("RenderOpacity");
			OutSpec.CanonicalPath = TEXT("RenderOpacity");
			return true;
		}

		if (Normalized == TEXT("rendertransform"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::None;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform");
		}
		else if (Normalized == TEXT("rendertransform.translation.x"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformTranslationX;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Translation.X");
		}
		else if (Normalized == TEXT("rendertransform.translation.y"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformTranslationY;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Translation.Y");
		}
		else if (Normalized == TEXT("rendertransform.scale.x"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformScaleX;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Scale.X");
		}
		else if (Normalized == TEXT("rendertransform.scale.y"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformScaleY;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Scale.Y");
		}
		else if (Normalized == TEXT("rendertransform.shear.x"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformShearX;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Shear.X");
		}
		else if (Normalized == TEXT("rendertransform.shear.y"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformShearY;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Shear.Y");
		}
		else if (Normalized == TEXT("rendertransform.angle") || Normalized == TEXT("rendertransform.rotation"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Transform2D;
			OutSpec.Channel = EMCPUMGAnimationChannel::TransformRotation;
			OutSpec.PropertyName = TEXT("RenderTransform");
			OutSpec.PropertyPath = TEXT("RenderTransform");
			OutSpec.CanonicalPath = TEXT("RenderTransform.Angle");
		}
		else if (Normalized == TEXT("colorandopacity"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Color;
			OutSpec.Channel = EMCPUMGAnimationChannel::None;
			OutSpec.PropertyName = TEXT("ColorAndOpacity");
			OutSpec.PropertyPath = TEXT("ColorAndOpacity");
			OutSpec.CanonicalPath = TEXT("ColorAndOpacity");
		}
		else if (Normalized == TEXT("colorandopacity.r") || Normalized == TEXT("colorandopacity.red"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Color;
			OutSpec.Channel = EMCPUMGAnimationChannel::ColorR;
			OutSpec.PropertyName = TEXT("ColorAndOpacity");
			OutSpec.PropertyPath = TEXT("ColorAndOpacity");
			OutSpec.CanonicalPath = TEXT("ColorAndOpacity.R");
		}
		else if (Normalized == TEXT("colorandopacity.g") || Normalized == TEXT("colorandopacity.green"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Color;
			OutSpec.Channel = EMCPUMGAnimationChannel::ColorG;
			OutSpec.PropertyName = TEXT("ColorAndOpacity");
			OutSpec.PropertyPath = TEXT("ColorAndOpacity");
			OutSpec.CanonicalPath = TEXT("ColorAndOpacity.G");
		}
		else if (Normalized == TEXT("colorandopacity.b") || Normalized == TEXT("colorandopacity.blue"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Color;
			OutSpec.Channel = EMCPUMGAnimationChannel::ColorB;
			OutSpec.PropertyName = TEXT("ColorAndOpacity");
			OutSpec.PropertyPath = TEXT("ColorAndOpacity");
			OutSpec.CanonicalPath = TEXT("ColorAndOpacity.B");
		}
		else if (Normalized == TEXT("colorandopacity.a") || Normalized == TEXT("colorandopacity.alpha"))
		{
			OutSpec.TrackKind = EMCPUMGAnimationTrackKind::Color;
			OutSpec.Channel = EMCPUMGAnimationChannel::ColorA;
			OutSpec.PropertyName = TEXT("ColorAndOpacity");
			OutSpec.PropertyPath = TEXT("ColorAndOpacity");
			OutSpec.CanonicalPath = TEXT("ColorAndOpacity.A");
		}
		else
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Unsupported property_path for UMG animation tool.");
			OutDiagnostic.Detail = InPropertyPath;
			OutDiagnostic.Suggestion = TEXT("Use RenderOpacity / RenderTransform.<Translation|Scale|Shear>.<X|Y> / RenderTransform.Angle / ColorAndOpacity.<R|G|B|A>.");
			return false;
		}

		if (bRequireChannelForCompositeTrack
			&& OutSpec.TrackKind != EMCPUMGAnimationTrackKind::Float
			&& OutSpec.Channel == EMCPUMGAnimationChannel::None)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("property_path must target a concrete channel for key operations.");
			OutDiagnostic.Detail = InPropertyPath;
			OutDiagnostic.Suggestion = TEXT("Example: RenderTransform.Translation.X or ColorAndOpacity.R");
			return false;
		}

		return true;
	}

	bool MCPUMGAnim_ParseInterpolation(
		const TSharedPtr<FJsonObject>& Params,
		EMovieSceneKeyInterpolation& OutInterpolation,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutInterpolation = EMovieSceneKeyInterpolation::Auto;
		if (!Params.IsValid())
		{
			return true;
		}

		FString Interpolation;
		if (!Params->TryGetStringField(TEXT("interpolation"), Interpolation) || Interpolation.IsEmpty())
		{
			return true;
		}

		const FString Normalized = Interpolation.ToLower();
		if (Normalized == TEXT("auto"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Auto;
			return true;
		}
		if (Normalized == TEXT("smart_auto") || Normalized == TEXT("smartauto"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::SmartAuto;
			return true;
		}
		if (Normalized == TEXT("linear"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Linear;
			return true;
		}
		if (Normalized == TEXT("constant"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Constant;
			return true;
		}
		if (Normalized == TEXT("user"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::User;
			return true;
		}
		if (Normalized == TEXT("break"))
		{
			OutInterpolation = EMovieSceneKeyInterpolation::Break;
			return true;
		}

		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("Unsupported interpolation mode.");
		OutDiagnostic.Detail = Interpolation;
		OutDiagnostic.Suggestion = TEXT("Use auto|smart_auto|linear|constant|user|break.");
		return false;
	}

	bool MCPUMGAnim_ParseKeyTime(
		const TSharedPtr<FJsonObject>& Params,
		const FFrameRate& TickResolution,
		FFrameNumber& OutFrame,
		double& OutTimeSeconds,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutFrame = 0;
		OutTimeSeconds = 0.0;
		if (!Params.IsValid())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("params are required.");
			return false;
		}

		double FrameValue = 0.0;
		double TimeSecondsValue = 0.0;
		const bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameValue);
		const bool bHasTimeSeconds = Params->TryGetNumberField(TEXT("time_seconds"), TimeSecondsValue);
		if (!bHasFrame && !bHasTimeSeconds)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Either frame or time_seconds must be provided.");
			return false;
		}

		if (bHasFrame)
		{
			const int32 FrameInt = FMath::RoundToInt(FrameValue);
			if (FrameInt < 0)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("frame must be >= 0.");
				return false;
			}

			OutFrame = FFrameNumber(FrameInt);
			const double TickAsDecimal = TickResolution.AsDecimal();
			OutTimeSeconds = TickAsDecimal > KINDA_SMALL_NUMBER
				? static_cast<double>(OutFrame.Value) / TickAsDecimal
				: 0.0;
			return true;
		}

		if (TimeSecondsValue < 0.0)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("time_seconds must be >= 0.");
			return false;
		}

		OutTimeSeconds = TimeSecondsValue;
		OutFrame = TickResolution.AsFrameTime(TimeSecondsValue).RoundToFrame();
		return true;
	}

	UWidgetAnimation* MCPUMGAnim_FindAnimationByNameOrPath(UWidgetBlueprint* WidgetBlueprint, const FString& AnimationNameOrPath)
	{
		if (WidgetBlueprint == nullptr || AnimationNameOrPath.IsEmpty())
		{
			return nullptr;
		}

		FArrayProperty* AnimationsArrayProperty = nullptr;
		void* AnimationsArrayPtr = nullptr;
		if (!TryResolveWidgetAnimationArray(WidgetBlueprint, AnimationsArrayProperty, AnimationsArrayPtr))
		{
			return nullptr;
		}

		const FObjectPropertyBase* AnimationObjectProperty = CastField<FObjectPropertyBase>(AnimationsArrayProperty->Inner);
		if (AnimationObjectProperty == nullptr)
		{
			return nullptr;
		}

		FScriptArrayHelper ArrayHelper(AnimationsArrayProperty, AnimationsArrayPtr);
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			UWidgetAnimation* Animation = Cast<UWidgetAnimation>(
				AnimationObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index)));
			if (Animation == nullptr)
			{
				continue;
			}

			if (Animation->GetName().Equals(AnimationNameOrPath, ESearchCase::IgnoreCase)
				|| Animation->GetPathName().Equals(AnimationNameOrPath, ESearchCase::IgnoreCase))
			{
				return Animation;
			}
		}

		return nullptr;
	}

	bool MCPUMGAnim_ResolveBindingGuid(
		UWidgetAnimation* Animation,
		UWidget* Widget,
		const bool bCreateIfMissing,
		FGuid& OutBindingGuid,
		bool& bOutBindingCreated,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutBindingGuid.Invalidate();
		bOutBindingCreated = false;
		if (Animation == nullptr || Widget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("animation and widget are required.");
			return false;
		}

		UMovieScene* MovieScene = Animation->GetMovieScene();
		if (MovieScene == nullptr)
		{
			OutDiagnostic.Code = TEXT("MCP.UMG.ANIMATION.NO_MOVIESCENE");
			OutDiagnostic.Message = TEXT("Widget animation has no MovieScene.");
			return false;
		}

		const FName WidgetName = Widget->GetFName();
		for (FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
		{
			if (Binding.WidgetName != WidgetName)
			{
				continue;
			}

			if (!Binding.AnimationGuid.IsValid() || MovieScene->FindPossessable(Binding.AnimationGuid) == nullptr)
			{
				if (!bCreateIfMissing)
				{
					OutDiagnostic.Code = TEXT("MCP.UMG.ANIMATION.BINDING_NOT_FOUND");
					OutDiagnostic.Message = TEXT("Animation binding exists but possessable is missing.");
					OutDiagnostic.Detail = Widget->GetName();
					return false;
				}

				const FGuid NewGuid = MovieScene->AddPossessable(Widget->GetName(), Widget->GetClass());
				if (!NewGuid.IsValid())
				{
					OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
					OutDiagnostic.Message = TEXT("Failed to create MovieScene possessable for widget.");
					OutDiagnostic.Detail = Widget->GetName();
					return false;
				}

				Binding.AnimationGuid = NewGuid;
				bOutBindingCreated = true;
			}

			OutBindingGuid = Binding.AnimationGuid;
			return true;
		}

		if (!bCreateIfMissing)
		{
			OutDiagnostic.Code = TEXT("MCP.UMG.ANIMATION.BINDING_NOT_FOUND");
			OutDiagnostic.Message = TEXT("Widget is not bound to the target animation.");
			OutDiagnostic.Detail = Widget->GetName();
			OutDiagnostic.Suggestion = TEXT("Call umg.animation.track.add first, or provide the correct widget_ref.");
			return false;
		}

		const FGuid NewGuid = MovieScene->AddPossessable(Widget->GetName(), Widget->GetClass());
		if (!NewGuid.IsValid())
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("Failed to create MovieScene possessable for widget.");
			OutDiagnostic.Detail = Widget->GetName();
			return false;
		}

		FWidgetAnimationBinding NewBinding;
		NewBinding.WidgetName = WidgetName;
		NewBinding.SlotWidgetName = NAME_None;
		NewBinding.AnimationGuid = NewGuid;
		NewBinding.bIsRootWidget = false;
		Animation->AnimationBindings.Add(NewBinding);

		OutBindingGuid = NewGuid;
		bOutBindingCreated = true;
		return true;
	}

	template <typename TrackType>
	TrackType* MCPUMGAnim_FindPropertyTrack(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FName PropertyName,
		const FString& PropertyPath)
	{
		if (MovieScene == nullptr || !BindingGuid.IsValid())
		{
			return nullptr;
		}

		const TArray<UMovieSceneTrack*> ExistingTracks = MovieScene->FindTracks(TrackType::StaticClass(), BindingGuid);
		const FName PropertyPathName(*PropertyPath);
		for (UMovieSceneTrack* ExistingTrack : ExistingTracks)
		{
			UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(ExistingTrack);
			if (PropertyTrack == nullptr)
			{
				continue;
			}

			if (PropertyTrack->GetPropertyName() == PropertyName && PropertyTrack->GetPropertyPath() == PropertyPathName)
			{
				return Cast<TrackType>(PropertyTrack);
			}
		}

		return nullptr;
	}

	template <typename TrackType>
	TrackType* MCPUMGAnim_FindOrAddPropertyTrack(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FName PropertyName,
		const FString& PropertyPath,
		const bool bAllowCreate,
		bool& bOutTrackAdded)
	{
		bOutTrackAdded = false;
		TrackType* Track = MCPUMGAnim_FindPropertyTrack<TrackType>(MovieScene, BindingGuid, PropertyName, PropertyPath);
		if (Track != nullptr || !bAllowCreate || MovieScene == nullptr || !BindingGuid.IsValid())
		{
			return Track;
		}

		Track = MovieScene->AddTrack<TrackType>(BindingGuid);
		if (Track == nullptr)
		{
			return nullptr;
		}

		Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
		bOutTrackAdded = true;
		return Track;
	}

	template <typename SectionType>
	SectionType* MCPUMGAnim_FindOrAddSection(UMovieSceneTrack* Track, const bool bAllowCreate, bool& bOutSectionAdded)
	{
		bOutSectionAdded = false;
		if (Track == nullptr)
		{
			return nullptr;
		}

		const TArray<UMovieSceneSection*>& ExistingSections = Track->GetAllSections();
		for (UMovieSceneSection* ExistingSection : ExistingSections)
		{
			if (SectionType* TypedSection = Cast<SectionType>(ExistingSection))
			{
				return TypedSection;
			}
		}

		if (!bAllowCreate)
		{
			return nullptr;
		}

		UMovieSceneSection* NewSection = Track->CreateNewSection();
		SectionType* TypedSection = Cast<SectionType>(NewSection);
		if (TypedSection == nullptr)
		{
			return nullptr;
		}

		Track->AddSection(*TypedSection);
		TypedSection->SetRange(TRange<FFrameNumber>::All());
		bOutSectionAdded = true;
		return TypedSection;
	}

	UMovieSceneTrack* MCPUMGAnim_FindOrAddTrackForSpec(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FMCPUMGAnimationPropertySpec& PropertySpec,
		const bool bAllowCreate,
		bool& bOutTrackAdded)
	{
		bOutTrackAdded = false;
		switch (PropertySpec.TrackKind)
		{
		case EMCPUMGAnimationTrackKind::Float:
			return MCPUMGAnim_FindOrAddPropertyTrack<UMovieSceneFloatTrack>(
				MovieScene,
				BindingGuid,
				PropertySpec.PropertyName,
				PropertySpec.PropertyPath,
				bAllowCreate,
				bOutTrackAdded);
		case EMCPUMGAnimationTrackKind::Transform2D:
			return MCPUMGAnim_FindOrAddPropertyTrack<UMovieScene2DTransformTrack>(
				MovieScene,
				BindingGuid,
				PropertySpec.PropertyName,
				PropertySpec.PropertyPath,
				bAllowCreate,
				bOutTrackAdded);
		case EMCPUMGAnimationTrackKind::Color:
			return MCPUMGAnim_FindOrAddPropertyTrack<UMovieSceneColorTrack>(
				MovieScene,
				BindingGuid,
				PropertySpec.PropertyName,
				PropertySpec.PropertyPath,
				bAllowCreate,
				bOutTrackAdded);
		default:
			break;
		}

		return nullptr;
	}

	FMovieSceneFloatChannel* MCPUMGAnim_ResolveChannelFromTrack(
		UMovieSceneTrack* Track,
		const FMCPUMGAnimationPropertySpec& PropertySpec,
		const bool bAllowCreateSection,
		bool& bOutSectionAdded)
	{
		bOutSectionAdded = false;
		if (Track == nullptr)
		{
			return nullptr;
		}

		switch (PropertySpec.TrackKind)
		{
		case EMCPUMGAnimationTrackKind::Float:
		{
			UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
			UMovieSceneFloatSection* FloatSection = MCPUMGAnim_FindOrAddSection<UMovieSceneFloatSection>(
				FloatTrack, bAllowCreateSection, bOutSectionAdded);
			return FloatSection ? &FloatSection->GetChannel() : nullptr;
		}
		case EMCPUMGAnimationTrackKind::Transform2D:
		{
			UMovieScene2DTransformTrack* TransformTrack = Cast<UMovieScene2DTransformTrack>(Track);
			UMovieScene2DTransformSection* TransformSection = MCPUMGAnim_FindOrAddSection<UMovieScene2DTransformSection>(
				TransformTrack, bAllowCreateSection, bOutSectionAdded);
			if (TransformSection == nullptr)
			{
				return nullptr;
			}

			switch (PropertySpec.Channel)
			{
			case EMCPUMGAnimationChannel::TransformTranslationX:
				return &TransformSection->Translation[0];
			case EMCPUMGAnimationChannel::TransformTranslationY:
				return &TransformSection->Translation[1];
			case EMCPUMGAnimationChannel::TransformScaleX:
				return &TransformSection->Scale[0];
			case EMCPUMGAnimationChannel::TransformScaleY:
				return &TransformSection->Scale[1];
			case EMCPUMGAnimationChannel::TransformShearX:
				return &TransformSection->Shear[0];
			case EMCPUMGAnimationChannel::TransformShearY:
				return &TransformSection->Shear[1];
			case EMCPUMGAnimationChannel::TransformRotation:
				return &TransformSection->Rotation;
			default:
				return nullptr;
			}
		}
		case EMCPUMGAnimationTrackKind::Color:
		{
			UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track);
			UMovieSceneColorSection* ColorSection = MCPUMGAnim_FindOrAddSection<UMovieSceneColorSection>(
				ColorTrack, bAllowCreateSection, bOutSectionAdded);
			if (ColorSection == nullptr)
			{
				return nullptr;
			}

			switch (PropertySpec.Channel)
			{
			case EMCPUMGAnimationChannel::ColorR:
				return &ColorSection->GetRedChannel();
			case EMCPUMGAnimationChannel::ColorG:
				return &ColorSection->GetGreenChannel();
			case EMCPUMGAnimationChannel::ColorB:
				return &ColorSection->GetBlueChannel();
			case EMCPUMGAnimationChannel::ColorA:
				return &ColorSection->GetAlphaChannel();
			default:
				return nullptr;
			}
		}
		default:
			break;
		}

		return nullptr;
	}
}

bool FMCPToolsUMGAnimationHandler::HandleAnimationList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FArrayProperty* AnimationsArrayProperty = nullptr;
	void* AnimationsArrayPtr = nullptr;
	if (!TryResolveWidgetAnimationArray(WidgetBlueprint, AnimationsArrayProperty, AnimationsArrayPtr))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.UNSUPPORTED");
		Diagnostic.Message = TEXT("Current engine/widget blueprint does not expose animations array via reflection.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FObjectPropertyBase* AnimationObjectProperty = CastField<FObjectPropertyBase>(AnimationsArrayProperty->Inner);
	FScriptArrayHelper ArrayHelper(AnimationsArrayProperty, AnimationsArrayPtr);
	TArray<TSharedPtr<FJsonValue>> Animations;
	Animations.Reserve(ArrayHelper.Num());
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		UObject* AnimationObject = AnimationObjectProperty != nullptr
			? AnimationObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index))
			: nullptr;
		if (AnimationObject == nullptr)
		{
			continue;
		}

		TSharedRef<FJsonObject> AnimationEntry = MakeShared<FJsonObject>();
		AnimationEntry->SetStringField(TEXT("name"), AnimationObject->GetName());
		AnimationEntry->SetStringField(TEXT("object_path"), AnimationObject->GetPathName());
		Animations.Add(MakeShared<FJsonValueObject>(AnimationEntry));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("animations"), Animations);
	OutResult.ResultObject->SetNumberField(TEXT("animation_count"), Animations.Num());
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGAnimationHandler::HandleAnimationCreate(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString AnimationName;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	bool bOverwrite = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("animation_name"), AnimationName);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || AnimationName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and animation_name are required for umg.animation.create.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FArrayProperty* AnimationsArrayProperty = nullptr;
	void* AnimationsArrayPtr = nullptr;
	if (!TryResolveWidgetAnimationArray(WidgetBlueprint, AnimationsArrayProperty, AnimationsArrayPtr))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.UNSUPPORTED");
		Diagnostic.Message = TEXT("Current engine/widget blueprint does not expose animations array via reflection.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FObjectPropertyBase* AnimationObjectProperty = CastField<FObjectPropertyBase>(AnimationsArrayProperty->Inner);
	FScriptArrayHelper ArrayHelper(AnimationsArrayProperty, AnimationsArrayPtr);

	UObject* ExistingAnimation = nullptr;
	int32 ExistingIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		UObject* AnimationObject = AnimationObjectProperty != nullptr
			? AnimationObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index))
			: nullptr;
		if (AnimationObject != nullptr && AnimationObject->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
		{
			ExistingAnimation = AnimationObject;
			ExistingIndex = Index;
			break;
		}
	}

	UObject* ResultAnimation = ExistingAnimation;
	bool bCreated = false;
	bool bGuidEntryAdded = false;
	bool bMovieSceneCreated = false;
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Animation Create")));
		WidgetBlueprint->Modify();

		if (ExistingAnimation == nullptr)
		{
			UWidgetAnimation* NewAnimation = NewObject<UWidgetAnimation>(
				WidgetBlueprint,
				FName(*AnimationName),
				RF_Public | RF_Transactional);
			if (NewAnimation == nullptr)
			{
				Transaction.Cancel();
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
				Diagnostic.Message = TEXT("Failed to allocate UWidgetAnimation object.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			const int32 InsertIndex = ArrayHelper.AddValue();
			if (AnimationObjectProperty != nullptr)
			{
				AnimationObjectProperty->SetObjectPropertyValue(ArrayHelper.GetRawPtr(InsertIndex), NewAnimation);
			}
			ResultAnimation = NewAnimation;
			bCreated = true;
		}
		else if (bOverwrite)
		{
			if (AnimationObjectProperty != nullptr && ExistingIndex >= 0)
			{
				UWidgetAnimation* ReplacementAnimation = NewObject<UWidgetAnimation>(
					WidgetBlueprint,
					FName(*AnimationName),
					RF_Public | RF_Transactional);
				if (ReplacementAnimation == nullptr)
				{
					Transaction.Cancel();
					FMCPDiagnostic Diagnostic;
					Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
					Diagnostic.Message = TEXT("Failed to allocate replacement UWidgetAnimation object.");
					OutResult.Diagnostics.Add(Diagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}

				AnimationObjectProperty->SetObjectPropertyValue(ArrayHelper.GetRawPtr(ExistingIndex), ReplacementAnimation);
				ResultAnimation = ReplacementAnimation;
				bCreated = true;
			}
		}

		if (UWidgetAnimation* ResultWidgetAnimation = Cast<UWidgetAnimation>(ResultAnimation))
		{
			bMovieSceneCreated = EnsureAnimationMovieScene(ResultWidgetAnimation);
		}

		const FName ResultAnimationName = ResultAnimation != nullptr
			? ResultAnimation->GetFName()
			: FName(*AnimationName);
		bGuidEntryAdded = EnsureWidgetVariableGuidEntry(WidgetBlueprint, ResultAnimationName);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("created"), bCreated && !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("reused"), !bCreated && ResultAnimation != nullptr);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("animation_name"), AnimationName);
	OutResult.ResultObject->SetStringField(TEXT("animation_object_path"), ResultAnimation != nullptr ? ResultAnimation->GetPathName() : FString());
	OutResult.ResultObject->SetBoolField(TEXT("guid_entry_added"), bGuidEntryAdded);
	OutResult.ResultObject->SetBoolField(TEXT("movie_scene_created"), bMovieSceneCreated);
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGAnimationHandler::HandleAnimationRemove(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString AnimationName;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("animation_name"), AnimationName);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || AnimationName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and animation_name are required for umg.animation.remove.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FArrayProperty* AnimationsArrayProperty = nullptr;
	void* AnimationsArrayPtr = nullptr;
	if (!TryResolveWidgetAnimationArray(WidgetBlueprint, AnimationsArrayProperty, AnimationsArrayPtr))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.UNSUPPORTED");
		Diagnostic.Message = TEXT("Current engine/widget blueprint does not expose animations array via reflection.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FObjectPropertyBase* AnimationObjectProperty = CastField<FObjectPropertyBase>(AnimationsArrayProperty->Inner);
	FScriptArrayHelper ArrayHelper(AnimationsArrayProperty, AnimationsArrayPtr);

	TArray<int32> RemoveIndices;
	TArray<FString> RemovedObjectPaths;
	TArray<FName> RemovedVariableNames;
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		UObject* AnimationObject = AnimationObjectProperty != nullptr
			? AnimationObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index))
			: nullptr;
		if (AnimationObject == nullptr)
		{
			continue;
		}

		if (AnimationObject->GetName().Equals(AnimationName, ESearchCase::IgnoreCase)
			|| AnimationObject->GetPathName().Equals(AnimationName, ESearchCase::IgnoreCase))
		{
			RemoveIndices.Add(Index);
			RemovedObjectPaths.Add(AnimationObject->GetPathName());
			RemovedVariableNames.AddUnique(AnimationObject->GetFName());
		}
	}

	if (!Request.Context.bDryRun && RemoveIndices.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Animation Remove")));
		WidgetBlueprint->Modify();

		for (const FName VariableName : RemovedVariableNames)
		{
			RemoveWidgetVariableGuidEntry(WidgetBlueprint, VariableName);
		}

		RemoveIndices.Sort();
		for (int32 RemoveIdx = RemoveIndices.Num() - 1; RemoveIdx >= 0; --RemoveIdx)
		{
			ArrayHelper.RemoveValues(RemoveIndices[RemoveIdx], 1);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemoveIndices.Num());
	OutResult.ResultObject->SetArrayField(TEXT("removed"), MCPToolCommonJson::ToJsonStringArray(RemovedObjectPaths));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGAnimationHandler::HandleAnimationTrackAdd(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString AnimationName;
	FString PropertyPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("animation_name"), AnimationName);
		Request.Params->TryGetStringField(TEXT("property_path"), PropertyPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || AnimationName.IsEmpty() || PropertyPath.IsEmpty() || WidgetRef == nullptr || !WidgetRef->IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, animation_name, widget_ref and property_path are required for umg.animation.track.add.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPUMGAnimationPropertySpec PropertySpec;
	FMCPDiagnostic ParseDiagnostic;
	if (!MCPUMGAnim_ParsePropertySpec(PropertyPath, false, PropertySpec, ParseDiagnostic))
	{
		OutResult.Diagnostics.Add(ParseDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path or widget_ref could not be resolved.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetAnimation* Animation = MCPUMGAnim_FindAnimationByNameOrPath(WidgetBlueprint, AnimationName);
	if (Animation == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("animation_name was not found in widget blueprint.");
		Diagnostic.Detail = AnimationName;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (MovieScene == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.NO_MOVIESCENE");
		Diagnostic.Message = TEXT("Target animation has no MovieScene.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bBindingCreated = false;
	bool bTrackAdded = false;
	bool bSectionAdded = false;
	FGuid BindingGuid;
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Animation Track Add")));
		WidgetBlueprint->Modify();
		Animation->Modify();
		MovieScene->Modify();

		FMCPDiagnostic BindingDiagnostic;
		if (!MCPUMGAnim_ResolveBindingGuid(Animation, Widget, true, BindingGuid, bBindingCreated, BindingDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(BindingDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		UMovieSceneTrack* Track = MCPUMGAnim_FindOrAddTrackForSpec(
			MovieScene,
			BindingGuid,
			PropertySpec,
			true,
			bTrackAdded);
		if (Track == nullptr)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.TRACK_CREATE_FAILED");
			Diagnostic.Message = TEXT("Failed to resolve or create MovieScene track.");
			Diagnostic.Detail = PropertySpec.CanonicalPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (PropertySpec.TrackKind == EMCPUMGAnimationTrackKind::Float)
		{
			FMovieSceneFloatChannel* FloatChannel = MCPUMGAnim_ResolveChannelFromTrack(Track, PropertySpec, true, bSectionAdded);
			if (FloatChannel == nullptr)
			{
				Transaction.Cancel();
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.SECTION_CREATE_FAILED");
				Diagnostic.Message = TEXT("Failed to resolve float section/channel for track.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}
		else
		{
			if (PropertySpec.TrackKind == EMCPUMGAnimationTrackKind::Transform2D)
			{
				UMovieScene2DTransformTrack* TransformTrack = Cast<UMovieScene2DTransformTrack>(Track);
				UMovieScene2DTransformSection* Section = MCPUMGAnim_FindOrAddSection<UMovieScene2DTransformSection>(
					TransformTrack, true, bSectionAdded);
				if (Section == nullptr)
				{
					Transaction.Cancel();
					FMCPDiagnostic Diagnostic;
					Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.SECTION_CREATE_FAILED");
					Diagnostic.Message = TEXT("Failed to resolve transform section for track.");
					OutResult.Diagnostics.Add(Diagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}
			}
			else if (PropertySpec.TrackKind == EMCPUMGAnimationTrackKind::Color)
			{
				UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track);
				UMovieSceneColorSection* Section = MCPUMGAnim_FindOrAddSection<UMovieSceneColorSection>(
					ColorTrack, true, bSectionAdded);
				if (Section == nullptr)
				{
					Transaction.Cancel();
					FMCPDiagnostic Diagnostic;
					Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.SECTION_CREATE_FAILED");
					Diagnostic.Message = TEXT("Failed to resolve color section for track.");
					OutResult.Diagnostics.Add(Diagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		FMCPDiagnostic BindingDiagnostic;
		bool bDummyCreated = false;
		MCPUMGAnim_ResolveBindingGuid(Animation, Widget, false, BindingGuid, bDummyCreated, BindingDiagnostic);
		if (BindingGuid.IsValid())
		{
			UMovieSceneTrack* ExistingTrack = MCPUMGAnim_FindOrAddTrackForSpec(
				MovieScene,
				BindingGuid,
				PropertySpec,
				false,
				bTrackAdded);
			bTrackAdded = ExistingTrack == nullptr;
		}
		else
		{
			bBindingCreated = true;
			bTrackAdded = true;
			bSectionAdded = true;
		}
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("binding_created"), bBindingCreated && !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("track_added"), bTrackAdded);
	OutResult.ResultObject->SetBoolField(TEXT("section_added"), bSectionAdded);
	OutResult.ResultObject->SetStringField(TEXT("track_kind"), MCPUMGAnim_TrackKindToString(PropertySpec.TrackKind));
	OutResult.ResultObject->SetStringField(TEXT("property_path"), PropertySpec.CanonicalPath);
	OutResult.ResultObject->SetStringField(TEXT("channel"), MCPUMGAnim_ChannelToString(PropertySpec.Channel));
	OutResult.ResultObject->SetStringField(TEXT("widget_name"), Widget->GetName());
	OutResult.ResultObject->SetStringField(TEXT("animation_object_path"), Animation->GetPathName());
	OutResult.ResultObject->SetStringField(TEXT("binding_guid"), BindingGuid.IsValid() ? BindingGuid.ToString() : FString());
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGAnimationHandler::HandleAnimationKeySet(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString AnimationName;
	FString PropertyPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	double KeyValue = 0.0;
	bool bHasValue = false;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("animation_name"), AnimationName);
		Request.Params->TryGetStringField(TEXT("property_path"), PropertyPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		bHasValue = Request.Params->TryGetNumberField(TEXT("value"), KeyValue);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || AnimationName.IsEmpty() || PropertyPath.IsEmpty() || WidgetRef == nullptr || !WidgetRef->IsValid() || !bHasValue)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, animation_name, widget_ref, property_path and value are required for umg.animation.key.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPUMGAnimationPropertySpec PropertySpec;
	FMCPDiagnostic ParseDiagnostic;
	if (!MCPUMGAnim_ParsePropertySpec(PropertyPath, true, PropertySpec, ParseDiagnostic))
	{
		OutResult.Diagnostics.Add(ParseDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path or widget_ref could not be resolved.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetAnimation* Animation = MCPUMGAnim_FindAnimationByNameOrPath(WidgetBlueprint, AnimationName);
	if (Animation == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("animation_name was not found in widget blueprint.");
		Diagnostic.Detail = AnimationName;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (MovieScene == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.NO_MOVIESCENE");
		Diagnostic.Message = TEXT("Target animation has no MovieScene.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FFrameNumber FrameNumber = 0;
	double TimeSeconds = 0.0;
	FMCPDiagnostic TimeDiagnostic;
	if (!MCPUMGAnim_ParseKeyTime(Request.Params, MovieScene->GetTickResolution(), FrameNumber, TimeSeconds, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	EMovieSceneKeyInterpolation Interpolation = EMovieSceneKeyInterpolation::Auto;
	FMCPDiagnostic InterpolationDiagnostic;
	if (!MCPUMGAnim_ParseInterpolation(Request.Params, Interpolation, InterpolationDiagnostic))
	{
		OutResult.Diagnostics.Add(InterpolationDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bBindingCreated = false;
	bool bTrackAdded = false;
	bool bSectionAdded = false;
	bool bKeySet = false;
	FGuid BindingGuid;
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Animation Key Set")));
		WidgetBlueprint->Modify();
		Animation->Modify();
		MovieScene->Modify();

		FMCPDiagnostic BindingDiagnostic;
		if (!MCPUMGAnim_ResolveBindingGuid(Animation, Widget, true, BindingGuid, bBindingCreated, BindingDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(BindingDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		UMovieSceneTrack* Track = MCPUMGAnim_FindOrAddTrackForSpec(
			MovieScene,
			BindingGuid,
			PropertySpec,
			true,
			bTrackAdded);
		if (Track == nullptr)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.TRACK_CREATE_FAILED");
			Diagnostic.Message = TEXT("Failed to resolve or create MovieScene track.");
			Diagnostic.Detail = PropertySpec.CanonicalPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		FMovieSceneFloatChannel* Channel = MCPUMGAnim_ResolveChannelFromTrack(Track, PropertySpec, true, bSectionAdded);
		if (Channel == nullptr)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.CHANNEL_NOT_FOUND");
			Diagnostic.Message = TEXT("Failed to resolve channel for property_path.");
			Diagnostic.Detail = PropertySpec.CanonicalPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		const int32 AddedKeyIndex = AddFloatKeyWithInterpolation(
			Channel,
			FrameNumber,
			static_cast<float>(KeyValue),
			Interpolation);
		bKeySet = AddedKeyIndex != INDEX_NONE;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		bKeySet = true;
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	FString InterpolationString = TEXT("auto");
	switch (Interpolation)
	{
	case EMovieSceneKeyInterpolation::SmartAuto:
		InterpolationString = TEXT("smart_auto");
		break;
	case EMovieSceneKeyInterpolation::Linear:
		InterpolationString = TEXT("linear");
		break;
	case EMovieSceneKeyInterpolation::Constant:
		InterpolationString = TEXT("constant");
		break;
	case EMovieSceneKeyInterpolation::User:
		InterpolationString = TEXT("user");
		break;
	case EMovieSceneKeyInterpolation::Break:
		InterpolationString = TEXT("break");
		break;
	case EMovieSceneKeyInterpolation::Auto:
	default:
		InterpolationString = TEXT("auto");
		break;
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("binding_created"), bBindingCreated && !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("track_added"), bTrackAdded && !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("section_added"), bSectionAdded && !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("key_set"), bKeySet);
	OutResult.ResultObject->SetNumberField(TEXT("frame"), FrameNumber.Value);
	OutResult.ResultObject->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutResult.ResultObject->SetNumberField(TEXT("value"), KeyValue);
	OutResult.ResultObject->SetStringField(TEXT("interpolation"), InterpolationString);
	OutResult.ResultObject->SetStringField(TEXT("track_kind"), MCPUMGAnim_TrackKindToString(PropertySpec.TrackKind));
	OutResult.ResultObject->SetStringField(TEXT("property_path"), PropertySpec.CanonicalPath);
	OutResult.ResultObject->SetStringField(TEXT("channel"), MCPUMGAnim_ChannelToString(PropertySpec.Channel));
	OutResult.ResultObject->SetStringField(TEXT("widget_name"), Widget->GetName());
	OutResult.ResultObject->SetStringField(TEXT("animation_object_path"), Animation->GetPathName());
	OutResult.ResultObject->SetStringField(TEXT("binding_guid"), BindingGuid.IsValid() ? BindingGuid.ToString() : FString());
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGAnimationHandler::HandleAnimationKeyRemove(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString AnimationName;
	FString PropertyPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("animation_name"), AnimationName);
		Request.Params->TryGetStringField(TEXT("property_path"), PropertyPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || AnimationName.IsEmpty() || PropertyPath.IsEmpty() || WidgetRef == nullptr || !WidgetRef->IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, animation_name, widget_ref and property_path are required for umg.animation.key.remove.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPUMGAnimationPropertySpec PropertySpec;
	FMCPDiagnostic ParseDiagnostic;
	if (!MCPUMGAnim_ParsePropertySpec(PropertyPath, true, PropertySpec, ParseDiagnostic))
	{
		OutResult.Diagnostics.Add(ParseDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path or widget_ref could not be resolved.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetAnimation* Animation = MCPUMGAnim_FindAnimationByNameOrPath(WidgetBlueprint, AnimationName);
	if (Animation == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("animation_name was not found in widget blueprint.");
		Diagnostic.Detail = AnimationName;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (MovieScene == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.ANIMATION.NO_MOVIESCENE");
		Diagnostic.Message = TEXT("Target animation has no MovieScene.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FFrameNumber FrameNumber = 0;
	double TimeSeconds = 0.0;
	FMCPDiagnostic TimeDiagnostic;
	if (!MCPUMGAnim_ParseKeyTime(Request.Params, MovieScene->GetTickResolution(), FrameNumber, TimeSeconds, TimeDiagnostic))
	{
		OutResult.Diagnostics.Add(TimeDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	int32 RemovedCount = 0;
	bool bTrackFound = false;
	bool bSectionFound = false;
	FGuid BindingGuid;
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Animation Key Remove")));
		WidgetBlueprint->Modify();
		Animation->Modify();
		MovieScene->Modify();

		FMCPDiagnostic BindingDiagnostic;
		bool bBindingCreated = false;
		if (!MCPUMGAnim_ResolveBindingGuid(Animation, Widget, false, BindingGuid, bBindingCreated, BindingDiagnostic))
		{
			Transaction.Cancel();
			if (!BindingDiagnostic.Code.Equals(TEXT("MCP.UMG.ANIMATION.BINDING_NOT_FOUND"), ESearchCase::IgnoreCase))
			{
				OutResult.Diagnostics.Add(BindingDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}
		else
		{
			bool bTrackAdded = false;
			UMovieSceneTrack* Track = MCPUMGAnim_FindOrAddTrackForSpec(
				MovieScene,
				BindingGuid,
				PropertySpec,
				false,
				bTrackAdded);
			bTrackFound = Track != nullptr;
			if (Track != nullptr)
			{
				bool bSectionAdded = false;
				FMovieSceneFloatChannel* Channel = MCPUMGAnim_ResolveChannelFromTrack(Track, PropertySpec, false, bSectionAdded);
				bSectionFound = Channel != nullptr;
				if (Channel != nullptr)
				{
					auto ChannelData = Channel->GetData();
					int32 KeyIndex = ChannelData.FindKey(FrameNumber, 0);
					while (KeyIndex != INDEX_NONE)
					{
						ChannelData.RemoveKey(KeyIndex);
						++RemovedCount;
						KeyIndex = ChannelData.FindKey(FrameNumber, 0);
					}
				}
			}
		}

		if (RemovedCount > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
			WidgetBlueprint->MarkPackageDirty();
		}
		else
		{
			Transaction.Cancel();
		}
	}
	else
	{
		RemovedCount = 0;
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	const bool bShouldCompile = bCompileOnSuccess && !Request.Context.bDryRun && RemovedCount > 0;
	if (bShouldCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave && RemovedCount > 0)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemovedCount);
	OutResult.ResultObject->SetBoolField(TEXT("track_found"), bTrackFound || Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("section_found"), bSectionFound || Request.Context.bDryRun);
	OutResult.ResultObject->SetNumberField(TEXT("frame"), FrameNumber.Value);
	OutResult.ResultObject->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutResult.ResultObject->SetStringField(TEXT("track_kind"), MCPUMGAnim_TrackKindToString(PropertySpec.TrackKind));
	OutResult.ResultObject->SetStringField(TEXT("property_path"), PropertySpec.CanonicalPath);
	OutResult.ResultObject->SetStringField(TEXT("channel"), MCPUMGAnim_ChannelToString(PropertySpec.Channel));
	OutResult.ResultObject->SetStringField(TEXT("widget_name"), Widget->GetName());
	OutResult.ResultObject->SetStringField(TEXT("animation_object_path"), Animation->GetPathName());
	OutResult.ResultObject->SetStringField(TEXT("binding_guid"), BindingGuid.IsValid() ? BindingGuid.ToString() : FString());
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}
