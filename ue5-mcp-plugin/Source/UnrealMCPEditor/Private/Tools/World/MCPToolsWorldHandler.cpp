#include "Tools/World/MCPToolsWorldHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
	UWorld* GetEditorWorld()
	{
		if (GEditor == nullptr)
		{
			return nullptr;
		}
		return GEditor->GetEditorWorldContext().World();
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
}

bool FMCPToolsWorldHandler::HandleOutlinerList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	UWorld* World = GetEditorWorld();
	if (World == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Editor world is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	int32 Limit = 200;
	const int32 Cursor = MCPToolCommonJson::ParseCursor(Request.Params);
	bool bIncludeClassPath = true;
	bool bIncludeFolderPath = true;
	bool bIncludeTags = false;
	bool bIncludeTransform = false;
	FString NameGlob;
	TSet<FString> AllowedClassPaths;

	if (Request.Params.IsValid())
	{
		double LimitNumber = static_cast<double>(Limit);
		Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 2000);

		const TSharedPtr<FJsonObject>* IncludeObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("include"), IncludeObject) && IncludeObject != nullptr && IncludeObject->IsValid())
		{
			(*IncludeObject)->TryGetBoolField(TEXT("class"), bIncludeClassPath);
			(*IncludeObject)->TryGetBoolField(TEXT("folder_path"), bIncludeFolderPath);
			(*IncludeObject)->TryGetBoolField(TEXT("tags"), bIncludeTags);
			(*IncludeObject)->TryGetBoolField(TEXT("transform"), bIncludeTransform);
		}

		const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject) && FiltersObject != nullptr && FiltersObject->IsValid())
		{
			(*FiltersObject)->TryGetStringField(TEXT("name_glob"), NameGlob);
			const TArray<TSharedPtr<FJsonValue>>* ClassPathArray = nullptr;
			if ((*FiltersObject)->TryGetArrayField(TEXT("class_path_in"), ClassPathArray) && ClassPathArray != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& ClassPathValue : *ClassPathArray)
				{
					FString ClassPath;
					if (ClassPathValue.IsValid() && ClassPathValue->TryGetString(ClassPath) && !ClassPath.IsEmpty())
					{
						AllowedClassPaths.Add(ClassPath);
					}
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonObject>> AllNodes;
	TSet<FString> FolderPaths;
	USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == nullptr)
		{
			continue;
		}

		const FString ActorLabel = Actor->GetActorLabel();
		if (!NameGlob.IsEmpty() && !ActorLabel.MatchesWildcard(NameGlob))
		{
			continue;
		}

		const FString ClassPath = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
		if (AllowedClassPaths.Num() > 0 && !AllowedClassPaths.Contains(ClassPath))
		{
			continue;
		}

		const FString FolderPath = Actor->GetFolderPath().ToString();
		if (bIncludeFolderPath && !FolderPath.IsEmpty())
		{
			FolderPaths.Add(FolderPath);
		}

		TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		NodeObject->SetStringField(TEXT("node_type"), TEXT("actor"));
		NodeObject->SetStringField(TEXT("id"), Actor->GetActorGuid().IsValid() ? Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphens) : MCPObjectUtils::BuildActorPath(Actor));
		NodeObject->SetStringField(TEXT("name"), ActorLabel);
		NodeObject->SetStringField(TEXT("actor_path"), MCPObjectUtils::BuildActorPath(Actor));
		NodeObject->SetStringField(TEXT("folder_path"), FolderPath);
		NodeObject->SetStringField(TEXT("parent_id"), FolderPath.IsEmpty() ? TEXT("") : FString::Printf(TEXT("folder:%s"), *FolderPath));
		NodeObject->SetStringField(TEXT("class_path"), bIncludeClassPath ? ClassPath : TEXT(""));
		NodeObject->SetBoolField(TEXT("is_selected"), Selection != nullptr && Selection->IsSelected(Actor));
		NodeObject->SetBoolField(TEXT("is_hidden_in_editor"), Actor->IsHiddenEd());

		if (bIncludeTags)
		{
			TArray<FString> TagStrings;
			TagStrings.Reserve(Actor->Tags.Num());
			for (const FName& Tag : Actor->Tags)
			{
				TagStrings.Add(Tag.ToString());
			}
			NodeObject->SetArrayField(TEXT("tags"), MCPToolCommonJson::ToJsonStringArray(TagStrings));
		}

		if (bIncludeTransform)
		{
			const FTransform Transform = Actor->GetActorTransform();
			TSharedRef<FJsonObject> TransformObject = MakeShared<FJsonObject>();
			TransformObject->SetStringField(TEXT("location"), Transform.GetLocation().ToCompactString());
			TransformObject->SetStringField(TEXT("rotation"), Transform.GetRotation().Rotator().ToCompactString());
			TransformObject->SetStringField(TEXT("scale"), Transform.GetScale3D().ToCompactString());
			NodeObject->SetObjectField(TEXT("transform"), TransformObject);
		}

		AllNodes.Add(NodeObject);
	}

	if (bIncludeFolderPath)
	{
		TArray<FString> SortedFolders = FolderPaths.Array();
		SortedFolders.Sort();
		for (const FString& FolderPath : SortedFolders)
		{
			TSharedRef<FJsonObject> FolderNode = MakeShared<FJsonObject>();
			FolderNode->SetStringField(TEXT("node_type"), TEXT("folder"));
			FolderNode->SetStringField(TEXT("id"), FString::Printf(TEXT("folder:%s"), *FolderPath));
			FolderNode->SetStringField(TEXT("folder_path"), FolderPath);

			FString ParentFolderPath;
			FString FolderName = FolderPath;
			int32 SeparatorIndex = INDEX_NONE;
			if (FolderPath.FindLastChar(TEXT('/'), SeparatorIndex))
			{
				FolderName = FolderPath.Mid(SeparatorIndex + 1);
				ParentFolderPath = FolderPath.Left(SeparatorIndex);
			}

			FolderNode->SetStringField(TEXT("name"), FolderName);
			FolderNode->SetStringField(TEXT("parent_id"), ParentFolderPath.IsEmpty() ? TEXT("") : FString::Printf(TEXT("folder:%s"), *ParentFolderPath));
			AllNodes.Add(FolderNode);
		}
	}

	AllNodes.Sort([](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	{
		FString LeftType;
		FString RightType;
		Left->TryGetStringField(TEXT("node_type"), LeftType);
		Right->TryGetStringField(TEXT("node_type"), RightType);
		if (LeftType != RightType)
		{
			return LeftType < RightType;
		}

		FString LeftName;
		FString RightName;
		Left->TryGetStringField(TEXT("name"), LeftName);
		Right->TryGetStringField(TEXT("name"), RightName);
		return LeftName < RightName;
	});

	const int32 SafeCursor = FMath::Max(0, Cursor);
	const int32 EndIndex = FMath::Min(SafeCursor + Limit, AllNodes.Num());
	TArray<TSharedPtr<FJsonValue>> OutputNodes;
	for (int32 Index = SafeCursor; Index < EndIndex; ++Index)
	{
		OutputNodes.Add(MakeShared<FJsonValueObject>(AllNodes[Index].ToSharedRef()));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("nodes"), OutputNodes);
	if (EndIndex < AllNodes.Num())
	{
		OutResult.ResultObject->SetStringField(TEXT("next_cursor"), FString::FromInt(EndIndex));
	}
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsWorldHandler::HandleSelectionGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	(void)Request;

	USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
	TArray<FString> SelectedActorPaths;
	if (Selection != nullptr)
	{
		for (FSelectionIterator It(*Selection); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (Actor != nullptr)
			{
				SelectedActorPaths.Add(MCPObjectUtils::BuildActorPath(Actor));
			}
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("actors"), MCPToolCommonJson::ToJsonStringArray(SelectedActorPaths));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsWorldHandler::HandleSelectionSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	UWorld* World = GetEditorWorld();
	if (World == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Editor world is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString Mode = TEXT("replace");
	const TArray<TSharedPtr<FJsonValue>>* ActorValues = nullptr;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetArrayField(TEXT("actors"), ActorValues);
	}

	if (ActorValues == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("actors array is required for world.selection.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
	{
		GEditor->SelectNone(false, true, false);
	}

	int32 MissingActorCount = 0;
	for (const TSharedPtr<FJsonValue>& ActorValue : *ActorValues)
	{
		FString ActorReference;
		if (!ActorValue.IsValid() || !ActorValue->TryGetString(ActorReference) || ActorReference.IsEmpty())
		{
			continue;
		}

		AActor* Actor = FindActorByReference(World, ActorReference);
		if (Actor == nullptr)
		{
			++MissingActorCount;
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Actor reference could not be resolved.");
			Diagnostic.Detail = ActorReference;
			Diagnostic.bRetriable = true;
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		if (Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
		{
			GEditor->SelectActor(Actor, false, false, true);
		}
		else
		{
			GEditor->SelectActor(Actor, true, false, true);
		}
	}

	GEditor->NoteSelectionChange();

	USelection* Selection = GEditor->GetSelectedActors();
	TArray<FString> SelectedActorPaths;
	for (FSelectionIterator It(*Selection); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (Actor != nullptr)
		{
			SelectedActorPaths.Add(MCPObjectUtils::BuildActorPath(Actor));
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("selected"), MCPToolCommonJson::ToJsonStringArray(SelectedActorPaths));
	OutResult.Status = (MissingActorCount > 0 && SelectedActorPaths.Num() > 0) ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	if (MissingActorCount > 0 && SelectedActorPaths.Num() == 0)
	{
		OutResult.Status = EMCPResponseStatus::Error;
	}

	return OutResult.Status != EMCPResponseStatus::Error;
}
