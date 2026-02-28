#include "Tools/UMG/MCPToolsUMGReadHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ContentWidget.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "UObject/UObjectIterator.h"

namespace
{
	UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
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

	bool TryReadStructFieldAsString(
		const UStruct* OwnerStruct,
		const void* OwnerData,
		const TArray<FString>& CandidateNames,
		FString& OutValue)
	{
		OutValue.Reset();
		if (OwnerStruct == nullptr || OwnerData == nullptr)
		{
			return false;
		}

		for (const FString& CandidateName : CandidateNames)
		{
			FProperty* Property = OwnerStruct->FindPropertyByName(FName(*CandidateName));
			if (Property == nullptr)
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(OwnerData);
			if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				OutValue = NameProperty->GetPropertyValue(ValuePtr).ToString();
				return true;
			}

			if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
			{
				OutValue = StringProperty->GetPropertyValue(ValuePtr);
				return true;
			}

			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				OutValue = TextProperty->GetPropertyValue(ValuePtr).ToString();
				return true;
			}
		}

		return false;
	}

	bool TryGetWidgetBlueprintBindingArray(UWidgetBlueprint* WidgetBlueprint, FArrayProperty*& OutArrayProperty, void*& OutArrayPtr)
	{
		OutArrayProperty = nullptr;
		OutArrayPtr = nullptr;
		if (WidgetBlueprint == nullptr)
		{
			return false;
		}

		OutArrayProperty = FindFProperty<FArrayProperty>(WidgetBlueprint->GetClass(), TEXT("Bindings"));
		if (OutArrayProperty == nullptr || !OutArrayProperty->Inner->IsA<FStructProperty>())
		{
			return false;
		}

		OutArrayPtr = OutArrayProperty->ContainerPtrToValuePtr<void>(WidgetBlueprint);
		return OutArrayPtr != nullptr;
	}

	FString GetWidgetStableId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr)
		{
			return TEXT("");
		}

		return FString::Printf(TEXT("name:%s"), *Widget->GetName());
	}

	bool TryExportFieldAsText(const UStruct* OwnerStruct, const void* OwnerData, const FString& FieldName, FString& OutText)
	{
		if (OwnerStruct == nullptr || OwnerData == nullptr || FieldName.IsEmpty())
		{
			return false;
		}

		FProperty* Property = OwnerStruct->FindPropertyByName(FName(*FieldName));
		if (Property == nullptr)
		{
			return false;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(OwnerData);
		Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, nullptr, PPF_None);
		return true;
	}

	bool TryExportObjectPropertyAsText(UObject* TargetObject, const FString& PropertyName, FString& OutText)
	{
		return TargetObject != nullptr
			&& TryExportFieldAsText(TargetObject->GetClass(), TargetObject, PropertyName, OutText);
	}

	TSharedRef<FJsonObject> BuildSlotSummaryObject(UPanelSlot* Slot)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		if (Slot == nullptr)
		{
			return Summary;
		}

		Summary->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetPathName());

		const TArray<FString> CandidateFields{
			TEXT("LayoutData"),
			TEXT("Offsets"),
			TEXT("Anchors"),
			TEXT("Alignment"),
			TEXT("Padding"),
			TEXT("HorizontalAlignment"),
			TEXT("VerticalAlignment"),
			TEXT("ZOrder")
		};

		for (const FString& CandidateField : CandidateFields)
		{
			FString ExportedText;
			if (TryExportObjectPropertyAsText(Slot, CandidateField, ExportedText))
			{
				Summary->SetStringField(CandidateField, ExportedText);
			}
		}

		return Summary;
	}

	TSharedRef<FJsonObject> BuildWidgetLayoutSummaryObject(UWidget* Widget)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		if (Widget == nullptr)
		{
			return Summary;
		}

		Summary->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetPathName());

		FString ExportedText;
		if (TryExportObjectPropertyAsText(Widget, TEXT("RenderTransform"), ExportedText))
		{
			Summary->SetStringField(TEXT("RenderTransform"), ExportedText);
		}
		if (TryExportObjectPropertyAsText(Widget, TEXT("RenderOpacity"), ExportedText))
		{
			Summary->SetStringField(TEXT("RenderOpacity"), ExportedText);
		}
		if (TryExportObjectPropertyAsText(Widget, TEXT("Visibility"), ExportedText))
		{
			Summary->SetStringField(TEXT("Visibility"), ExportedText);
		}

		if (Widget->Slot != nullptr)
		{
			Summary->SetObjectField(TEXT("slot"), BuildSlotSummaryObject(Widget->Slot));
		}

		return Summary;
	}

	FString GetWidgetSlotType(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (Widget == nullptr)
		{
			return FString();
		}

		if (Widget->Slot != nullptr)
		{
			return Widget->Slot->GetClass()->GetName();
		}

		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			return FString();
		}

		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* CandidateHost : AllWidgets)
		{
			if (CandidateHost == nullptr || CandidateHost == Widget)
			{
				continue;
			}

			INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(CandidateHost);
			if (NamedSlotHost == nullptr)
			{
				continue;
			}

			TArray<FName> SlotNames;
			NamedSlotHost->GetSlotNames(SlotNames);
			for (const FName SlotName : SlotNames)
			{
				if (SlotName.IsNone())
				{
					continue;
				}

				if (NamedSlotHost->GetContentForSlot(SlotName) == Widget)
				{
					return FString::Printf(TEXT("NamedSlot:%s"), *SlotName.ToString());
				}
			}
		}

		TArray<FName> TreeSlotNames;
		WidgetBlueprint->WidgetTree->GetSlotNames(TreeSlotNames);
		for (const FName SlotName : TreeSlotNames)
		{
			if (SlotName.IsNone())
			{
				continue;
			}

			if (WidgetBlueprint->WidgetTree->GetContentForSlot(SlotName) == Widget)
			{
				return FString::Printf(TEXT("NamedSlot:%s"), *SlotName.ToString());
			}
		}

		return FString();
	}

	void BuildWidgetTreeNodesRecursive(
		const UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		const FString& ParentId,
		const FString& SlotTypeOverride,
		const FString& NameGlob,
		const TSet<FString>& AllowedClassPaths,
		const int32 MaxDepth,
		const int32 Depth,
		const bool bIncludeSlotSummary,
		const bool bIncludeLayoutSummary,
		TArray<TSharedPtr<FJsonValue>>& OutNodes)
	{
		if (Widget == nullptr || Depth > MaxDepth)
		{
			return;
		}

		const FString WidgetId = GetWidgetStableId(WidgetBlueprint, Widget);
		const FString ClassPath = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
		const bool bPassNameFilter = NameGlob.IsEmpty() || Widget->GetName().MatchesWildcard(NameGlob);
		const bool bPassClassFilter = AllowedClassPaths.Num() == 0 || AllowedClassPaths.Contains(ClassPath);

		if (bPassNameFilter && bPassClassFilter)
		{
			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("widget_id"), WidgetId);
			NodeObject->SetStringField(TEXT("name"), Widget->GetName());
			NodeObject->SetStringField(TEXT("class_path"), ClassPath);
			NodeObject->SetStringField(TEXT("parent_id"), ParentId);
			const FString SlotType = !SlotTypeOverride.IsEmpty()
				? SlotTypeOverride
				: GetWidgetSlotType(WidgetBlueprint, Widget);
			NodeObject->SetStringField(TEXT("slot_type"), SlotType);

			int32 ChildrenCount = 0;
			if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				ChildrenCount += Panel->GetChildrenCount();
			}
			if (const INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(Widget))
			{
				TArray<FName> SlotNames;
				NamedSlotHost->GetSlotNames(SlotNames);
				for (const FName SlotName : SlotNames)
				{
					if (SlotName.IsNone())
					{
						continue;
					}
					if (NamedSlotHost->GetContentForSlot(SlotName) != nullptr)
					{
						++ChildrenCount;
					}
				}
			}
			NodeObject->SetNumberField(TEXT("children_count"), ChildrenCount);
			NodeObject->SetObjectField(TEXT("flags"), MakeShared<FJsonObject>());
			if (bIncludeSlotSummary && Widget->Slot != nullptr)
			{
				NodeObject->SetObjectField(TEXT("slot_summary"), BuildSlotSummaryObject(Widget->Slot));
			}
			if (bIncludeLayoutSummary)
			{
				NodeObject->SetObjectField(TEXT("layout_summary"), BuildWidgetLayoutSummaryObject(Widget));
			}
			OutNodes.Add(MakeShared<FJsonValueObject>(NodeObject));
		}

		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				UWidget* ChildWidget = Panel->GetChildAt(Index);
				const FString ChildSlotType = (ChildWidget != nullptr && ChildWidget->Slot != nullptr)
					? ChildWidget->Slot->GetClass()->GetName()
					: FString();
				BuildWidgetTreeNodesRecursive(
					WidgetBlueprint,
					ChildWidget,
					WidgetId,
					ChildSlotType,
					NameGlob,
					AllowedClassPaths,
					MaxDepth,
					Depth + 1,
					bIncludeSlotSummary,
					bIncludeLayoutSummary,
					OutNodes);
			}
		}

		if (const INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(Widget))
		{
			TSet<const UWidget*> VisitedNamedSlotChildren;
			TArray<FName> SlotNames;
			NamedSlotHost->GetSlotNames(SlotNames);
			for (const FName SlotName : SlotNames)
			{
				if (SlotName.IsNone())
				{
					continue;
				}

				UWidget* SlotContent = NamedSlotHost->GetContentForSlot(SlotName);
				if (SlotContent == nullptr || VisitedNamedSlotChildren.Contains(SlotContent))
				{
					continue;
				}

				VisitedNamedSlotChildren.Add(SlotContent);
				const FString NamedSlotType = FString::Printf(TEXT("NamedSlot:%s"), *SlotName.ToString());
				BuildWidgetTreeNodesRecursive(
					WidgetBlueprint,
					SlotContent,
					WidgetId,
					NamedSlotType,
					NameGlob,
					AllowedClassPaths,
					MaxDepth,
					Depth + 1,
					bIncludeSlotSummary,
					bIncludeLayoutSummary,
					OutNodes);
			}
		}
	}
}

