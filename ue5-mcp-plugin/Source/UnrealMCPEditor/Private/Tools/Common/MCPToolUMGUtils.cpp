#include "Tools/Common/MCPToolUMGUtils.h"

#include "MCPErrorCodes.h"
#include "Tools/Common/MCPToolAssetUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ContentWidget.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"
#include <type_traits>
#include <utility>

namespace MCPToolUMGUtils
{
	namespace
	{
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

		template <typename T, typename = void>
		struct THasGetInheritedAvailableNamedSlots : std::false_type
		{
		};

		template <typename T>
		struct THasGetInheritedAvailableNamedSlots<T, std::void_t<decltype(std::declval<const T*>()->GetInheritedAvailableNamedSlots())>> : std::true_type
		{
		};

		void CollectWidgetTreeNamedSlotNames(const UWidgetBlueprint* WidgetBlueprint, TArray<FName>& OutSlotNames)
		{
			OutSlotNames.Reset();
			if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
			{
				return;
			}

			WidgetBlueprint->WidgetTree->GetSlotNames(OutSlotNames);

			if constexpr (THasGetInheritedAvailableNamedSlots<UWidgetBlueprint>::value)
			{
				const TArray<FName> InheritedNamedSlots = WidgetBlueprint->GetInheritedAvailableNamedSlots();
				for (const FName InheritedNamedSlot : InheritedNamedSlots)
				{
					if (!InheritedNamedSlot.IsNone())
					{
						OutSlotNames.AddUnique(InheritedNamedSlot);
					}
				}
			}

			OutSlotNames.RemoveAll([](const FName SlotName) { return SlotName.IsNone(); });
		}

		FString JoinNamedSlotNames(const TArray<FName>& SlotNames)
		{
			TArray<FString> Names;
			Names.Reserve(SlotNames.Num());
			for (const FName SlotName : SlotNames)
			{
				if (!SlotName.IsNone())
				{
					Names.Add(SlotName.ToString());
				}
			}

			return FString::Join(Names, TEXT(", "));
		}
	}

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

