#include "UnrealMcpWidgetTools.h"

#include "UnrealMcpModule.h"

#include "Blueprint/UserWidget.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "EditorScriptingHelpers.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "UnrealMcpWidgetTools"

namespace UnrealMcp
{
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	UClass* ResolveClassPath(const FString& ClassPath, UEditorAssetSubsystem* EditorAssetSubsystem);
	UBlueprint* LoadBlueprintAsset(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& BlueprintPath, FString& OutObjectPath, FString& OutFailureReason);
	UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName, bool bCreateEventGraphIfMissing, FString& OutFailureReason);
	TSharedPtr<FJsonObject> DescribeBlueprintNode(const UEdGraphNode* Node);
	bool ResolveObjectPropertyPath(UObject* RootObject, const FString& PropertyPath, UObject*& OutOwnerObject, FProperty*& OutLeafProperty, FProperty*& OutNotifyProperty, void*& OutValuePtr, FString& OutFailureReason);
	TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr);
	UWidgetBlueprint* LoadWidgetBlueprintAsset(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& WidgetBlueprintPath, FString& OutObjectPath, FString& OutFailureReason);
	UWidgetBlueprint* LoadOrCreateWidgetBlueprintAsset(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& WidgetBlueprintPath, FString& OutObjectPath, bool& bOutCreated, FString& OutFailureReason);
	UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetName);
	UClass* ResolveWidgetClass(const FString& WidgetClassName, UEditorAssetSubsystem* EditorAssetSubsystem);
	FString MakeUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName);
	TSharedPtr<FJsonObject> DescribeWidget(UWidget* Widget);
	TSharedPtr<FJsonObject> DescribeWidgetBlueprint(UWidgetBlueprint* WidgetBlueprint, const FString& Action);
	void EnsureWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget);
	void RemoveWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName);
	void RenameWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, const FName& OldWidgetName, UWidget* Widget);
	void MarkWidgetBlueprintModified(UWidgetBlueprint* WidgetBlueprint, bool bStructurallyModified);
	bool ApplyStringToProperty(UObject* RootObject, const FString& PropertyPath, const FString& Value, FString& OutFailureReason, TSharedPtr<FJsonObject>& OutEditObject);
	void ApplyPanelSlotLayout(UPanelSlot* Slot, const FJsonObject& Arguments, int32& InOutChangedCount);
	TSharedPtr<FJsonObject> DescribeBlueprintNode(const UEdGraphNode* Node);
	UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName, bool bCreateEventGraphIfMissing, FString& OutFailureReason);
	bool BuildDefaultWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason);

	TSharedPtr<FJsonObject> DescribeWidgetTreeNode(UWidget* Widget)
	{
		TSharedPtr<FJsonObject> NodeObject = DescribeWidget(Widget);
		TArray<TSharedPtr<FJsonValue>> Children;
		if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
		{
			for (int32 Index = 0; Index < PanelWidget->GetChildrenCount(); ++Index)
			{
				if (UWidget* Child = PanelWidget->GetChildAt(Index))
				{
					Children.Add(MakeShared<FJsonValueObject>(DescribeWidgetTreeNode(Child)));
				}
			}
		}
		NodeObject->SetNumberField(TEXT("childCount"), Children.Num());
		NodeObject->SetArrayField(TEXT("children"), Children);
		return NodeObject;
	}

	UWidgetBlueprint* LoadWidgetBlueprintAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& WidgetBlueprintPath,
		FString& OutObjectPath,
		FString& OutFailureReason)
	{
		UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, OutObjectPath, OutFailureReason);
		if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
		{
			return WidgetBlueprint;
		}

		if (Blueprint)
		{
			OutFailureReason = FString::Printf(TEXT("Blueprint '%s' is not a Widget Blueprint."), *OutObjectPath);
		}
		return nullptr;
	}

	UWidgetBlueprint* LoadOrCreateWidgetBlueprintAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& WidgetBlueprintPath,
		FString& OutObjectPath,
		bool& bOutCreated,
		FString& OutFailureReason)
	{
		bOutCreated = false;
		OutObjectPath.Reset();
		OutFailureReason.Reset();

		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return nullptr;
		}

		const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(WidgetBlueprintPath, OutFailureReason);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		OutObjectPath = ObjectPath;
		if (EditorAssetSubsystem->DoesAssetExist(ObjectPath))
		{
			if (UWidgetBlueprint* ExistingWidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, ObjectPath, OutObjectPath, OutFailureReason))
			{
				return ExistingWidgetBlueprint;
			}
			if (!ObjectPath.StartsWith(TEXT("/Game/__UEvolve"), ESearchCase::CaseSensitive))
			{
				return nullptr;
			}
			// Disposable tests may leave stale registry entries after a sandbox reset; allow recreation only in the sandbox prefix.
			OutFailureReason.Reset();
		}

		FString CreateFailureReason;
		if (!EditorScriptingHelpers::IsAValidPathForCreateNewAsset(ObjectPath, CreateFailureReason))
		{
			OutFailureReason = FString::Printf(TEXT("Invalid Widget Blueprint path '%s': %s"), *ObjectPath, *CreateFailureReason);
			return nullptr;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		const FName AssetName(*FPackageName::GetLongPackageAssetName(PackageName));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
			return nullptr;
		}

		UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			Package,
			AssetName,
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UnrealMcp"))));

		if (!WidgetBlueprint)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create Widget Blueprint '%s'."), *ObjectPath);
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(WidgetBlueprint);
		Package->MarkPackageDirty();
		bOutCreated = true;
		return WidgetBlueprint;
	}

	UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return nullptr;
		}

		const FString TrimmedName = WidgetName.TrimStartAndEnd();
		if (TrimmedName.IsEmpty()
			|| TrimmedName.Equals(TEXT("Root"), ESearchCase::IgnoreCase)
			|| TrimmedName.Equals(TEXT("RootWidget"), ESearchCase::IgnoreCase))
		{
			return WidgetBlueprint->WidgetTree->RootWidget;
		}

		return WidgetBlueprint->WidgetTree->FindWidget(FName(*TrimmedName));
	}

	UClass* ResolveWidgetClass(const FString& WidgetClassName, UEditorAssetSubsystem* EditorAssetSubsystem)
	{
		const FString TrimmedClassName = WidgetClassName.TrimStartAndEnd();
		const FString LowerClassName = TrimmedClassName.ToLower();
		if (LowerClassName.IsEmpty())
		{
			return nullptr;
		}

		if (LowerClassName == TEXT("border") || LowerClassName == TEXT("uborder")) { return UBorder::StaticClass(); }
		if (LowerClassName == TEXT("button") || LowerClassName == TEXT("ubutton")) { return UButton::StaticClass(); }
		if (LowerClassName == TEXT("canvas") || LowerClassName == TEXT("canvaspanel") || LowerClassName == TEXT("ucanvaspanel")) { return UCanvasPanel::StaticClass(); }
		if (LowerClassName == TEXT("editabletextbox") || LowerClassName == TEXT("ueditabletextbox")) { return UEditableTextBox::StaticClass(); }
		if (LowerClassName == TEXT("horizontalbox") || LowerClassName == TEXT("uhorizontalbox")) { return UHorizontalBox::StaticClass(); }
		if (LowerClassName == TEXT("image") || LowerClassName == TEXT("uimage")) { return UImage::StaticClass(); }
		if (LowerClassName == TEXT("overlay") || LowerClassName == TEXT("uoverlay")) { return UOverlay::StaticClass(); }
		if (LowerClassName == TEXT("progressbar") || LowerClassName == TEXT("uprogressbar")) { return UProgressBar::StaticClass(); }
		if (LowerClassName == TEXT("scalebox") || LowerClassName == TEXT("uscalebox")) { return UScaleBox::StaticClass(); }
		if (LowerClassName == TEXT("scrollbox") || LowerClassName == TEXT("uscrollbox")) { return UScrollBox::StaticClass(); }
		if (LowerClassName == TEXT("sizebox") || LowerClassName == TEXT("usizebox")) { return USizeBox::StaticClass(); }
		if (LowerClassName == TEXT("spacer") || LowerClassName == TEXT("uspacer")) { return USpacer::StaticClass(); }
		if (LowerClassName == TEXT("text") || LowerClassName == TEXT("textblock") || LowerClassName == TEXT("utextblock")) { return UTextBlock::StaticClass(); }
		if (LowerClassName == TEXT("verticalbox") || LowerClassName == TEXT("uverticalbox")) { return UVerticalBox::StaticClass(); }

		UClass* ResolvedClass = ResolveClassPath(TrimmedClassName, EditorAssetSubsystem);
		if (ResolvedClass && ResolvedClass->IsChildOf(UWidget::StaticClass()))
		{
			return ResolvedClass;
		}

		return nullptr;
	}

	FString MakeUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return RequestedName.TrimStartAndEnd().IsEmpty() ? TEXT("Widget") : RequestedName.TrimStartAndEnd();
		}

		FString BaseName = RequestedName.TrimStartAndEnd();
		if (BaseName.IsEmpty())
		{
			BaseName = WidgetClass ? WidgetClass->GetName().Replace(TEXT("U"), TEXT("")) : TEXT("Widget");
		}

		FString CandidateName = BaseName;
		int32 Suffix = 1;
		while (WidgetBlueprint->WidgetTree->FindWidget(FName(*CandidateName)) != nullptr)
		{
			CandidateName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
		return CandidateName;
	}

	TSharedPtr<FJsonObject> DescribeWidget(UWidget* Widget)
	{
		TSharedPtr<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
		if (!Widget)
		{
			return WidgetObject;
		}

		WidgetObject->SetStringField(TEXT("name"), Widget->GetName());
		WidgetObject->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
		WidgetObject->SetBoolField(TEXT("isVariable"), Widget->bIsVariable != 0);
		WidgetObject->SetStringField(TEXT("slotClass"), Widget->Slot ? Widget->Slot->GetClass()->GetPathName() : FString());
		if (UPanelSlot* Slot = Widget->Slot)
		{
			WidgetObject->SetStringField(TEXT("parent"), Slot->Parent ? Slot->Parent->GetName() : FString());
		}
		return WidgetObject;
	}

	TSharedPtr<FJsonObject> DescribeWidgetBlueprint(UWidgetBlueprint* WidgetBlueprint, const FString& Action)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), Action);
		StructuredContent->SetStringField(TEXT("widgetBlueprint"), WidgetBlueprint ? WidgetBlueprint->GetPathName() : FString());
		StructuredContent->SetStringField(TEXT("rootWidget"), WidgetBlueprint && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget
			? WidgetBlueprint->WidgetTree->RootWidget->GetName()
			: FString());

		TArray<TSharedPtr<FJsonValue>> WidgetsArray;
		if (WidgetBlueprint && WidgetBlueprint->WidgetTree)
		{
			TArray<UWidget*> Widgets;
			WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
			for (UWidget* Widget : Widgets)
			{
				WidgetsArray.Add(MakeShared<FJsonValueObject>(DescribeWidget(Widget)));
			}
		}
		StructuredContent->SetArrayField(TEXT("widgets"), WidgetsArray);
		StructuredContent->SetNumberField(TEXT("widgetCount"), WidgetsArray.Num());
		return StructuredContent;
	}

	void EnsureWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
	{
		if (!WidgetBlueprint || !Widget)
		{
			return;
		}

		const FName WidgetName = Widget->GetFName();
		if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
		{
			WidgetBlueprint->OnVariableAdded(WidgetName);
		}
	}

	void RemoveWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName)
	{
		if (!WidgetBlueprint || WidgetName.IsNone())
		{
			return;
		}

		if (WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
		{
			WidgetBlueprint->OnVariableRemoved(WidgetName);
		}
	}

	void RenameWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, const FName& OldWidgetName, UWidget* Widget)
	{
		if (!WidgetBlueprint || !Widget || OldWidgetName.IsNone())
		{
			return;
		}

		const FName NewWidgetName = Widget->GetFName();
		if (OldWidgetName == NewWidgetName)
		{
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
			return;
		}

		const bool bHadOldGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(OldWidgetName);
		const bool bHasNewGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(NewWidgetName);
		if (bHadOldGuid && !bHasNewGuid)
		{
			WidgetBlueprint->OnVariableRenamed(OldWidgetName, NewWidgetName);
		}
		else
		{
			if (bHadOldGuid && bHasNewGuid)
			{
				WidgetBlueprint->OnVariableRemoved(OldWidgetName);
			}
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
		}
	}

	void ClearWidgetBlueprintTree(UWidgetBlueprint* WidgetBlueprint)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return;
		}

		TArray<UWidget*> ExistingWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(ExistingWidgets);
		if (WidgetBlueprint->WidgetTree->RootWidget)
		{
			WidgetBlueprint->WidgetTree->RemoveWidget(WidgetBlueprint->WidgetTree->RootWidget);
			WidgetBlueprint->WidgetTree->RootWidget = nullptr;
		}

		for (UWidget* ExistingWidget : ExistingWidgets)
		{
			if (!ExistingWidget)
			{
				continue;
			}
			const FName ExistingName = ExistingWidget->GetFName();
			ExistingWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			RemoveWidgetBlueprintGuid(WidgetBlueprint, ExistingName);
		}

		WidgetBlueprint->Bindings.Empty();
	}

	void MarkWidgetBlueprintModified(UWidgetBlueprint* WidgetBlueprint, bool bStructurallyModified = true)
	{
		if (!WidgetBlueprint)
		{
			return;
		}

		if (WidgetBlueprint->WidgetTree)
		{
			WidgetBlueprint->WidgetTree->Modify();
		}

		if (bStructurallyModified)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
		}
		WidgetBlueprint->MarkPackageDirty();
	}

	bool ApplyStringToProperty(
		UObject* RootObject,
		const FString& PropertyPath,
		const FString& Value,
		FString& OutFailureReason,
		TSharedPtr<FJsonObject>& OutEditObject)
	{
		OutEditObject = MakeShared<FJsonObject>();
		OutEditObject->SetStringField(TEXT("propertyPath"), PropertyPath);
		OutEditObject->SetStringField(TEXT("value"), Value);

		UObject* OwnerObject = nullptr;
		FProperty* LeafProperty = nullptr;
		FProperty* NotifyProperty = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveObjectPropertyPath(RootObject, PropertyPath, OwnerObject, LeafProperty, NotifyProperty, ValuePtr, OutFailureReason))
		{
			OutEditObject->SetBoolField(TEXT("success"), false);
			OutEditObject->SetStringField(TEXT("error"), OutFailureReason);
			return false;
		}

		OutEditObject->SetStringField(TEXT("ownerObjectPath"), OwnerObject->GetPathName());
		OutEditObject->SetField(TEXT("before"), PropertyValueToJson(LeafProperty, ValuePtr));

		OwnerObject->Modify();
		OwnerObject->PreEditChange(NotifyProperty);

		bool bApplied = false;
		FStringOutputDevice ImportErrors;

		if (FTextProperty* TextProperty = CastField<FTextProperty>(LeafProperty))
		{
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Value));
			bApplied = true;
		}
		else if (FStrProperty* StringProperty = CastField<FStrProperty>(LeafProperty))
		{
			StringProperty->SetPropertyValue(ValuePtr, Value);
			bApplied = true;
		}
		else if (FNameProperty* NameProperty = CastField<FNameProperty>(LeafProperty))
		{
			NameProperty->SetPropertyValue(ValuePtr, FName(*Value));
			bApplied = true;
		}
		else
		{
			bApplied = LeafProperty->ImportText_Direct(*Value, ValuePtr, OwnerObject, PPF_None, &ImportErrors) != nullptr;
		}

		if (bApplied)
		{
			FPropertyChangedEvent PropertyChangedEvent(NotifyProperty, EPropertyChangeType::ValueSet);
			OwnerObject->PostEditChangeProperty(PropertyChangedEvent);
			OwnerObject->MarkPackageDirty();
			OutEditObject->SetBoolField(TEXT("success"), true);
			OutEditObject->SetField(TEXT("after"), PropertyValueToJson(LeafProperty, ValuePtr));
			return true;
		}

		OwnerObject->PostEditChange();
		OutFailureReason = ImportErrors.IsEmpty()
			? FString::Printf(TEXT("Failed to import '%s' into property '%s'."), *Value, *PropertyPath)
			: FString(ImportErrors).TrimStartAndEnd();
		OutEditObject->SetBoolField(TEXT("success"), false);
		OutEditObject->SetStringField(TEXT("error"), OutFailureReason);
		return false;
	}

	EHorizontalAlignment ParseHorizontalAlignment(const FString& Value)
	{
		const FString Lower = Value.TrimStartAndEnd().ToLower();
		if (Lower == TEXT("center") || Lower == TEXT("centre") || Lower == TEXT("middle")) { return HAlign_Center; }
		if (Lower == TEXT("right")) { return HAlign_Right; }
		if (Lower == TEXT("fill") || Lower == TEXT("stretch")) { return HAlign_Fill; }
		return HAlign_Left;
	}

	EVerticalAlignment ParseVerticalAlignment(const FString& Value)
	{
		const FString Lower = Value.TrimStartAndEnd().ToLower();
		if (Lower == TEXT("center") || Lower == TEXT("centre") || Lower == TEXT("middle")) { return VAlign_Center; }
		if (Lower == TEXT("bottom")) { return VAlign_Bottom; }
		if (Lower == TEXT("fill") || Lower == TEXT("stretch")) { return VAlign_Fill; }
		return VAlign_Top;
	}

	bool TryGetMargin(const FJsonObject& Arguments, FMargin& OutMargin)
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;
		const bool bHasAny =
			Arguments.TryGetNumberField(TEXT("paddingLeft"), Left)
			| Arguments.TryGetNumberField(TEXT("paddingTop"), Top)
			| Arguments.TryGetNumberField(TEXT("paddingRight"), Right)
			| Arguments.TryGetNumberField(TEXT("paddingBottom"), Bottom);

		if (!bHasAny)
		{
			return false;
		}

		OutMargin = FMargin(static_cast<float>(Left), static_cast<float>(Top), static_cast<float>(Right), static_cast<float>(Bottom));
		return true;
	}

	void ApplyPanelSlotLayout(UPanelSlot* Slot, const FJsonObject& Arguments, int32& InOutChangedCount)
	{
		if (!Slot)
		{
			return;
		}

		Slot->Modify();

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			double X = 0.0;
			double Y = 0.0;
			if (Arguments.TryGetNumberField(TEXT("x"), X) | Arguments.TryGetNumberField(TEXT("y"), Y))
			{
				CanvasSlot->SetPosition(FVector2D(X, Y));
				++InOutChangedCount;
			}

			double Width = 0.0;
			double Height = 0.0;
			if (Arguments.TryGetNumberField(TEXT("width"), Width) | Arguments.TryGetNumberField(TEXT("height"), Height))
			{
				CanvasSlot->SetSize(FVector2D(Width, Height));
				++InOutChangedCount;
			}

			bool bAutoSize = false;
			if (Arguments.TryGetBoolField(TEXT("autoSize"), bAutoSize))
			{
				CanvasSlot->SetAutoSize(bAutoSize);
				++InOutChangedCount;
			}

			double ZOrder = 0.0;
			if (Arguments.TryGetNumberField(TEXT("zOrder"), ZOrder))
			{
				CanvasSlot->SetZOrder(static_cast<int32>(ZOrder));
				++InOutChangedCount;
			}

			double AlignmentX = 0.0;
			double AlignmentY = 0.0;
			if (Arguments.TryGetNumberField(TEXT("alignmentX"), AlignmentX) | Arguments.TryGetNumberField(TEXT("alignmentY"), AlignmentY))
			{
				CanvasSlot->SetAlignment(FVector2D(AlignmentX, AlignmentY));
				++InOutChangedCount;
			}

			double AnchorMinX = 0.0;
			double AnchorMinY = 0.0;
			double AnchorMaxX = 0.0;
			double AnchorMaxY = 0.0;
			const bool bHasAnchor =
				Arguments.TryGetNumberField(TEXT("anchorMinX"), AnchorMinX)
				| Arguments.TryGetNumberField(TEXT("anchorMinY"), AnchorMinY)
				| Arguments.TryGetNumberField(TEXT("anchorMaxX"), AnchorMaxX)
				| Arguments.TryGetNumberField(TEXT("anchorMaxY"), AnchorMaxY);
			if (bHasAnchor)
			{
				CanvasSlot->SetAnchors(FAnchors(AnchorMinX, AnchorMinY, AnchorMaxX, AnchorMaxY));
				++InOutChangedCount;
			}
		}

		FMargin Padding;
		const bool bHasPadding = TryGetMargin(Arguments, Padding);
		FString HorizontalAlignmentString;
		const bool bHasHorizontalAlignment = Arguments.TryGetStringField(TEXT("hAlign"), HorizontalAlignmentString) && !HorizontalAlignmentString.TrimStartAndEnd().IsEmpty();
		FString VerticalAlignmentString;
		const bool bHasVerticalAlignment = Arguments.TryGetStringField(TEXT("vAlign"), VerticalAlignmentString) && !VerticalAlignmentString.TrimStartAndEnd().IsEmpty();
		FString SizeRuleString;
		const bool bHasSizeRule = Arguments.TryGetStringField(TEXT("sizeRule"), SizeRuleString) && !SizeRuleString.TrimStartAndEnd().IsEmpty();
		double SizeValue = 1.0;
		const bool bHasSizeValue = Arguments.TryGetNumberField(TEXT("sizeValue"), SizeValue);

		if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			if (bHasPadding) { VerticalSlot->SetPadding(Padding); ++InOutChangedCount; }
			if (bHasHorizontalAlignment) { VerticalSlot->SetHorizontalAlignment(ParseHorizontalAlignment(HorizontalAlignmentString)); ++InOutChangedCount; }
			if (bHasVerticalAlignment) { VerticalSlot->SetVerticalAlignment(ParseVerticalAlignment(VerticalAlignmentString)); ++InOutChangedCount; }
			if (bHasSizeRule || bHasSizeValue)
			{
				FSlateChildSize ChildSize(SizeRuleString.TrimStartAndEnd().Equals(TEXT("automatic"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill);
				ChildSize.Value = static_cast<float>(SizeValue);
				VerticalSlot->SetSize(ChildSize);
				++InOutChangedCount;
			}
		}
		else if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			if (bHasPadding) { HorizontalSlot->SetPadding(Padding); ++InOutChangedCount; }
			if (bHasHorizontalAlignment) { HorizontalSlot->SetHorizontalAlignment(ParseHorizontalAlignment(HorizontalAlignmentString)); ++InOutChangedCount; }
			if (bHasVerticalAlignment) { HorizontalSlot->SetVerticalAlignment(ParseVerticalAlignment(VerticalAlignmentString)); ++InOutChangedCount; }
			if (bHasSizeRule || bHasSizeValue)
			{
				FSlateChildSize ChildSize(SizeRuleString.TrimStartAndEnd().Equals(TEXT("automatic"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill);
				ChildSize.Value = static_cast<float>(SizeValue);
				HorizontalSlot->SetSize(ChildSize);
				++InOutChangedCount;
			}
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			if (bHasPadding) { OverlaySlot->SetPadding(Padding); ++InOutChangedCount; }
			if (bHasHorizontalAlignment) { OverlaySlot->SetHorizontalAlignment(ParseHorizontalAlignment(HorizontalAlignmentString)); ++InOutChangedCount; }
			if (bHasVerticalAlignment) { OverlaySlot->SetVerticalAlignment(ParseVerticalAlignment(VerticalAlignmentString)); ++InOutChangedCount; }
		}
	}

	UWidget* AddTemplateWidget(UWidgetBlueprint* WidgetBlueprint, UPanelWidget* Parent, UClass* WidgetClass, const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !Parent || !WidgetClass)
		{
			return nullptr;
		}

		UWidget* Widget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
		if (!Widget)
		{
			return nullptr;
		}

		Parent->AddChild(Widget);
		EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
		return Widget;
	}

	void SetCanvasLayout(UWidget* Widget, double X, double Y, double Width, double Height, int32 ZOrder = 0)
	{
		if (UCanvasPanelSlot* CanvasSlot = Widget ? Cast<UCanvasPanelSlot>(Widget->Slot) : nullptr)
		{
			CanvasSlot->SetPosition(FVector2D(X, Y));
			CanvasSlot->SetSize(FVector2D(Width, Height));
			CanvasSlot->SetZOrder(ZOrder);
		}
	}

	void SetBoxSlotPadding(UWidget* Widget, const FMargin& Padding)
	{
		if (UVerticalBoxSlot* VerticalSlot = Widget ? Cast<UVerticalBoxSlot>(Widget->Slot) : nullptr)
		{
			VerticalSlot->SetPadding(Padding);
		}
		else if (UHorizontalBoxSlot* HorizontalSlot = Widget ? Cast<UHorizontalBoxSlot>(Widget->Slot) : nullptr)
		{
			HorizontalSlot->SetPadding(Padding);
		}
	}

	bool BuildDefaultWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutFailureReason = TEXT("Widget Blueprint or WidgetTree is unavailable.");
			return false;
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		ClearWidgetBlueprintTree(WidgetBlueprint);

		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		if (!RootCanvas)
		{
			OutFailureReason = TEXT("Failed to create RootCanvas.");
			return false;
		}
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, RootCanvas);

		UTextBlock* Title = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UTextBlock::StaticClass(), TEXT("TitleText")));
		if (Title)
		{
			Title->SetText(FText::FromString(TitleText.IsEmpty() ? TEXT("MCP Demo") : TitleText));
			SetCanvasLayout(Title, 32.0, 24.0, 760.0, 56.0, 1);
		}

		UTextBlock* Economy = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UTextBlock::StaticClass(), TEXT("EconomyText")));
		if (Economy)
		{
			Economy->SetText(FText::FromString(TEXT("Food 3/10    Gold 1    Hub Lv.1")));
			SetCanvasLayout(Economy, 820.0, 32.0, 420.0, 40.0, 1);
			Economy->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Economy);
		}

		UVerticalBox* BoardPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("BoardPanel")));
		if (BoardPanel)
		{
			BoardPanel->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, BoardPanel);
			SetCanvasLayout(BoardPanel, 64.0, 130.0, 760.0, 420.0, 1);

			UTextBlock* BoardLabel = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, BoardPanel, UTextBlock::StaticClass(), TEXT("BoardLabel")));
			if (BoardLabel)
			{
				BoardLabel->SetText(FText::FromString(TEXT("FIELD")));
				SetBoxSlotPadding(BoardLabel, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
			}

			UHorizontalBox* FieldSlots = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, BoardPanel, UHorizontalBox::StaticClass(), TEXT("FieldSlots")));
			if (FieldSlots)
			{
				FieldSlots->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, FieldSlots);
				for (int32 Index = 0; Index < 5; ++Index)
				{
					UButton* CardSlot = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, FieldSlots, UButton::StaticClass(), FString::Printf(TEXT("FieldCardSlot_%d"), Index + 1)));
					if (CardSlot)
					{
						CardSlot->bIsVariable = true;
						EnsureWidgetBlueprintGuid(WidgetBlueprint, CardSlot);
						SetBoxSlotPadding(CardSlot, FMargin(0.0f, 0.0f, 12.0f, 0.0f));
						UTextBlock* CardLabel = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*FString::Printf(TEXT("FieldCardSlotLabel_%d"), Index + 1)));
						if (CardLabel)
						{
							CardLabel->SetText(FText::FromString(FString::Printf(TEXT("Card %d"), Index + 1)));
							EnsureWidgetBlueprintGuid(WidgetBlueprint, CardLabel);
							CardSlot->AddChild(CardLabel);
						}
					}
				}
			}
		}

		UVerticalBox* ShopPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ShopPanel")));
		if (ShopPanel)
		{
			ShopPanel->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopPanel);
			SetCanvasLayout(ShopPanel, 64.0, 590.0, 840.0, 180.0, 1);

			UTextBlock* ShopLabel = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UTextBlock::StaticClass(), TEXT("ShopLabel")));
			if (ShopLabel)
			{
				ShopLabel->SetText(FText::FromString(TEXT("TAVERN SHOP")));
				SetBoxSlotPadding(ShopLabel, FMargin(0.0f, 0.0f, 0.0f, 10.0f));
			}

			UHorizontalBox* ShopSlots = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UHorizontalBox::StaticClass(), TEXT("ShopSlots")));
			if (ShopSlots)
			{
				ShopSlots->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopSlots);
				for (int32 Index = 0; Index < 5; ++Index)
				{
					UButton* ShopCard = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ShopSlots, UButton::StaticClass(), FString::Printf(TEXT("ShopCard_%d"), Index + 1)));
					if (ShopCard)
					{
						ShopCard->bIsVariable = true;
						EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopCard);
						SetBoxSlotPadding(ShopCard, FMargin(0.0f, 0.0f, 10.0f, 0.0f));
						UTextBlock* ShopCardLabel = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*FString::Printf(TEXT("ShopCardLabel_%d"), Index + 1)));
						if (ShopCardLabel)
						{
							ShopCardLabel->SetText(FText::FromString(FString::Printf(TEXT("Offer %d"), Index + 1)));
							EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopCardLabel);
							ShopCard->AddChild(ShopCardLabel);
						}
					}
				}
			}
		}

		UVerticalBox* ActionPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ActionPanel")));
		if (ActionPanel)
		{
			ActionPanel->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ActionPanel);
			SetCanvasLayout(ActionPanel, 940.0, 130.0, 280.0, 300.0, 1);

			struct FButtonSpec
			{
				const TCHAR* Name;
				const TCHAR* Label;
			};

			const FButtonSpec Buttons[] = {
				{ TEXT("RefreshButton"), TEXT("Refresh") },
				{ TEXT("UpgradeHubButton"), TEXT("Upgrade Hub") },
				{ TEXT("ReadyButton"), TEXT("Ready For Combat") },
			};

			for (const FButtonSpec& ButtonSpec : Buttons)
			{
				UButton* Button = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ActionPanel, UButton::StaticClass(), ButtonSpec.Name));
				if (!Button)
				{
					continue;
				}

				Button->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, Button);
				SetBoxSlotPadding(Button, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
				UTextBlock* ButtonLabel = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*(FString(ButtonSpec.Name) + TEXT("Label"))));
				if (ButtonLabel)
				{
					ButtonLabel->SetText(FText::FromString(ButtonSpec.Label));
					EnsureWidgetBlueprintGuid(WidgetBlueprint, ButtonLabel);
					Button->AddChild(ButtonLabel);
				}
			}
		}

		return true;
	}

	bool BuildShopWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutFailureReason = TEXT("Widget Blueprint or WidgetTree is unavailable.");
			return false;
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		ClearWidgetBlueprintTree(WidgetBlueprint);

		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		if (!RootCanvas)
		{
			OutFailureReason = TEXT("Failed to create RootCanvas.");
			return false;
		}
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, RootCanvas);

		UVerticalBox* ShopPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ShopPanel")));
		if (!ShopPanel)
		{
			OutFailureReason = TEXT("Failed to create ShopPanel.");
			return false;
		}
		ShopPanel->bIsVariable = true;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopPanel);
		SetCanvasLayout(ShopPanel, 32.0, 32.0, 980.0, 260.0, 1);

		UTextBlock* HeaderText = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UTextBlock::StaticClass(), TEXT("ShopHeaderText")));
		if (HeaderText)
		{
			HeaderText->SetText(FText::FromString(TitleText.IsEmpty() ? TEXT("Demo Shop") : TitleText));
			SetBoxSlotPadding(HeaderText, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
		}

		UHorizontalBox* OfferSlots = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UHorizontalBox::StaticClass(), TEXT("ShopOfferSlots")));
		if (OfferSlots)
		{
			OfferSlots->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, OfferSlots);
			for (int32 Index = 0; Index < 5; ++Index)
			{
				UButton* OfferButton = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, OfferSlots, UButton::StaticClass(), FString::Printf(TEXT("ShopOfferButton_%d"), Index + 1)));
				if (!OfferButton)
				{
					continue;
				}
				OfferButton->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, OfferButton);
				SetBoxSlotPadding(OfferButton, FMargin(0.0f, 0.0f, 10.0f, 0.0f));
				UTextBlock* OfferText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*FString::Printf(TEXT("ShopOfferText_%d"), Index + 1)));
				if (OfferText)
				{
					OfferText->SetText(FText::FromString(FString::Printf(TEXT("Offer %d"), Index + 1)));
					EnsureWidgetBlueprintGuid(WidgetBlueprint, OfferText);
					OfferButton->AddChild(OfferText);
				}
			}
		}

		UHorizontalBox* ActionBar = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UHorizontalBox::StaticClass(), TEXT("ShopActionBar")));
		if (ActionBar)
		{
			ActionBar->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ActionBar);

			const TCHAR* ButtonNames[] = { TEXT("RefreshButton"), TEXT("BuySelectedButton"), TEXT("SellSelectedButton") };
			const TCHAR* ButtonLabels[] = { TEXT("Refresh"), TEXT("Buy Selected"), TEXT("Sell Selected") };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				UButton* Button = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ActionBar, UButton::StaticClass(), ButtonNames[Index]));
				if (!Button)
				{
					continue;
				}
				Button->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, Button);
				SetBoxSlotPadding(Button, FMargin(0.0f, 12.0f, 12.0f, 0.0f));
				UTextBlock* ButtonText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*(FString(ButtonNames[Index]) + TEXT("Label"))));
				if (ButtonText)
				{
					ButtonText->SetText(FText::FromString(ButtonLabels[Index]));
					EnsureWidgetBlueprintGuid(WidgetBlueprint, ButtonText);
					Button->AddChild(ButtonText);
				}
			}
		}

		return true;
	}

	bool BuildResultWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutFailureReason = TEXT("Widget Blueprint or WidgetTree is unavailable.");
			return false;
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		ClearWidgetBlueprintTree(WidgetBlueprint);

		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		if (!RootCanvas)
		{
			OutFailureReason = TEXT("Failed to create RootCanvas.");
			return false;
		}
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, RootCanvas);

		UVerticalBox* ResultPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ResultPanel")));
		if (!ResultPanel)
		{
			OutFailureReason = TEXT("Failed to create ResultPanel.");
			return false;
		}
		ResultPanel->bIsVariable = true;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, ResultPanel);
		SetCanvasLayout(ResultPanel, 360.0, 160.0, 560.0, 360.0, 10);

		UTextBlock* Title = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UTextBlock::StaticClass(), TEXT("ResultTitleText")));
		if (Title)
		{
			Title->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Title);
			Title->SetText(FText::FromString(TitleText.IsEmpty() ? TEXT("Combat Result") : TitleText));
			SetBoxSlotPadding(Title, FMargin(0.0f, 0.0f, 0.0f, 16.0f));
		}

		UTextBlock* Outcome = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UTextBlock::StaticClass(), TEXT("OutcomeText")));
		if (Outcome)
		{
			Outcome->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Outcome);
			Outcome->SetText(FText::FromString(TEXT("Victory / Defeat")));
			SetBoxSlotPadding(Outcome, FMargin(0.0f, 0.0f, 0.0f, 10.0f));
		}

		UTextBlock* Damage = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UTextBlock::StaticClass(), TEXT("DamageText")));
		if (Damage)
		{
			Damage->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Damage);
			Damage->SetText(FText::FromString(TEXT("Damage: 0")));
			SetBoxSlotPadding(Damage, FMargin(0.0f, 0.0f, 0.0f, 18.0f));
		}

		UButton* ContinueButton = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UButton::StaticClass(), TEXT("ContinueButton")));
		if (ContinueButton)
		{
			ContinueButton->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ContinueButton);
			UTextBlock* ButtonText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ContinueButtonLabel"));
			if (ButtonText)
			{
				ButtonText->SetText(FText::FromString(TEXT("Continue")));
				EnsureWidgetBlueprintGuid(WidgetBlueprint, ButtonText);
				ContinueButton->AddChild(ButtonText);
			}
		}

		return true;
	}

	namespace
	{
		UEditorAssetSubsystem* GetEditorAssetSubsystem()
		{
			return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		}

		FUnrealMcpExecutionResult WidgetAddTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_add");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString WidgetClassName;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("widgetClass"), WidgetClassName) || WidgetClassName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetClass'."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			if (!WidgetBlueprint->WidgetTree)
			{
				return MakeExecutionResult(TEXT("Widget Blueprint has no WidgetTree."), nullptr, true);
			}

			UClass* WidgetClass = ResolveWidgetClass(WidgetClassName, EditorAssetSubsystem);
			if (!WidgetClass)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to resolve widgetClass '%s'."), *WidgetClassName), nullptr, true);
			}

			FString RequestedWidgetName;
			FString ParentWidgetName;
			double IndexValue = -1.0;
			bool bIsVariable = true;
			Arguments.TryGetStringField(TEXT("widgetName"), RequestedWidgetName);
			Arguments.TryGetStringField(TEXT("parentWidgetName"), ParentWidgetName);
			Arguments.TryGetNumberField(TEXT("index"), IndexValue);
			Arguments.TryGetBoolField(TEXT("isVariable"), bIsVariable);

			const FString WidgetName = MakeUniqueWidgetName(WidgetBlueprint, WidgetClass, RequestedWidgetName);
			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetAdd", "Unreal MCP Add Widget"));
			WidgetBlueprint->Modify();
			WidgetBlueprint->WidgetTree->Modify();

			UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
			if (!NewWidget)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to construct widget '%s' of class '%s'."), *WidgetName, *WidgetClass->GetPathName()), nullptr, true);
			}
			NewWidget->Modify();
			NewWidget->bIsVariable = bIsVariable;
			NewWidget->OnCreationFromPalette();

			if (!WidgetBlueprint->WidgetTree->RootWidget && ParentWidgetName.TrimStartAndEnd().IsEmpty())
			{
				WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
			}
			else
			{
				UWidget* ParentWidget = FindWidgetByName(WidgetBlueprint, ParentWidgetName);
				if (!ParentWidget)
				{
					return MakeExecutionResult(FString::Printf(TEXT("Parent widget '%s' was not found."), *ParentWidgetName), nullptr, true);
				}

				UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
				if (!ParentPanel)
				{
					return MakeExecutionResult(FString::Printf(TEXT("Parent widget '%s' is not a panel/content widget that can receive children."), *ParentWidget->GetName()), nullptr, true);
				}

				if (!ParentPanel->CanAddMoreChildren())
				{
					return MakeExecutionResult(FString::Printf(TEXT("Parent widget '%s' cannot accept children."), *ParentWidget->GetName()), nullptr, true);
				}

				UPanelSlot* NewSlot = nullptr;
				const int32 Index = static_cast<int32>(IndexValue);
				if (Index >= 0)
				{
					NewSlot = ParentPanel->InsertChildAt(FMath::Clamp(Index, 0, ParentPanel->GetChildrenCount()), NewWidget);
				}
				else
				{
					NewSlot = ParentPanel->AddChild(NewWidget);
				}

				if (!NewSlot)
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to add widget '%s' to parent '%s'."), *WidgetName, *ParentWidget->GetName()), nullptr, true);
				}
			}

			EnsureWidgetBlueprintGuid(WidgetBlueprint, NewWidget);
			MarkWidgetBlueprintModified(WidgetBlueprint, true);

			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_add"));
			StructuredContent->SetObjectField(TEXT("widget"), DescribeWidget(NewWidget));
			return MakeExecutionResult(
				FString::Printf(TEXT("Added widget %s (%s) to %s."), *NewWidget->GetName(), *WidgetClass->GetPathName(), *ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult WidgetRemoveTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_remove");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString WidgetName;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, WidgetName);
			if (!Widget)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
			}

			const FString RemovedWidgetName = Widget->GetName();
			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetRemove", "Unreal MCP Remove Widget"));
			WidgetBlueprint->Modify();
			WidgetBlueprint->WidgetTree->Modify();
			Widget->Modify();

			TArray<UWidget*> RemovedWidgets;
			RemovedWidgets.Add(Widget);
			TArray<UWidget*> ChildWidgets;
			UWidgetTree::GetChildWidgets(Widget, ChildWidgets);
			RemovedWidgets.Append(ChildWidgets);

			TSet<FString> RemovedWidgetNames;
			for (UWidget* RemovedWidget : RemovedWidgets)
			{
				if (RemovedWidget)
				{
					RemovedWidgetNames.Add(RemovedWidget->GetName());
				}
			}
			WidgetBlueprint->Bindings.RemoveAll([&RemovedWidgetNames](const FDelegateEditorBinding& Binding)
			{
				return RemovedWidgetNames.Contains(Binding.ObjectName);
			});

			const bool bRemoved = WidgetBlueprint->WidgetTree->RemoveWidget(Widget);
			if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
			{
				WidgetBlueprint->WidgetTree->RootWidget = nullptr;
			}

			if (!bRemoved)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to remove widget '%s' from %s."), *RemovedWidgetName, *ObjectPath), nullptr, true);
			}

			for (UWidget* RemovedWidget : RemovedWidgets)
			{
				if (!RemovedWidget)
				{
					continue;
				}
				const FName RemovedName = RemovedWidget->GetFName();
				RemovedWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
				RemoveWidgetBlueprintGuid(WidgetBlueprint, RemovedName);
			}

			MarkWidgetBlueprintModified(WidgetBlueprint, true);
			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_remove"));
			StructuredContent->SetStringField(TEXT("removedWidgetName"), RemovedWidgetName);
			return MakeExecutionResult(
				FString::Printf(TEXT("Removed widget %s from %s."), *RemovedWidgetName, *ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult WidgetSetPropertyTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_set_property");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString WidgetName;
			FString PropertyName;
			FString Value;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'propertyName'."), nullptr, true);
			}
			Arguments.TryGetStringField(TEXT("value"), Value);

			FString ObjectPath;
			FString FailureReason;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, WidgetName);
			if (!Widget)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetSetProperty", "Unreal MCP Set Widget Property"));
			WidgetBlueprint->Modify();
			TSharedPtr<FJsonObject> EditObject;
			if (!ApplyStringToProperty(Widget, PropertyName, Value, FailureReason, EditObject))
			{
				return MakeExecutionResult(FailureReason, EditObject, true);
			}

			MarkWidgetBlueprintModified(WidgetBlueprint, false);
			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_set_property"));
			StructuredContent->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
			StructuredContent->SetObjectField(TEXT("edit"), EditObject);

			return MakeExecutionResult(
				FString::Printf(TEXT("Set %s.%s on %s."), *Widget->GetName(), *PropertyName, *ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult WidgetSetSlotLayoutTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_set_slot_layout");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString WidgetName;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, WidgetName);
			if (!Widget)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
			}
			if (!Widget->Slot)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Widget '%s' has no parent slot. Root widget layout cannot be edited with widget_set_slot_layout."), *Widget->GetName()), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetSetSlotLayout", "Unreal MCP Set Widget Slot Layout"));
			WidgetBlueprint->Modify();
			Widget->Modify();
			int32 ChangedCount = 0;
			ApplyPanelSlotLayout(Widget->Slot, Arguments, ChangedCount);
			MarkWidgetBlueprintModified(WidgetBlueprint, false);

			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_set_slot_layout"));
			StructuredContent->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
			StructuredContent->SetNumberField(TEXT("changedFieldGroups"), ChangedCount);
			return MakeExecutionResult(
				FString::Printf(TEXT("Updated slot layout for widget %s in %s. changedFieldGroups=%d"), *Widget->GetName(), *ObjectPath, ChangedCount),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult WidgetBindEventTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_bind_event");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString WidgetName;
			FString EventName = TEXT("OnClicked");
			FString FunctionName;
			double X = 0.0;
			double Y = 0.0;
			bool bCompile = true;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
			}
			Arguments.TryGetStringField(TEXT("eventName"), EventName);
			Arguments.TryGetStringField(TEXT("functionName"), FunctionName);
			const bool bHasX = Arguments.TryGetNumberField(TEXT("x"), X);
			const bool bHasY = Arguments.TryGetNumberField(TEXT("y"), Y);
			Arguments.TryGetBoolField(TEXT("compile"), bCompile);

			const FName EventFName(*EventName.TrimStartAndEnd());
			if (EventFName.IsNone())
			{
				return MakeExecutionResult(TEXT("eventName cannot be empty."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, WidgetName);
			if (!Widget)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
			}

			FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), EventFName);
			if (!DelegateProperty)
			{
				for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					if (It->GetName().Equals(EventName, ESearchCase::IgnoreCase))
					{
						DelegateProperty = *It;
						break;
					}
				}
			}
			if (!DelegateProperty)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Event delegate '%s' was not found on widget class '%s'."), *EventName, *Widget->GetClass()->GetPathName()), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetBindEvent", "Unreal MCP Bind Widget Event"));
			WidgetBlueprint->Modify();
			Widget->Modify();
			if (!Widget->bIsVariable)
			{
				Widget->bIsVariable = true;
			}
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
			ResolveBlueprintGraph(WidgetBlueprint, UEdGraphSchema_K2::GN_EventGraph.ToString(), true, FailureReason);
			MarkWidgetBlueprintModified(WidgetBlueprint, true);
			if (bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
			}

			FObjectProperty* VariableProperty = WidgetBlueprint->SkeletonGeneratedClass
				? FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, Widget->GetFName())
				: nullptr;
			if (!VariableProperty)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Widget '%s' is not available as an object property on the Widget Blueprint skeleton class. Try compile=true or widget_bind_blueprint_variable first."), *Widget->GetName()),
					nullptr,
					true);
			}

			bool bCreated = false;
			const UK2Node_ComponentBoundEvent* ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, DelegateProperty->GetFName(), VariableProperty->GetFName());
			if (!ExistingNode)
			{
				FKismetEditorUtilities::CreateNewBoundEventForClass(Widget->GetClass(), DelegateProperty->GetFName(), WidgetBlueprint, VariableProperty);
				ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, DelegateProperty->GetFName(), VariableProperty->GetFName());
				bCreated = ExistingNode != nullptr;
			}

			UK2Node_ComponentBoundEvent* EventNode = const_cast<UK2Node_ComponentBoundEvent*>(ExistingNode);
			if (!EventNode)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to create bound event %s for widget %s."), *DelegateProperty->GetName(), *Widget->GetName()), nullptr, true);
			}

			EventNode->Modify();
			if (!FunctionName.TrimStartAndEnd().IsEmpty())
			{
				EventNode->CustomFunctionName = FName(*FunctionName.TrimStartAndEnd());
			}
			if (bHasX || bHasY)
			{
				EventNode->NodePosX = static_cast<int32>(X);
				EventNode->NodePosY = static_cast<int32>(Y);
			}

			if (UEdGraph* Graph = EventNode->GetGraph())
			{
				Graph->NotifyGraphChanged();
			}
			MarkWidgetBlueprintModified(WidgetBlueprint, true);

			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_bind_event"));
			StructuredContent->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
			StructuredContent->SetObjectField(TEXT("node"), DescribeBlueprintNode(EventNode));
			StructuredContent->SetBoolField(TEXT("created"), bCreated);
			StructuredContent->SetStringField(TEXT("eventName"), DelegateProperty->GetName());
			StructuredContent->SetStringField(TEXT("functionName"), EventNode->CustomFunctionName.ToString());
			return MakeExecutionResult(
				FString::Printf(TEXT("%s bound event %s for widget %s in %s."),
					bCreated ? TEXT("Created") : TEXT("Found existing"),
					*DelegateProperty->GetName(),
					*Widget->GetName(),
					*ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult WidgetBindBlueprintVariableTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_bind_blueprint_variable");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString WidgetName;
			FString VariableName;
			bool bExpose = true;
			bool bCompile = true;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
			}
			Arguments.TryGetStringField(TEXT("variableName"), VariableName);
			Arguments.TryGetBoolField(TEXT("expose"), bExpose);
			Arguments.TryGetBoolField(TEXT("compile"), bCompile);

			FString ObjectPath;
			FString FailureReason;
			UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, WidgetName);
			if (!Widget)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetBindBlueprintVariable", "Unreal MCP Bind Widget Blueprint Variable"));
			WidgetBlueprint->Modify();
			Widget->Modify();

			const FName OldWidgetName = Widget->GetFName();
			FString FinalVariableName = VariableName.TrimStartAndEnd();
			if (!FinalVariableName.IsEmpty() && !FinalVariableName.Equals(Widget->GetName(), ESearchCase::CaseSensitive))
			{
				if (WidgetBlueprint->WidgetTree->FindWidget(FName(*FinalVariableName)))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Cannot rename widget '%s' to '%s' because that widget name already exists."), *Widget->GetName(), *FinalVariableName), nullptr, true);
				}

				const bool bHadGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(OldWidgetName);
				if (!Widget->Rename(*FinalVariableName, Widget->GetOuter(), REN_DontCreateRedirectors))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to rename widget '%s' to '%s'."), *OldWidgetName.ToString(), *FinalVariableName), nullptr, true);
				}
				if (bHadGuid)
				{
					RenameWidgetBlueprintGuid(WidgetBlueprint, OldWidgetName, Widget);
				}
			}

			if (bExpose && !Widget->bIsVariable)
			{
				Widget->bIsVariable = true;
			}
			else if (!bExpose && Widget->bIsVariable)
			{
				Widget->bIsVariable = false;
			}
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);

			MarkWidgetBlueprintModified(WidgetBlueprint, true);
			if (bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
			}

			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_bind_blueprint_variable"));
			StructuredContent->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
			StructuredContent->SetBoolField(TEXT("expose"), bExpose);
			StructuredContent->SetBoolField(TEXT("compiled"), bCompile);
			return MakeExecutionResult(
				FString::Printf(TEXT("Widget %s is now %s as a Blueprint variable in %s."),
					*Widget->GetName(),
					bExpose ? TEXT("exposed") : TEXT("hidden"),
					*ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult WidgetBuildTemplateTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.widget_build_template");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString WidgetBlueprintPath;
			FString TemplateName = TEXT("mcp_demo_hud");
			FString TitleText = TEXT("MCP Demo");
			bool bReplaceRoot = true;
			bool bCompile = false;
			bool bSavePackage = false;
			if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
			}
			Arguments.TryGetStringField(TEXT("templateName"), TemplateName);
			Arguments.TryGetStringField(TEXT("title"), TitleText);
			Arguments.TryGetBoolField(TEXT("replaceRoot"), bReplaceRoot);
			Arguments.TryGetBoolField(TEXT("compile"), bCompile);
			Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

			if (!TemplateName.TrimStartAndEnd().Equals(TEXT("mcp_demo_hud"), ESearchCase::IgnoreCase))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unsupported widget template '%s'. Currently supported: mcp_demo_hud."), *TemplateName), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			bool bCreated = false;
			UWidgetBlueprint* WidgetBlueprint = LoadOrCreateWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, bCreated, FailureReason);
			if (!WidgetBlueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			if (!bReplaceRoot && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget)
			{
				return MakeExecutionResult(TEXT("replaceRoot=false but the Widget Blueprint already has a root widget."), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetBuildTemplate", "Unreal MCP Build Widget Template"));
			if (!BuildDefaultWidgetTemplate(WidgetBlueprint, TitleText, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			MarkWidgetBlueprintModified(WidgetBlueprint, true);

			bool bCompileSucceeded = true;
			if (bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
				bCompileSucceeded = WidgetBlueprint->Status != BS_Error;
			}

			bool bSaved = false;
			if (bSavePackage)
			{
				bSaved = EditorAssetSubsystem->SaveLoadedAsset(WidgetBlueprint, false);
			}

			TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_build_template"));
			StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
			StructuredContent->SetStringField(TEXT("templateName"), TemplateName);
			StructuredContent->SetBoolField(TEXT("created"), bCreated);
			StructuredContent->SetBoolField(TEXT("compiled"), bCompile);
			StructuredContent->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
			StructuredContent->SetBoolField(TEXT("savePackage"), bSavePackage);
			StructuredContent->SetBoolField(TEXT("saved"), bSaved);

			return MakeExecutionResult(
				FString::Printf(TEXT("Built widget template %s in %s. created=%s compiled=%s saved=%s"),
					*TemplateName,
					*ObjectPath,
					bCreated ? TEXT("true") : TEXT("false"),
					bCompile ? (bCompileSucceeded ? TEXT("true") : TEXT("error")) : TEXT("false"),
					bSaved ? TEXT("true") : TEXT("false")),
				StructuredContent,
				!bCompileSucceeded || (bSavePackage && !bSaved));
		}
	}

	FUnrealMcpExecutionResult WidgetDumpTreeTool(const FJsonObject& Arguments)
	{
		FString WidgetBlueprintPath;
		bool bIncludeDesignerTree = true;
		bool bIncludeGraphNodes = false;
		Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath);
		Arguments.TryGetBoolField(TEXT("includeDesignerTree"), bIncludeDesignerTree);
		Arguments.TryGetBoolField(TEXT("includeGraphNodes"), bIncludeGraphNodes);

		if (WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide widgetBlueprintPath."), nullptr, true);
		}

		UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TSharedPtr<FJsonObject> StructuredContent = DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_dump_tree"));
		StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
		StructuredContent->SetBoolField(TEXT("includeDesignerTree"), bIncludeDesignerTree);
		StructuredContent->SetBoolField(TEXT("includeGraphNodes"), bIncludeGraphNodes);
		if (bIncludeDesignerTree && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget)
		{
			StructuredContent->SetObjectField(TEXT("tree"), DescribeWidgetTreeNode(WidgetBlueprint->WidgetTree->RootWidget));
		}

		if (bIncludeGraphNodes)
		{
			FString GraphFailureReason;
			UEdGraph* Graph = ResolveBlueprintGraph(WidgetBlueprint, TEXT("EventGraph"), false, GraphFailureReason);
			TArray<TSharedPtr<FJsonValue>> Nodes;
			if (Graph)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node)
					{
						Nodes.Add(MakeShared<FJsonValueObject>(DescribeBlueprintNode(Node)));
					}
				}
			}
			StructuredContent->SetArrayField(TEXT("graphNodes"), Nodes);
			StructuredContent->SetNumberField(TEXT("graphNodeCount"), Nodes.Num());
			if (!GraphFailureReason.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("graphWarning"), GraphFailureReason);
			}
		}

		return MakeExecutionResult(
			FString::Printf(TEXT("Dumped Widget Blueprint tree for %s with %d widget(s)."), *ObjectPath, static_cast<int32>(StructuredContent->GetNumberField(TEXT("widgetCount")))),
			StructuredContent,
			false);
	}

	bool TryExecuteWidgetTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.widget_dump_tree"))
		{
			OutResult = WidgetDumpTreeTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_add"))
		{
			OutResult = WidgetAddTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_remove"))
		{
			OutResult = WidgetRemoveTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_set_property"))
		{
			OutResult = WidgetSetPropertyTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_set_slot_layout"))
		{
			OutResult = WidgetSetSlotLayoutTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_bind_event"))
		{
			OutResult = WidgetBindEventTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_bind_blueprint_variable"))
		{
			OutResult = WidgetBindBlueprintVariableTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.widget_build_template"))
		{
			OutResult = WidgetBuildTemplateTool(Arguments);
			return true;
		}

		return false;
	}
}

#undef LOCTEXT_NAMESPACE