bool FMCPToolsUMGReadHandler::HandleWidgetClassList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ClassPathGlob = TEXT("/Script/UMG.*");
	FString NameGlob;
	bool bIncludeAbstract = false;
	bool bIncludeDeprecated = false;
	bool bIncludeEditorOnly = false;
	int32 Limit = 200;
	int32 Cursor = 0;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("class_path_glob"), ClassPathGlob);
		Request.Params->TryGetStringField(TEXT("name_glob"), NameGlob);
		Request.Params->TryGetBoolField(TEXT("include_abstract"), bIncludeAbstract);
		Request.Params->TryGetBoolField(TEXT("include_deprecated"), bIncludeDeprecated);
		Request.Params->TryGetBoolField(TEXT("include_editor_only"), bIncludeEditorOnly);

		double LimitNumber = static_cast<double>(Limit);
		Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 2000);

		double CursorNumber = static_cast<double>(Cursor);
		Request.Params->TryGetNumberField(TEXT("cursor"), CursorNumber);
		Cursor = FMath::Max(0, static_cast<int32>(CursorNumber));
	}

	struct FWidgetClassEntry
	{
		FString ClassPath;
		FString ClassName;
		FString ModulePath;
		bool bIsAbstract = false;
		bool bIsDeprecated = false;
		bool bIsEditorOnly = false;
	};

	TArray<FWidgetClassEntry> Entries;
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* WidgetClass = *ClassIt;
		if (WidgetClass == nullptr || !WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}

		const FString ClassName = WidgetClass->GetName();
		if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_")))
		{
			continue;
		}

		const bool bIsAbstract = WidgetClass->HasAnyClassFlags(CLASS_Abstract);
		const bool bIsDeprecated = WidgetClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists);
		const bool bIsEditorOnly = WidgetClass->IsEditorOnly();

		if (!bIncludeAbstract && bIsAbstract)
		{
			continue;
		}
		if (!bIncludeDeprecated && bIsDeprecated)
		{
			continue;
		}
		if (!bIncludeEditorOnly && bIsEditorOnly)
		{
			continue;
		}

		const FString ClassPath = WidgetClass->GetClassPathName().ToString();
		if (!ClassPathGlob.IsEmpty() && !ClassPath.MatchesWildcard(ClassPathGlob))
		{
			continue;
		}
		if (!NameGlob.IsEmpty() && !ClassName.MatchesWildcard(NameGlob))
		{
			continue;
		}

		FWidgetClassEntry Entry;
		Entry.ClassPath = ClassPath;
		Entry.ClassName = ClassName;
		Entry.ModulePath = WidgetClass->GetClassPathName().GetPackageName().ToString();
		Entry.bIsAbstract = bIsAbstract;
		Entry.bIsDeprecated = bIsDeprecated;
		Entry.bIsEditorOnly = bIsEditorOnly;
		Entries.Add(MoveTemp(Entry));
	}

	Entries.Sort([](const FWidgetClassEntry& Left, const FWidgetClassEntry& Right)
	{
		return Left.ClassPath < Right.ClassPath;
	});

	const int32 SafeCursor = FMath::Clamp(Cursor, 0, Entries.Num());
	const int32 EndIndex = FMath::Min(SafeCursor + Limit, Entries.Num());
	TArray<TSharedPtr<FJsonValue>> ClassValues;
	ClassValues.Reserve(EndIndex - SafeCursor);
	for (int32 Index = SafeCursor; Index < EndIndex; ++Index)
	{
		const FWidgetClassEntry& Entry = Entries[Index];
		TSharedRef<FJsonObject> ClassObject = MakeShared<FJsonObject>();
		ClassObject->SetStringField(TEXT("class_path"), Entry.ClassPath);
		ClassObject->SetStringField(TEXT("class_name"), Entry.ClassName);
		ClassObject->SetStringField(TEXT("module_path"), Entry.ModulePath);
		ClassObject->SetBoolField(TEXT("is_abstract"), Entry.bIsAbstract);
		ClassObject->SetBoolField(TEXT("is_deprecated"), Entry.bIsDeprecated);
		ClassObject->SetBoolField(TEXT("is_editor_only"), Entry.bIsEditorOnly);
		ClassValues.Add(MakeShared<FJsonValueObject>(ClassObject));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("classes"), ClassValues);
	OutResult.ResultObject->SetNumberField(TEXT("total_count"), Entries.Num());
	if (EndIndex < Entries.Num())
	{
		OutResult.ResultObject->SetStringField(TEXT("next_cursor"), FString::FromInt(EndIndex));
	}
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGReadHandler::HandleTreeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	int32 Depth = 10;
	FString NameGlob;
	TSet<FString> AllowedClassPaths;
	bool bIncludeSlotSummary = true;
	bool bIncludeLayoutSummary = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 20);

		const TSharedPtr<FJsonObject>* IncludeObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("include"), IncludeObject) && IncludeObject != nullptr && IncludeObject->IsValid())
		{
			(*IncludeObject)->TryGetBoolField(TEXT("slot_summary"), bIncludeSlotSummary);
			(*IncludeObject)->TryGetBoolField(TEXT("layout_summary"), bIncludeLayoutSummary);
		}

		const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject) && FiltersObject != nullptr && FiltersObject->IsValid())
		{
			(*FiltersObject)->TryGetStringField(TEXT("name_glob"), NameGlob);
			const TArray<TSharedPtr<FJsonValue>>* ClassPaths = nullptr;
			if ((*FiltersObject)->TryGetArrayField(TEXT("class_path_in"), ClassPaths) && ClassPaths != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& ClassPathValue : *ClassPaths)
				{
					FString ClassPath;
					if (ClassPathValue.IsValid() && ClassPathValue->TryGetString(ClassPath))
					{
						AllowedClassPaths.Add(ClassPath);
					}
				}
			}
		}
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	UWidget* RootWidget = WidgetBlueprint->WidgetTree->RootWidget;
	if (RootWidget != nullptr)
	{
		BuildWidgetTreeNodesRecursive(
			WidgetBlueprint,
			RootWidget,
			TEXT(""),
			TEXT(""),
			NameGlob,
			AllowedClassPaths,
			Depth,
			0,
			bIncludeSlotSummary,
			bIncludeLayoutSummary,
			Nodes);
	}

	TArray<FName> TopLevelSlotNames;
	WidgetBlueprint->WidgetTree->GetSlotNames(TopLevelSlotNames);
	TSet<UWidget*> TopLevelVisited;
	for (const FName SlotName : TopLevelSlotNames)
	{
		if (SlotName.IsNone())
		{
			continue;
		}

		UWidget* SlotContent = WidgetBlueprint->WidgetTree->GetContentForSlot(SlotName);
		if (SlotContent == nullptr || TopLevelVisited.Contains(SlotContent))
		{
			continue;
		}

		bool bReachableFromRoot = false;
		if (RootWidget != nullptr)
		{
			UWidgetTree::ForEachWidgetAndChildrenUntil(RootWidget, [&bReachableFromRoot, SlotContent](UWidget* CandidateWidget)
			{
				if (CandidateWidget == SlotContent)
				{
					bReachableFromRoot = true;
					return false;
				}

				return true;
			});
		}

		if (bReachableFromRoot)
		{
			continue;
		}

		TopLevelVisited.Add(SlotContent);
		const FString SlotType = FString::Printf(TEXT("NamedSlot:%s"), *SlotName.ToString());
		BuildWidgetTreeNodesRecursive(
			WidgetBlueprint,
			SlotContent,
			TEXT("this"),
			SlotType,
			NameGlob,
			AllowedClassPaths,
			Depth,
			0,
			bIncludeSlotSummary,
			bIncludeLayoutSummary,
			Nodes);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (RootWidget == nullptr)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("WidgetTree has no root widget.")));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("root_id"), RootWidget ? GetWidgetStableId(WidgetBlueprint, RootWidget) : TEXT(""));
	OutResult.ResultObject->SetArrayField(TEXT("nodes"), Nodes);
	OutResult.ResultObject->SetArrayField(TEXT("warnings"), Warnings);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGReadHandler::HandleWidgetInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	int32 Depth = 2;
	bool bIncludeProperties = true;
	bool bIncludeMetadata = true;
	bool bIncludeStyle = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);

		const TSharedPtr<FJsonObject>* IncludeObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("include"), IncludeObject) && IncludeObject != nullptr && IncludeObject->IsValid())
		{
			(*IncludeObject)->TryGetBoolField(TEXT("properties"), bIncludeProperties);
			(*IncludeObject)->TryGetBoolField(TEXT("metadata"), bIncludeMetadata);
			(*IncludeObject)->TryGetBoolField(TEXT("style"), bIncludeStyle);
		}
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("UMG widget could not be resolved.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), GetWidgetStableId(WidgetBlueprint, Widget));
	WidgetObject->SetStringField(TEXT("name"), Widget->GetName());
	WidgetObject->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);

	TArray<TSharedPtr<FJsonValue>> Properties;
	if (bIncludeProperties)
	{
		MCPObjectUtils::InspectObject(Widget, nullptr, Depth, Properties);
	}
	OutResult.ResultObject->SetArrayField(TEXT("properties"), Properties);

	if (bIncludeMetadata)
	{
		TSharedRef<FJsonObject> MetadataObject = MakeShared<FJsonObject>();
		UClass* WidgetClass = Widget->GetClass();
		MetadataObject->SetStringField(TEXT("class_name"), WidgetClass->GetName());
		MetadataObject->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
		MetadataObject->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

		const FString DisplayName = WidgetClass->GetMetaData(TEXT("DisplayName"));
		if (!DisplayName.IsEmpty())
		{
			MetadataObject->SetStringField(TEXT("display_name"), DisplayName);
		}
		const FString ToolTip = WidgetClass->GetMetaData(TEXT("ToolTip"));
		if (!ToolTip.IsEmpty())
		{
			MetadataObject->SetStringField(TEXT("tooltip"), ToolTip);
		}

		OutResult.ResultObject->SetObjectField(TEXT("metadata"), MetadataObject);
	}

	if (bIncludeStyle)
	{
		TSharedRef<FJsonObject> StyleFilters = MakeShared<FJsonObject>();
		StyleFilters->SetBoolField(TEXT("only_editable"), false);
		StyleFilters->SetStringField(TEXT("property_name_glob"), TEXT("*Style*"));
		TArray<TSharedPtr<FJsonValue>> StyleProperties;
		MCPObjectUtils::InspectObject(Widget, StyleFilters, Depth, StyleProperties);
		OutResult.ResultObject->SetArrayField(TEXT("style"), StyleProperties);
	}

	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGReadHandler::HandleSlotInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	int32 Depth = 2;
	bool bIncludeLayout = true;
	bool bIncludeAnchors = true;
	bool bIncludePadding = true;
	bool bIncludeAlignment = true;
	bool bIncludeZOrder = true;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);

		const TSharedPtr<FJsonObject>* IncludeObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("include"), IncludeObject) && IncludeObject != nullptr && IncludeObject->IsValid())
		{
			(*IncludeObject)->TryGetBoolField(TEXT("layout"), bIncludeLayout);
			(*IncludeObject)->TryGetBoolField(TEXT("anchors"), bIncludeAnchors);
			(*IncludeObject)->TryGetBoolField(TEXT("padding"), bIncludePadding);
			(*IncludeObject)->TryGetBoolField(TEXT("alignment"), bIncludeAlignment);
			(*IncludeObject)->TryGetBoolField(TEXT("zorder"), bIncludeZOrder);
		}
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("UMG widget could not be resolved.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (Widget->Slot == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("Resolved widget does not have a slot.");
		Diagnostic.Detail = Widget->GetName();
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> SlotProperties;
	MCPObjectUtils::InspectObject(Widget->Slot, nullptr, Depth, SlotProperties);

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), GetWidgetStableId(WidgetBlueprint, Widget));
	WidgetObject->SetStringField(TEXT("name"), Widget->GetName());
	WidgetObject->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());

	TSharedRef<FJsonObject> SlotObject = MakeShared<FJsonObject>();
	SlotObject->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
	SlotObject->SetArrayField(TEXT("slot_properties"), SlotProperties);

	TSharedRef<FJsonObject> LayoutSummary = MakeShared<FJsonObject>();
	if (bIncludeLayout || bIncludeAnchors || bIncludePadding || bIncludeAlignment || bIncludeZOrder)
	{
		const TArray<TPair<FString, FString>> Fields{
			{TEXT("LayoutData"), TEXT("layout_data")},
			{TEXT("Offsets"), TEXT("offsets")},
			{TEXT("Anchors"), TEXT("anchors")},
			{TEXT("Padding"), TEXT("padding")},
			{TEXT("Alignment"), TEXT("alignment")},
			{TEXT("HorizontalAlignment"), TEXT("horizontal_alignment")},
			{TEXT("VerticalAlignment"), TEXT("vertical_alignment")},
			{TEXT("ZOrder"), TEXT("z_order")}
		};

		for (const TPair<FString, FString>& Field : Fields)
		{
			const bool bAllowed =
				bIncludeLayout
				|| (Field.Key.Equals(TEXT("Anchors")) && bIncludeAnchors)
				|| ((Field.Key.Equals(TEXT("Padding"))) && bIncludePadding)
				|| ((Field.Key.Equals(TEXT("Alignment")) || Field.Key.Equals(TEXT("HorizontalAlignment")) || Field.Key.Equals(TEXT("VerticalAlignment"))) && bIncludeAlignment)
				|| ((Field.Key.Equals(TEXT("ZOrder"))) && bIncludeZOrder);

			if (!bAllowed)
			{
				continue;
			}

			FString ExportedText;
			if (TryExportObjectPropertyAsText(Widget->Slot, Field.Key, ExportedText))
			{
				LayoutSummary->SetStringField(Field.Value, ExportedText);
			}
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);
	OutResult.ResultObject->SetObjectField(TEXT("slot"), SlotObject);
	OutResult.ResultObject->SetObjectField(TEXT("layout_summary"), LayoutSummary);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGReadHandler::HandleBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
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

	FArrayProperty* BindingsArrayProperty = nullptr;
	void* BindingsArrayPtr = nullptr;
	if (!TryGetWidgetBlueprintBindingArray(WidgetBlueprint, BindingsArrayProperty, BindingsArrayPtr))
	{
		TArray<TSharedPtr<FJsonValue>> EmptyBindings;
		OutResult.ResultObject = MakeShared<FJsonObject>();
		OutResult.ResultObject->SetArrayField(TEXT("bindings"), EmptyBindings);
		OutResult.ResultObject->SetNumberField(TEXT("binding_count"), 0);
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	const FStructProperty* BindingStructProperty = CastField<FStructProperty>(BindingsArrayProperty->Inner);
	FScriptArrayHelper ArrayHelper(BindingsArrayProperty, BindingsArrayPtr);
	TArray<TSharedPtr<FJsonValue>> Bindings;
	Bindings.Reserve(ArrayHelper.Num());
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		const void* EntryPtr = ArrayHelper.GetRawPtr(Index);
		if (EntryPtr == nullptr)
		{
			continue;
		}

		FString ObjectName;
		FString PropertyName;
		FString FunctionName;
		FString SourcePath;
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("ObjectName"), TEXT("WidgetName") }, ObjectName);
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("PropertyName"), TEXT("DelegatePropertyName") }, PropertyName);
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("FunctionName"), TEXT("SourceFunctionName") }, FunctionName);
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("SourcePath"), TEXT("SourcePathString") }, SourcePath);

		TSharedRef<FJsonObject> BindingObject = MakeShared<FJsonObject>();
		BindingObject->SetNumberField(TEXT("index"), Index);
		BindingObject->SetStringField(TEXT("object_name"), ObjectName);
		BindingObject->SetStringField(TEXT("property_name"), PropertyName);
		BindingObject->SetStringField(TEXT("function_name"), FunctionName);
		if (!SourcePath.IsEmpty())
		{
			BindingObject->SetStringField(TEXT("source_path"), SourcePath);
		}
		Bindings.Add(MakeShared<FJsonValueObject>(BindingObject));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("bindings"), Bindings);
	OutResult.ResultObject->SetNumberField(TEXT("binding_count"), Bindings.Num());
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGReadHandler::HandleGraphSummary(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	bool bIncludeNames = true;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetBoolField(TEXT("include_names"), bIncludeNames);
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

	auto SummarizeGraphArray = [bIncludeNames](UBlueprint* Blueprint, const TCHAR* PropertyName, TSharedRef<FJsonObject> OutSummary)
	{
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Blueprint->GetClass(), PropertyName);
		int32 Count = 0;
		int32 NodeCount = 0;
		TArray<TSharedPtr<FJsonValue>> Names;
		if (ArrayProperty != nullptr && ArrayProperty->Inner->IsA<FObjectPropertyBase>())
		{
			void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(Blueprint);
			FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayPtr);
			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
			for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
			{
				UObject* GraphObject = ObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index));
				UEdGraph* Graph = Cast<UEdGraph>(GraphObject);
				if (Graph == nullptr)
				{
					continue;
				}

				++Count;
				NodeCount += Graph->Nodes.Num();
				if (bIncludeNames)
				{
					Names.Add(MakeShared<FJsonValueString>(Graph->GetName()));
				}
			}
		}

		OutSummary->SetNumberField(TEXT("graph_count"), Count);
		OutSummary->SetNumberField(TEXT("node_count"), NodeCount);
		if (bIncludeNames)
		{
			OutSummary->SetArrayField(TEXT("names"), Names);
		}
	};

	TSharedRef<FJsonObject> UbergraphSummary = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> FunctionGraphSummary = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> DelegateGraphSummary = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> MacroGraphSummary = MakeShared<FJsonObject>();
	SummarizeGraphArray(WidgetBlueprint, TEXT("UbergraphPages"), UbergraphSummary);
	SummarizeGraphArray(WidgetBlueprint, TEXT("FunctionGraphs"), FunctionGraphSummary);
	SummarizeGraphArray(WidgetBlueprint, TEXT("DelegateSignatureGraphs"), DelegateGraphSummary);
	SummarizeGraphArray(WidgetBlueprint, TEXT("MacroGraphs"), MacroGraphSummary);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetObjectField(TEXT("ubergraph"), UbergraphSummary);
	OutResult.ResultObject->SetObjectField(TEXT("function_graphs"), FunctionGraphSummary);
	OutResult.ResultObject->SetObjectField(TEXT("delegate_graphs"), DelegateGraphSummary);
	OutResult.ResultObject->SetObjectField(TEXT("macro_graphs"), MacroGraphSummary);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}