	FString GetWidgetStableId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr)
		{
			return TEXT("");
		}

		return FString::Printf(TEXT("name:%s"), *Widget->GetName());
	}

	void CollectWidgetSubtreeVariableNames(UWidget* Widget, TArray<FName>& OutVariableNames)
	{
		if (Widget == nullptr)
		{
			return;
		}

		OutVariableNames.AddUnique(Widget->GetFName());

		if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
		{
			const int32 ChildCount = PanelWidget->GetChildrenCount();
			for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
			{
				CollectWidgetSubtreeVariableNames(PanelWidget->GetChildAt(ChildIndex), OutVariableNames);
			}
		}
		else if (UContentWidget* ContentWidget = Cast<UContentWidget>(Widget))
		{
			CollectWidgetSubtreeVariableNames(ContentWidget->GetContent(), OutVariableNames);
		}

		if (INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(Widget))
		{
			TArray<FName> SlotNames;
			NamedSlotHost->GetSlotNames(SlotNames);
			for (const FName& SlotName : SlotNames)
			{
				CollectWidgetSubtreeVariableNames(NamedSlotHost->GetContentForSlot(SlotName), OutVariableNames);
			}
		}
	}

	void RemoveWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, const FName WidgetName)
	{
		if (WidgetBlueprint == nullptr || WidgetName.IsNone())
		{
			return;
		}

		if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
			{
				return;
			}
		}

		if constexpr (THasOnVariableRemoved<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->OnVariableRemoved(WidgetName);
		}
		else if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->WidgetVariableNameToGuidMap.Remove(WidgetName);
		}
	}

	void EnsureWidgetGuidMap(UWidgetBlueprint* WidgetBlueprint)
	{
		(void)WidgetBlueprint;
	}

	void EnsureWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr)
		{
			return;
		}

		const FName WidgetName = Widget->GetFName();
		if (WidgetName.IsNone())
		{
			return;
		}

		if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			if (WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
			{
				return;
			}
		}

		if constexpr (THasOnVariableAdded<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->OnVariableAdded(WidgetName);
		}
		else if constexpr (THasWidgetVariableGuidMap<UWidgetBlueprint>::value)
		{
			WidgetBlueprint->WidgetVariableNameToGuidMap.Add(WidgetName, FGuid::NewGuid());
		}
	}

	UClass* ResolveWidgetClassByPath(const FString& WidgetClassPath)
	{
		UClass* WidgetClass = MCPToolAssetUtils::ResolveClassByPath(WidgetClassPath);
		if (WidgetClass == nullptr)
		{
			return nullptr;
		}

		if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			return nullptr;
		}

		if (WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
		{
			return nullptr;
		}

		return WidgetClass;
	}

	FString GetPanelSlotTypeName(UPanelWidget* PanelWidget)
	{
		if (PanelWidget == nullptr)
		{
			return FString();
		}

		const int32 ChildCount = PanelWidget->GetChildrenCount();
		for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
		{
			if (UWidget* ChildWidget = PanelWidget->GetChildAt(ChildIndex))
			{
				if (ChildWidget->Slot != nullptr)
				{
					return ChildWidget->Slot->GetClass()->GetName();
				}
			}
		}

		return UPanelSlot::StaticClass()->GetName();
	}

	FName BuildUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			return NAME_None;
		}

		FString BaseName = RequestedName;
		if (BaseName.IsEmpty() && WidgetClass != nullptr)
		{
			BaseName = WidgetClass->GetName();
			if (BaseName.StartsWith(TEXT("U")))
			{
				BaseName.RightChopInline(1);
			}
		}

		BaseName.TrimStartAndEndInline();
		if (BaseName.IsEmpty())
		{
			BaseName = TEXT("Widget");
		}

		FString CandidateName = BaseName;
		int32 Suffix = 1;
		while (WidgetBlueprint->WidgetTree->FindWidget(FName(*CandidateName)) != nullptr)
		{
			CandidateName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
		return FName(*CandidateName);
	}

	FString GetParentWidgetId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr)
		{
			return TEXT("");
		}

		if (Widget->Slot != nullptr && Widget->Slot->Parent != nullptr)
		{
			return GetWidgetStableId(WidgetBlueprint, Widget->Slot->Parent);
		}

		if (WidgetBlueprint->WidgetTree == nullptr)
		{
			return TEXT("");
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
					return GetWidgetStableId(WidgetBlueprint, CandidateHost);
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
				return TEXT("this");
			}
		}

		return TEXT("");
	}

	FString GetWidgetSlotType(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget, FString* OutNamedSlotName)
	{
		if (OutNamedSlotName != nullptr)
		{
			OutNamedSlotName->Reset();
		}

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
					if (OutNamedSlotName != nullptr)
					{
						*OutNamedSlotName = SlotName.ToString();
					}
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
				if (OutNamedSlotName != nullptr)
				{
					*OutNamedSlotName = SlotName.ToString();
				}

				return FString::Printf(TEXT("NamedSlot:%s"), *SlotName.ToString());
			}
		}

		return FString();
	}

	bool ResolveNamedSlotName(
		UWidget* ParentWidget,
		const FString& RequestedNamedSlotName,
		FName& OutSlotName,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutSlotName = NAME_None;

		if (ParentWidget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("named_slot_name requires a valid parent widget.");
			return false;
		}

		INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(ParentWidget);
		if (NamedSlotHost == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("parent_ref does not support named slots.");
			OutDiagnostic.Detail = ParentWidget->GetClass()->GetPathName();
			OutDiagnostic.Suggestion = TEXT("Choose a UUserWidget (or widget) that exposes named slots.");
			return false;
		}

		TArray<FName> SlotNames;
		NamedSlotHost->GetSlotNames(SlotNames);
		SlotNames.RemoveAll([](const FName Name) { return Name.IsNone(); });
		if (SlotNames.Num() == 0)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("parent_ref has no named slots.");
			OutDiagnostic.Detail = ParentWidget->GetName();
			return false;
		}

		if (!RequestedNamedSlotName.IsEmpty())
		{
			for (const FName SlotName : SlotNames)
			{
				if (SlotName.ToString().Equals(RequestedNamedSlotName, ESearchCase::IgnoreCase))
				{
					OutSlotName = SlotName;
					return true;
				}
			}

			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("named_slot_name does not exist on parent_ref.");
			OutDiagnostic.Detail = RequestedNamedSlotName;
			OutDiagnostic.Suggestion = FString::Printf(TEXT("Available named slots: %s"), *JoinNamedSlotNames(SlotNames));
			return false;
		}

		if (SlotNames.Num() == 1)
		{
			OutSlotName = SlotNames[0];
			return true;
		}

		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("named_slot_name is required because parent_ref has multiple named slots.");
		OutDiagnostic.Suggestion = FString::Printf(TEXT("Available named slots: %s"), *JoinNamedSlotNames(SlotNames));
		return false;
	}

	bool ResolveWidgetTreeNamedSlotName(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& RequestedNamedSlotName,
		FName& OutSlotName,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutSlotName = NAME_None;

		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Widget blueprint context is invalid.");
			return false;
		}

		TArray<FName> SlotNames;
		CollectWidgetTreeNamedSlotNames(WidgetBlueprint, SlotNames);
		if (SlotNames.Num() == 0)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("No top-level named slots are available on this widget blueprint.");
			OutDiagnostic.Suggestion = TEXT("Use parent_ref for panel/content widgets, or expose named slots in the parent widget class.");
			return false;
		}

		if (!RequestedNamedSlotName.IsEmpty())
		{
			for (const FName SlotName : SlotNames)
			{
				if (SlotName.ToString().Equals(RequestedNamedSlotName, ESearchCase::IgnoreCase))
				{
					OutSlotName = SlotName;
					return true;
				}
			}

			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("named_slot_name does not exist on this widget blueprint.");
			OutDiagnostic.Detail = RequestedNamedSlotName;
			OutDiagnostic.Suggestion = FString::Printf(TEXT("Available named slots: %s"), *JoinNamedSlotNames(SlotNames));
			return false;
		}

		if (SlotNames.Num() == 1)
		{
			OutSlotName = SlotNames[0];
			return true;
		}

		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("named_slot_name is required because multiple top-level named slots are available.");
		OutDiagnostic.Suggestion = FString::Printf(TEXT("Available named slots: %s"), *JoinNamedSlotNames(SlotNames));
		return false;
	}

	bool AttachWidgetToParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* ParentWidget,
		UWidget* ChildWidget,
		const int32 InsertIndex,
		const bool bReplaceContent,
		const FString& NamedSlotName,
		FString& OutResolvedNamedSlotName,
		FString& OutResolvedSlotType,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutResolvedNamedSlotName.Reset();
		OutResolvedSlotType.Reset();

		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || ChildWidget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Widget blueprint context is invalid.");
			return false;
		}

		if (ParentWidget == nullptr && !NamedSlotName.IsEmpty())
		{
			FName ResolvedNamedSlot = NAME_None;
			FMCPDiagnostic NamedSlotDiagnostic;
			if (!ResolveWidgetTreeNamedSlotName(WidgetBlueprint, NamedSlotName, ResolvedNamedSlot, NamedSlotDiagnostic))
			{
				OutDiagnostic = NamedSlotDiagnostic;
				return false;
			}

			UWidget* ExistingContent = WidgetBlueprint->WidgetTree->GetContentForSlot(ResolvedNamedSlot);
			if (ExistingContent != nullptr && ExistingContent != ChildWidget && !bReplaceContent)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Target top-level named slot already has content. Set replace_content=true to replace it.");
				OutDiagnostic.Detail = ResolvedNamedSlot.ToString();
				return false;
			}

			if (ExistingContent != nullptr && ExistingContent != ChildWidget && bReplaceContent)
			{
				TArray<FName> RemovedVariableNames;
				CollectWidgetSubtreeVariableNames(ExistingContent, RemovedVariableNames);
				if (!WidgetBlueprint->WidgetTree->RemoveWidget(ExistingContent))
				{
					WidgetBlueprint->WidgetTree->SetContentForSlot(ResolvedNamedSlot, nullptr);
				}
				else
				{
					for (const FName& RemovedVariableName : RemovedVariableNames)
					{
						RemoveWidgetGuidEntry(WidgetBlueprint, RemovedVariableName);
					}
				}
			}

			WidgetBlueprint->WidgetTree->SetContentForSlot(ResolvedNamedSlot, ChildWidget);
			OutResolvedNamedSlotName = ResolvedNamedSlot.ToString();
			OutResolvedSlotType = FString::Printf(TEXT("NamedSlot:%s"), *OutResolvedNamedSlotName);
			return true;
		}

		if (ParentWidget == nullptr)
		{
			if (WidgetBlueprint->WidgetTree->RootWidget != nullptr)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("WidgetTree root already exists. Provide parent_ref to add child widgets.");
				return false;
			}

			WidgetBlueprint->WidgetTree->RootWidget = ChildWidget;
			return true;
		}

		const bool bIsPanelWidget = Cast<UPanelWidget>(ParentWidget) != nullptr;
		const bool bIsContentWidget = Cast<UContentWidget>(ParentWidget) != nullptr;
		const bool bShouldUseNamedSlot =
			!NamedSlotName.IsEmpty() || (!bIsPanelWidget && !bIsContentWidget && Cast<INamedSlotInterface>(ParentWidget) != nullptr);

		if (bShouldUseNamedSlot)
		{
			FName ResolvedNamedSlot = NAME_None;
			FMCPDiagnostic NamedSlotDiagnostic;
			if (!ResolveNamedSlotName(ParentWidget, NamedSlotName, ResolvedNamedSlot, NamedSlotDiagnostic))
			{
				OutDiagnostic = NamedSlotDiagnostic;
				return false;
			}

			INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(ParentWidget);
			UWidget* ExistingContent = NamedSlotHost->GetContentForSlot(ResolvedNamedSlot);
			if (ExistingContent != nullptr && ExistingContent != ChildWidget && !bReplaceContent)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Target named slot already has content. Set replace_content=true to replace it.");
				OutDiagnostic.Detail = ResolvedNamedSlot.ToString();
				return false;
			}

			if (ExistingContent != nullptr && ExistingContent != ChildWidget && bReplaceContent)
			{
				TArray<FName> RemovedVariableNames;
				CollectWidgetSubtreeVariableNames(ExistingContent, RemovedVariableNames);
				if (!WidgetBlueprint->WidgetTree->RemoveWidget(ExistingContent))
				{
					NamedSlotHost->SetContentForSlot(ResolvedNamedSlot, nullptr);
				}
				else
				{
					for (const FName& RemovedVariableName : RemovedVariableNames)
					{
						RemoveWidgetGuidEntry(WidgetBlueprint, RemovedVariableName);
					}
				}
			}

			NamedSlotHost->SetContentForSlot(ResolvedNamedSlot, ChildWidget);
			OutResolvedNamedSlotName = ResolvedNamedSlot.ToString();
			OutResolvedSlotType = FString::Printf(TEXT("NamedSlot:%s"), *OutResolvedNamedSlotName);
			return true;
		}

		if (UContentWidget* ContentWidget = Cast<UContentWidget>(ParentWidget))
		{
			if (ContentWidget->GetContent() != nullptr && !bReplaceContent)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Target content widget already has a child. Set replace_content=true to replace it.");
				return false;
			}

			if (ContentWidget->GetContent() != nullptr && bReplaceContent)
			{
				UWidget* ExistingContent = ContentWidget->GetContent();
				TArray<FName> RemovedVariableNames;
				CollectWidgetSubtreeVariableNames(ExistingContent, RemovedVariableNames);
				if (!WidgetBlueprint->WidgetTree->RemoveWidget(ExistingContent))
				{
					OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
					OutDiagnostic.Message = TEXT("Failed to remove existing child from content widget.");
					return false;
				}

				for (const FName& RemovedVariableName : RemovedVariableNames)
				{
					RemoveWidgetGuidEntry(WidgetBlueprint, RemovedVariableName);
				}
			}

			UPanelSlot* NewSlot = ContentWidget->SetContent(ChildWidget);
			if (NewSlot == nullptr)
			{
				OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
				OutDiagnostic.Message = TEXT("Failed to attach child to content widget.");
				return false;
			}

			OutResolvedSlotType = NewSlot->GetClass()->GetName();
			return true;
		}

		UPanelWidget* PanelWidget = Cast<UPanelWidget>(ParentWidget);
		if (PanelWidget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("parent_ref is neither panel/content/named-slot capable.");
			OutDiagnostic.Detail = ParentWidget->GetClass()->GetPathName();
			return false;
		}

		UPanelSlot* NewSlot = nullptr;
		if (InsertIndex >= 0)
		{
			const int32 ClampedIndex = FMath::Clamp(InsertIndex, 0, PanelWidget->GetChildrenCount());
			NewSlot = PanelWidget->InsertChildAt(ClampedIndex, ChildWidget);
		}
		else
		{
			NewSlot = PanelWidget->AddChild(ChildWidget);
		}

		if (NewSlot == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("Failed to attach widget to panel parent.");
			OutDiagnostic.Detail = ParentWidget->GetName();
			return false;
		}

		OutResolvedSlotType = NewSlot->GetClass()->GetName();
		return true;
	}

	bool DetachWidgetFromParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		FMCPDiagnostic& OutDiagnostic)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || Widget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Widget blueprint context is invalid.");
			return false;
		}

		if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
		{
			WidgetBlueprint->WidgetTree->RootWidget = nullptr;
			return true;
		}

		if (Widget->Slot == nullptr || Widget->Slot->Parent == nullptr)
		{
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
						NamedSlotHost->SetContentForSlot(SlotName, nullptr);
						return true;
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
					WidgetBlueprint->WidgetTree->SetContentForSlot(SlotName, nullptr);
					return true;
				}
			}

			OutDiagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
			OutDiagnostic.Message = TEXT("Widget has no parent slot and cannot be detached.");
			return false;
		}

		UPanelWidget* ParentPanel = Widget->Slot->Parent;
		if (!ParentPanel->RemoveChild(Widget))
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("Failed to detach widget from its parent.");
			return false;
		}

		return true;
	}
}
