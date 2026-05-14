#include "UnrealMcpBlueprintTools.h"

#include "UnrealMcpModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorScriptingHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "UnrealMcpBlueprintTools"

namespace UnrealMcp
{
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	UObject* LoadAssetFromAnyPath(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& AnyAssetPath, FString& OutObjectPath, FString& OutFailureReason);
	UClass* ResolveClassPath(const FString& ClassPath, UEditorAssetSubsystem* EditorAssetSubsystem);
	UBlueprint* LoadBlueprintAsset(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& BlueprintPath, FString& OutObjectPath, FString& OutFailureReason);
	TSharedPtr<FJsonObject> MakeBlueprintEditStructuredContent(UBlueprint* Blueprint, UEdGraph* Graph, UEdGraphNode* Node, const FString& Action);
	TSharedPtr<FJsonObject> DescribeBlueprintNode(const UEdGraphNode* Node);
	TSharedPtr<FJsonObject> DescribeBlueprintPin(const UEdGraphPin* Pin);
	UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName, bool bCreateEventGraphIfMissing, FString& OutFailureReason);
	UEdGraphNode* FindBlueprintNodeByGuid(UEdGraph* Graph, const FString& NodeGuidString);
	UEdGraphPin* FindBlueprintPinByName(UEdGraphNode* Node, const FString& PinName);
	bool BuildBlueprintPinType(const FJsonObject& Arguments, UEditorAssetSubsystem* EditorAssetSubsystem, FEdGraphPinType& OutPinType, FString& OutFailureReason);
	UFunction* ResolveFunctionByClassAndName(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& FunctionClassPath, const FString& FunctionName, FString& OutFailureReason);
	UEdGraph* FindStandardMacroGraph(const FString& MacroName);

	void GatherBlueprintGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
	{
		if (!Blueprint)
		{
			return;
		}

		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				OutGraphs.AddUnique(Graph);
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph)
			{
				OutGraphs.AddUnique(Graph);
			}
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph)
			{
				OutGraphs.AddUnique(Graph);
			}
		}
	}

	TSharedPtr<FJsonObject> DescribeBlueprintGraph(UEdGraph* Graph, bool bIncludePins)
	{
		TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		if (!Graph)
		{
			return GraphObject;
		}

		GraphObject->SetStringField(TEXT("name"), Graph->GetName());
		GraphObject->SetStringField(TEXT("schema"), Graph->Schema ? Graph->Schema->GetPathName() : FString());
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			TSharedPtr<FJsonObject> NodeObject = DescribeBlueprintNode(Node);
			if (!bIncludePins)
			{
				NodeObject->SetArrayField(TEXT("pins"), TArray<TSharedPtr<FJsonValue>>());
			}
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
		}
		GraphObject->SetNumberField(TEXT("nodeCount"), NodeValues.Num());
		GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
		return GraphObject;
	}

	UObject* LoadAssetFromAnyPath(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& AnyAssetPath,
		FString& OutObjectPath,
		FString& OutFailureReason)
	{
		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return nullptr;
		}

		OutObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AnyAssetPath, OutFailureReason);
		if (OutObjectPath.IsEmpty())
		{
			return nullptr;
		}

		UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(OutObjectPath);
		if (!LoadedAsset)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to load asset '%s'."), *OutObjectPath);
		}

		return LoadedAsset;
	}

	UClass* ResolveClassPath(const FString& ClassPath, UEditorAssetSubsystem* EditorAssetSubsystem)
	{
		const FString TrimmedPath = ClassPath.TrimStartAndEnd();
		if (TrimmedPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* NativeClass = StaticLoadClass(UObject::StaticClass(), nullptr, *TrimmedPath))
		{
			return NativeClass;
		}

		if (!EditorAssetSubsystem)
		{
			return nullptr;
		}

		if (UClass* BlueprintClass = EditorAssetSubsystem->LoadBlueprintClass(TrimmedPath))
		{
			return BlueprintClass;
		}

		if (UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(TrimmedPath))
		{
			if (UClass* LoadedClass = Cast<UClass>(LoadedAsset))
			{
				return LoadedClass;
			}

			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
			{
				return Blueprint->GeneratedClass;
			}
		}

		return nullptr;
	}

	FString PinDirectionToString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	TSharedPtr<FJsonObject> DescribeBlueprintPin(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return PinObject;
		}

		PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObject->SetStringField(TEXT("displayName"), Pin->GetDisplayName().ToString());
		PinObject->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		PinObject->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinObject->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObject->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject.Get()->GetPathName());
		}
		PinObject->SetBoolField(TEXT("isArray"), Pin->PinType.IsArray());
		PinObject->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
		PinObject->SetStringField(TEXT("defaultObject"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> LinkedPinsArray;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}

			TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
			LinkObject->SetStringField(TEXT("nodeGuid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			LinkObject->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
			LinkedPinsArray.Add(MakeShared<FJsonValueObject>(LinkObject));
		}
		PinObject->SetArrayField(TEXT("linkedTo"), LinkedPinsArray);

		return PinObject;
	}

	TSharedPtr<FJsonObject> DescribeBlueprintNode(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		if (!Node)
		{
			return NodeObject;
		}

		NodeObject->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeObject->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetPathName());
		NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObject->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
		NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(DescribeBlueprintPin(Pin)));
		}
		NodeObject->SetArrayField(TEXT("pins"), PinsArray);

		return NodeObject;
	}

	TSharedPtr<FJsonObject> MakeBlueprintEditStructuredContent(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		UEdGraphNode* Node,
		const FString& Action)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), Action);
		StructuredContent->SetStringField(TEXT("blueprint"), Blueprint ? Blueprint->GetPathName() : FString());
		StructuredContent->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
		if (Node)
		{
			StructuredContent->SetObjectField(TEXT("node"), DescribeBlueprintNode(Node));
		}
		return StructuredContent;
	}

	UBlueprint* LoadBlueprintAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& BlueprintPath,
		FString& OutObjectPath,
		FString& OutFailureReason)
	{
		UObject* LoadedAsset = LoadAssetFromAnyPath(EditorAssetSubsystem, BlueprintPath, OutObjectPath, OutFailureReason);
		if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
		{
			return Blueprint;
		}

		if (UClass* LoadedClass = Cast<UClass>(LoadedAsset))
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedClass->ClassGeneratedBy))
			{
				return Blueprint;
			}
		}

		if (LoadedAsset)
		{
			OutFailureReason = FString::Printf(TEXT("Asset '%s' is not a Blueprint."), *OutObjectPath);
		}
		return nullptr;
	}

	UEdGraph* ResolveBlueprintGraph(
		UBlueprint* Blueprint,
		const FString& GraphName,
		bool bCreateEventGraphIfMissing,
		FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return nullptr;
		}

		const FString RequestedGraphName = GraphName.TrimStartAndEnd().IsEmpty()
			? UEdGraphSchema_K2::GN_EventGraph.ToString()
			: GraphName.TrimStartAndEnd();

		if (RequestedGraphName.Equals(UEdGraphSchema_K2::GN_EventGraph.ToString(), ESearchCase::IgnoreCase))
		{
			if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
			{
				return EventGraph;
			}
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(RequestedGraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		if (bCreateEventGraphIfMissing && RequestedGraphName.Equals(UEdGraphSchema_K2::GN_EventGraph.ToString(), ESearchCase::IgnoreCase))
		{
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint,
				UEdGraphSchema_K2::GN_EventGraph,
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
			return NewGraph;
		}

		OutFailureReason = FString::Printf(TEXT("Blueprint graph '%s' was not found in %s."), *RequestedGraphName, *Blueprint->GetPathName());
		return nullptr;
	}

	UEdGraphNode* FindBlueprintNodeByGuid(UEdGraph* Graph, const FString& NodeGuidString)
	{
		if (!Graph)
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidString.TrimStartAndEnd(), NodeGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}

		return nullptr;
	}

	UEdGraphPin* FindBlueprintPinByName(UEdGraphNode* Node, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}

		const FString TrimmedPinName = PinName.TrimStartAndEnd();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			if (Pin->PinName.ToString().Equals(TrimmedPinName, ESearchCase::IgnoreCase)
				|| Pin->GetDisplayName().ToString().Equals(TrimmedPinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}

		return nullptr;
	}

	FName NormalizePinCategory(const FString& PinCategory)
	{
		const FString Category = PinCategory.TrimStartAndEnd().ToLower();
		if (Category == TEXT("exec")) { return UEdGraphSchema_K2::PC_Exec; }
		if (Category == TEXT("bool") || Category == TEXT("boolean")) { return UEdGraphSchema_K2::PC_Boolean; }
		if (Category == TEXT("byte")) { return UEdGraphSchema_K2::PC_Byte; }
		if (Category == TEXT("class")) { return UEdGraphSchema_K2::PC_Class; }
		if (Category == TEXT("int") || Category == TEXT("integer")) { return UEdGraphSchema_K2::PC_Int; }
		if (Category == TEXT("int64")) { return UEdGraphSchema_K2::PC_Int64; }
		if (Category == TEXT("float")) { return UEdGraphSchema_K2::PC_Real; }
		if (Category == TEXT("double") || Category == TEXT("real")) { return UEdGraphSchema_K2::PC_Real; }
		if (Category == TEXT("name")) { return UEdGraphSchema_K2::PC_Name; }
		if (Category == TEXT("object")) { return UEdGraphSchema_K2::PC_Object; }
		if (Category == TEXT("interface")) { return UEdGraphSchema_K2::PC_Interface; }
		if (Category == TEXT("string")) { return UEdGraphSchema_K2::PC_String; }
		if (Category == TEXT("text")) { return UEdGraphSchema_K2::PC_Text; }
		if (Category == TEXT("struct")) { return UEdGraphSchema_K2::PC_Struct; }
		if (Category == TEXT("wildcard")) { return UEdGraphSchema_K2::PC_Wildcard; }
		if (Category == TEXT("enum")) { return UEdGraphSchema_K2::PC_Enum; }
		if (Category == TEXT("softobject")) { return UEdGraphSchema_K2::PC_SoftObject; }
		if (Category == TEXT("softclass")) { return UEdGraphSchema_K2::PC_SoftClass; }
		return FName(*PinCategory.TrimStartAndEnd());
	}

	UObject* ResolvePinSubCategoryObject(
		const FName& PinCategory,
		const FString& SubCategoryObjectPath,
		UEditorAssetSubsystem* EditorAssetSubsystem,
		FString& OutFailureReason)
	{
		const FString TrimmedPath = SubCategoryObjectPath.TrimStartAndEnd();
		if (TrimmedPath.IsEmpty())
		{
			return nullptr;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Object
			|| PinCategory == UEdGraphSchema_K2::PC_Class
			|| PinCategory == UEdGraphSchema_K2::PC_Interface
			|| PinCategory == UEdGraphSchema_K2::PC_SoftObject
			|| PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			if (UClass* Class = ResolveClassPath(TrimmedPath, EditorAssetSubsystem))
			{
				return Class;
			}
			OutFailureReason = FString::Printf(TEXT("Unable to resolve class subCategoryObjectPath '%s'."), *TrimmedPath);
			return nullptr;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *TrimmedPath))
			{
				return Struct;
			}
			OutFailureReason = FString::Printf(TEXT("Unable to resolve struct subCategoryObjectPath '%s'."), *TrimmedPath);
			return nullptr;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Enum || PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			if (UEnum* Enum = LoadObject<UEnum>(nullptr, *TrimmedPath))
			{
				return Enum;
			}
			OutFailureReason = FString::Printf(TEXT("Unable to resolve enum subCategoryObjectPath '%s'."), *TrimmedPath);
			return nullptr;
		}

		FString ObjectPath;
		UObject* LoadedObject = LoadAssetFromAnyPath(EditorAssetSubsystem, TrimmedPath, ObjectPath, OutFailureReason);
		return LoadedObject;
	}

	bool BuildBlueprintPinType(
		const FJsonObject& Arguments,
		UEditorAssetSubsystem* EditorAssetSubsystem,
		FEdGraphPinType& OutPinType,
		FString& OutFailureReason)
	{
		FString PinCategoryString = TEXT("bool");
		Arguments.TryGetStringField(TEXT("pinCategory"), PinCategoryString);

		FName PinCategory = NormalizePinCategory(PinCategoryString);
		if (PinCategory.IsNone())
		{
			OutFailureReason = TEXT("pinCategory cannot be empty.");
			return false;
		}

		FString PinSubCategoryString;
		Arguments.TryGetStringField(TEXT("pinSubCategory"), PinSubCategoryString);

		UObject* PinSubCategoryObject = nullptr;
		FString SubCategoryObjectPath;
		if (Arguments.TryGetStringField(TEXT("subCategoryObjectPath"), SubCategoryObjectPath) && !SubCategoryObjectPath.TrimStartAndEnd().IsEmpty())
		{
			PinSubCategoryObject = ResolvePinSubCategoryObject(PinCategory, SubCategoryObjectPath, EditorAssetSubsystem, OutFailureReason);
			if (!PinSubCategoryObject)
			{
				return false;
			}
		}

		OutPinType = FEdGraphPinType();
		OutPinType.PinCategory = PinCategory;
		OutPinType.PinSubCategory = FName(*PinSubCategoryString.TrimStartAndEnd());
		OutPinType.PinSubCategoryObject = PinSubCategoryObject;

		if (PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			const FString Lower = PinCategoryString.ToLower();
			OutPinType.PinSubCategory = Lower == TEXT("float") ? UEdGraphSchema_K2::PC_Float : UEdGraphSchema_K2::PC_Double;
		}

		FString ContainerTypeString = TEXT("none");
		Arguments.TryGetStringField(TEXT("containerType"), ContainerTypeString);
		ContainerTypeString = ContainerTypeString.TrimStartAndEnd().ToLower();
		if (ContainerTypeString == TEXT("array"))
		{
			OutPinType.ContainerType = EPinContainerType::Array;
		}
		else if (ContainerTypeString == TEXT("set"))
		{
			OutPinType.ContainerType = EPinContainerType::Set;
		}
		else if (ContainerTypeString == TEXT("map"))
		{
			OutPinType.ContainerType = EPinContainerType::Map;
		}
		else
		{
			OutPinType.ContainerType = EPinContainerType::None;
		}

		return true;
	}

	UFunction* ResolveFunctionByClassAndName(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& FunctionClassPath,
		const FString& FunctionName,
		FString& OutFailureReason)
	{
		UClass* FunctionClass = ResolveClassPath(FunctionClassPath, EditorAssetSubsystem);
		if (!FunctionClass)
		{
			OutFailureReason = FString::Printf(TEXT("Unable to resolve functionClassPath '%s'."), *FunctionClassPath);
			return nullptr;
		}

		UFunction* Function = FunctionClass->FindFunctionByName(FName(*FunctionName.TrimStartAndEnd()));
		if (!Function)
		{
			OutFailureReason = FString::Printf(TEXT("Function '%s' was not found on class '%s'."), *FunctionName, *FunctionClass->GetPathName());
			return nullptr;
		}

		return Function;
	}

	UEdGraph* FindStandardMacroGraph(const FString& MacroName)
	{
		UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
		if (!StandardMacros)
		{
			StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros"));
		}
		if (!StandardMacros)
		{
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		StandardMacros->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(MacroName.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	namespace
	{
		UEditorAssetSubsystem* GetEditorAssetSubsystem()
		{
			return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		}

		FUnrealMcpExecutionResult AddVariableTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_add_variable");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			FString VariableName;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("name"), VariableName) || VariableName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'name'."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FEdGraphPinType PinType;
			if (!BuildBlueprintPinType(Arguments, EditorAssetSubsystem, PinType, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString DefaultValue;
			Arguments.TryGetStringField(TEXT("defaultValue"), DefaultValue);

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddVariable", "Unreal MCP Add Blueprint Variable"));
			Blueprint->Modify();
			const FName VarFName(*VariableName.TrimStartAndEnd());
			const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarFName, PinType, DefaultValue);
			if (!bAdded)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Failed to add variable '%s' to %s. It may already exist or conflict with a parent member."), *VariableName, *ObjectPath),
					nullptr,
					true);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, nullptr, nullptr, TEXT("bp_add_variable"));
			StructuredContent->SetStringField(TEXT("variableName"), VarFName.ToString());
			StructuredContent->SetStringField(TEXT("pinCategory"), PinType.PinCategory.ToString());
			StructuredContent->SetStringField(TEXT("pinSubCategory"), PinType.PinSubCategory.ToString());
			StructuredContent->SetStringField(TEXT("defaultValue"), DefaultValue);

			return MakeExecutionResult(
				FString::Printf(TEXT("Added Blueprint variable %s to %s."), *VarFName.ToString(), *ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult AddFunctionTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_add_function");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			FString FunctionName;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("functionName"), FunctionName) || FunctionName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'functionName'."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* ExistingGraph : Graphs)
			{
				if (ExistingGraph && ExistingGraph->GetName().Equals(FunctionName.TrimStartAndEnd(), ESearchCase::IgnoreCase))
				{
					TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, ExistingGraph, nullptr, TEXT("bp_add_function"));
					StructuredContent->SetBoolField(TEXT("created"), false);
					return MakeExecutionResult(
						FString::Printf(TEXT("Function graph %s already exists in %s."), *ExistingGraph->GetName(), *ObjectPath),
						StructuredContent,
						false);
				}
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddFunction", "Unreal MCP Add Blueprint Function"));
			Blueprint->Modify();
			UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint,
				FName(*FunctionName.TrimStartAndEnd()),
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, FunctionGraph, nullptr, TEXT("bp_add_function"));
			StructuredContent->SetBoolField(TEXT("created"), true);
			return MakeExecutionResult(
				FString::Printf(TEXT("Created function graph %s in %s."), *FunctionGraph->GetName(), *ObjectPath),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult AddEventNodeTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_add_event_node");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			FString EventName = TEXT("ReceiveBeginPlay");
			FString OwnerClassPath = TEXT("/Script/Engine.Actor");
			double X = 0.0;
			double Y = 0.0;
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);
			Arguments.TryGetStringField(TEXT("eventName"), EventName);
			Arguments.TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath);
			Arguments.TryGetNumberField(TEXT("x"), X);
			Arguments.TryGetNumberField(TEXT("y"), Y);

			if (EventName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'eventName'."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const FName EventFName(*EventName.TrimStartAndEnd());
			UEdGraphNode* NewNode = nullptr;
			const bool bCustomEvent = OwnerClassPath.TrimStartAndEnd().Equals(TEXT("custom"), ESearchCase::IgnoreCase)
				|| OwnerClassPath.TrimStartAndEnd().IsEmpty();

			if (!bCustomEvent)
			{
				UClass* OwnerClass = ResolveClassPath(OwnerClassPath, EditorAssetSubsystem);
				if (!OwnerClass)
				{
					return MakeExecutionResult(FString::Printf(TEXT("Unable to resolve ownerClassPath '%s'."), *OwnerClassPath), nullptr, true);
				}
				if (!OwnerClass->FindFunctionByName(EventFName))
				{
					return MakeExecutionResult(
						FString::Printf(TEXT("Event function '%s' was not found on class '%s'."), *EventName, *OwnerClass->GetPathName()),
						nullptr,
						true);
				}

				if (UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, OwnerClass, EventFName))
				{
					TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, ExistingEvent->GetGraph(), ExistingEvent, TEXT("bp_add_event_node"));
					StructuredContent->SetBoolField(TEXT("created"), false);
					return MakeExecutionResult(
						FString::Printf(TEXT("Override event %s already exists in %s."), *EventName, *ObjectPath),
						StructuredContent,
						false);
				}

				const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddEventNode", "Unreal MCP Add Blueprint Event Node"));
				Graph->Modify();
				Blueprint->Modify();
				NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Event>(
					Graph,
					FVector2D(X, Y),
					EK2NewNodeFlags::None,
					[EventFName, OwnerClass](UK2Node_Event* NewInstance)
					{
						NewInstance->EventReference.SetExternalMember(EventFName, OwnerClass);
						NewInstance->bOverrideFunction = true;
					});
			}
			else
			{
				if (UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindCustomEventNode(Blueprint, EventFName))
				{
					TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, ExistingEvent->GetGraph(), ExistingEvent, TEXT("bp_add_event_node"));
					StructuredContent->SetBoolField(TEXT("created"), false);
					return MakeExecutionResult(
						FString::Printf(TEXT("Custom event %s already exists in %s."), *EventName, *ObjectPath),
						StructuredContent,
						false);
				}

				const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddCustomEventNode", "Unreal MCP Add Blueprint Custom Event Node"));
				Graph->Modify();
				Blueprint->Modify();
				NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CustomEvent>(
					Graph,
					FVector2D(X, Y),
					EK2NewNodeFlags::None,
					[EventFName](UK2Node_CustomEvent* NewInstance)
					{
						NewInstance->CustomFunctionName = EventFName;
					});
			}

			if (!NewNode)
			{
				return MakeExecutionResult(TEXT("Failed to create event node."), nullptr, true);
			}

			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_event_node"));
			StructuredContent->SetBoolField(TEXT("created"), true);
			StructuredContent->SetBoolField(TEXT("customEvent"), bCustomEvent);
			return MakeExecutionResult(
				FString::Printf(TEXT("Added event node %s to %s:%s."), *EventName, *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult AddCallFunctionNodeTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_add_call_function_node");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			FString FunctionClassPath;
			FString FunctionName;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("functionClassPath"), FunctionClassPath) || FunctionClassPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'functionClassPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("functionName"), FunctionName) || FunctionName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'functionName'."), nullptr, true);
			}

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			double X = 200.0;
			double Y = 0.0;
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);
			Arguments.TryGetNumberField(TEXT("x"), X);
			Arguments.TryGetNumberField(TEXT("y"), Y);

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UFunction* Function = ResolveFunctionByClassAndName(EditorAssetSubsystem, FunctionClassPath, FunctionName, FailureReason);
			if (!Function)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddCallFunctionNode", "Unreal MCP Add Blueprint Call Function Node"));
			Graph->Modify();
			Blueprint->Modify();
			UK2Node_CallFunction* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
				Graph,
				FVector2D(X, Y),
				EK2NewNodeFlags::None,
				[Function](UK2Node_CallFunction* NewInstance)
				{
					NewInstance->SetFromFunction(Function);
				});
			if (!NewNode)
			{
				return MakeExecutionResult(TEXT("Failed to create call-function node."), nullptr, true);
			}

			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_call_function_node"));
			StructuredContent->SetStringField(TEXT("function"), Function->GetPathName());
			return MakeExecutionResult(
				FString::Printf(TEXT("Added call-function node %s to %s:%s."), *FunctionName, *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult AddBranchNodeTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_add_branch_node");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			double X = 400.0;
			double Y = 0.0;
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);
			Arguments.TryGetNumberField(TEXT("x"), X);
			Arguments.TryGetNumberField(TEXT("y"), Y);

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddBranchNode", "Unreal MCP Add Blueprint Branch Node"));
			Graph->Modify();
			Blueprint->Modify();
			UK2Node_IfThenElse* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_IfThenElse>(
				Graph,
				FVector2D(X, Y),
				EK2NewNodeFlags::None);
			if (!NewNode)
			{
				return MakeExecutionResult(TEXT("Failed to create Branch node."), nullptr, true);
			}

			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_branch_node"));
			return MakeExecutionResult(
				FString::Printf(TEXT("Added Branch node to %s:%s."), *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult AddForEachNodeTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_add_for_each_node");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			FString MacroName = TEXT("ForEachLoop");
			double X = 400.0;
			double Y = 180.0;
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);
			Arguments.TryGetStringField(TEXT("macroName"), MacroName);
			Arguments.TryGetNumberField(TEXT("x"), X);
			Arguments.TryGetNumberField(TEXT("y"), Y);

			UEdGraph* MacroGraph = FindStandardMacroGraph(MacroName);
			if (!MacroGraph)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to find StandardMacros graph '%s'."), *MacroName), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddForEachNode", "Unreal MCP Add Blueprint ForEach Node"));
			Graph->Modify();
			Blueprint->Modify();
			UK2Node_MacroInstance* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MacroInstance>(
				Graph,
				FVector2D(X, Y),
				EK2NewNodeFlags::None,
				[MacroGraph](UK2Node_MacroInstance* NewInstance)
				{
					NewInstance->SetMacroGraph(MacroGraph);
				});
			if (!NewNode)
			{
				return MakeExecutionResult(TEXT("Failed to create ForEach macro node."), nullptr, true);
			}

			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_for_each_node"));
			StructuredContent->SetStringField(TEXT("macroName"), MacroGraph->GetName());
			return MakeExecutionResult(
				FString::Printf(TEXT("Added %s node to %s:%s."), *MacroGraph->GetName(), *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ConnectPinsTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_connect_pins");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			FString FromNodeGuid;
			FString FromPinName;
			FString ToNodeGuid;
			FString ToPinName;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("fromNodeGuid"), FromNodeGuid) || FromNodeGuid.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'fromNodeGuid'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("fromPin"), FromPinName) || FromPinName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'fromPin'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("toNodeGuid"), ToNodeGuid) || ToNodeGuid.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'toNodeGuid'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("toPin"), ToPinName) || ToPinName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'toPin'."), nullptr, true);
			}

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UEdGraphNode* FromNode = FindBlueprintNodeByGuid(Graph, FromNodeGuid);
			UEdGraphNode* ToNode = FindBlueprintNodeByGuid(Graph, ToNodeGuid);
			if (!FromNode || !ToNode)
			{
				return MakeExecutionResult(TEXT("Unable to find one or both nodes by GUID in the target graph."), nullptr, true);
			}

			UEdGraphPin* FromPin = FindBlueprintPinByName(FromNode, FromPinName);
			UEdGraphPin* ToPin = FindBlueprintPinByName(ToNode, ToPinName);
			if (!FromPin || !ToPin)
			{
				return MakeExecutionResult(TEXT("Unable to find one or both pins by name/displayName."), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpConnectPins", "Unreal MCP Connect Blueprint Pins"));
			Graph->Modify();
			FromNode->Modify();
			ToNode->Modify();
			const bool bConnected = Graph->GetSchema() && Graph->GetSchema()->TryCreateConnection(FromPin, ToPin);
			if (!bConnected)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("K2 schema rejected connection %s.%s -> %s.%s."), *FromNodeGuid, *FromPinName, *ToNodeGuid, *ToPinName),
					nullptr,
					true);
			}

			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, nullptr, TEXT("bp_connect_pins"));
			StructuredContent->SetObjectField(TEXT("fromNode"), DescribeBlueprintNode(FromNode));
			StructuredContent->SetObjectField(TEXT("toNode"), DescribeBlueprintNode(ToNode));
			return MakeExecutionResult(
				FString::Printf(TEXT("Connected %s.%s -> %s.%s in %s:%s."), *FromNodeGuid, *FromPinName, *ToNodeGuid, *ToPinName, *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult SetPinDefaultTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_set_pin_default");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			FString NodeGuid;
			FString PinName;
			FString Value;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("nodeGuid"), NodeGuid) || NodeGuid.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'nodeGuid'."), nullptr, true);
			}
			if (!Arguments.TryGetStringField(TEXT("pinName"), PinName) || PinName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'pinName'."), nullptr, true);
			}
			Arguments.TryGetStringField(TEXT("value"), Value);

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			UEdGraphNode* Node = FindBlueprintNodeByGuid(Graph, NodeGuid);
			if (!Node)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to find node '%s' in graph '%s'."), *NodeGuid, *Graph->GetName()), nullptr, true);
			}

			UEdGraphPin* Pin = FindBlueprintPinByName(Node, PinName);
			if (!Pin)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to find pin '%s' on node '%s'."), *PinName, *NodeGuid), nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpSetPinDefault", "Unreal MCP Set Blueprint Pin Default"));
			Graph->Modify();
			Node->Modify();
			if (!Graph->GetSchema())
			{
				return MakeExecutionResult(TEXT("Target graph has no schema."), nullptr, true);
			}

			Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, Node, TEXT("bp_set_pin_default"));
			StructuredContent->SetObjectField(TEXT("pin"), DescribeBlueprintPin(Pin));
			return MakeExecutionResult(
				FString::Printf(TEXT("Set default value for %s.%s to '%s' in %s:%s."), *NodeGuid, *PinName, *Value, *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ArrangeGraphTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_arrange_graph");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}

			FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
			double OriginX = 0.0;
			double OriginY = 0.0;
			double ColumnSpacing = 320.0;
			double RowSpacing = 180.0;
			Arguments.TryGetStringField(TEXT("graphName"), GraphName);
			Arguments.TryGetNumberField(TEXT("originX"), OriginX);
			Arguments.TryGetNumberField(TEXT("originY"), OriginY);
			Arguments.TryGetNumberField(TEXT("columnSpacing"), ColumnSpacing);
			Arguments.TryGetNumberField(TEXT("rowSpacing"), RowSpacing);
			const int32 Columns = FMath::Max(1, GetPositiveIntArgument(Arguments, TEXT("columns"), 4));

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpArrangeGraph", "Unreal MCP Arrange Blueprint Graph"));
			Graph->Modify();

			int32 ArrangedCount = 0;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				const int32 Column = ArrangedCount % Columns;
				const int32 Row = ArrangedCount / Columns;
				Node->Modify();
				Node->NodePosX = static_cast<int32>(OriginX + Column * ColumnSpacing);
				Node->NodePosY = static_cast<int32>(OriginY + Row * RowSpacing);
				++ArrangedCount;
			}

			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, Graph, nullptr, TEXT("bp_arrange_graph"));
			StructuredContent->SetNumberField(TEXT("arrangedCount"), ArrangedCount);
			StructuredContent->SetNumberField(TEXT("columns"), Columns);
			return MakeExecutionResult(
				FString::Printf(TEXT("Arranged %d nodes in %s:%s."), ArrangedCount, *ObjectPath, *Graph->GetName()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult CompileSaveTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.bp_compile_save");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString BlueprintPath;
			if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
			}

			bool bSavePackage = true;
			Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

			FString ObjectPath;
			FString FailureReason;
			UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
			if (!Blueprint)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			const bool bCompileSucceeded = Blueprint->Status != BS_Error;
			bool bSaved = false;
			if (bSavePackage)
			{
				bSaved = EditorAssetSubsystem->SaveLoadedAsset(Blueprint, false);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeBlueprintEditStructuredContent(Blueprint, nullptr, nullptr, TEXT("bp_compile_save"));
			StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
			StructuredContent->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
			StructuredContent->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
			StructuredContent->SetBoolField(TEXT("savePackage"), bSavePackage);
			StructuredContent->SetBoolField(TEXT("saved"), bSaved);

			return MakeExecutionResult(
				FString::Printf(TEXT("Compiled Blueprint %s. status=%s saved=%s"), *ObjectPath, bCompileSucceeded ? TEXT("success") : TEXT("error"), bSaved ? TEXT("true") : TEXT("false")),
				StructuredContent,
				!bCompileSucceeded || (bSavePackage && !bSaved));
		}

		FUnrealMcpExecutionResult CompileBlueprintTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.compile_blueprint");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString AssetPath;
			if (!Arguments.TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UObject* LoadedAsset = LoadAssetFromAnyPath(EditorAssetSubsystem, AssetPath, ObjectPath, FailureReason);
			UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
			if (!Blueprint)
			{
				return MakeExecutionResult(
					LoadedAsset
						? FString::Printf(TEXT("Asset '%s' is not a Blueprint."), *ObjectPath)
						: FailureReason,
					nullptr,
					true);
			}

			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			const bool bSucceeded = Blueprint->Status != BS_Error;

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
			StructuredContent->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
			return MakeExecutionResult(
				FString::Printf(TEXT("Compiled Blueprint %s. status=%s"), *ObjectPath, bSucceeded ? TEXT("success") : TEXT("error")),
				StructuredContent,
				!bSucceeded);
		}

		FUnrealMcpExecutionResult CompileBlueprintsInPathTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.compile_blueprints_in_path");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString Path = TEXT("/Game");
			bool bRecursive = true;
			Arguments.TryGetStringField(TEXT("path"), Path);
			Arguments.TryGetBoolField(TEXT("recursive"), bRecursive);
			const int32 Limit = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("limit"), 100), 500);

			if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
			{
				return MakeExecutionResult(TEXT("The path argument must be a Content Browser path like /Game."), nullptr, true);
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

			FARFilter Filter;
			Filter.PackagePaths.Add(*Path);
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = bRecursive;
			Filter.bRecursiveClasses = true;

			TArray<FAssetData> AssetData;
			AssetRegistryModule.Get().GetAssets(Filter, AssetData);
			AssetData.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
			});

			int32 CompiledCount = 0;
			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			bool bTruncated = false;
			TArray<TSharedPtr<FJsonValue>> ResultsArray;
			TArray<FString> FailureLines;

			for (const FAssetData& Asset : AssetData)
			{
				if (CompiledCount >= Limit)
				{
					bTruncated = true;
					break;
				}

				UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(Asset.GetSoftObjectPath().ToString());
				UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
				if (!Blueprint)
				{
					continue;
				}

				FKismetEditorUtilities::CompileBlueprint(Blueprint);

				++CompiledCount;
				const bool bSucceeded = Blueprint->Status != BS_Error;
				if (bSucceeded)
				{
					++SuccessCount;
				}
				else
				{
					++FailureCount;
					FailureLines.Add(Asset.GetSoftObjectPath().ToString());
				}

				TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
				ResultObject->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
				ResultObject->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				ResultObject->SetBoolField(TEXT("success"), bSucceeded);
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("path"), Path);
			StructuredContent->SetBoolField(TEXT("recursive"), bRecursive);
			StructuredContent->SetNumberField(TEXT("limit"), Limit);
			StructuredContent->SetNumberField(TEXT("totalBlueprintAssets"), AssetData.Num());
			StructuredContent->SetNumberField(TEXT("compiledCount"), CompiledCount);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetArrayField(TEXT("results"), ResultsArray);

			FString Text = FString::Printf(
				TEXT("Compiled %d Blueprint assets under %s. success=%d failure=%d"),
				CompiledCount,
				*Path,
				SuccessCount,
				FailureCount);
			if (bTruncated)
			{
				Text += FString::Printf(TEXT(" (stopped at limit %d)"), Limit);
			}
			if (FailureLines.Num() > 0)
			{
				Text += TEXT("\nFailed:\n") + FString::Join(FailureLines, TEXT("\n"));
			}

			return MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		FUnrealMcpExecutionResult CreateBlueprintClassTool(const FJsonObject& Arguments)
		{
			const FString ToolName = TEXT("unreal.create_blueprint_class");
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString AssetPath;
			if (!Arguments.TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("The assetPath argument is required."), nullptr, true);
			}

			FString ParentClassPath = TEXT("/Script/Engine.Actor");
			bool bOpenAfterCreate = true;
			bool bCompile = true;
			Arguments.TryGetStringField(TEXT("parentClass"), ParentClassPath);
			Arguments.TryGetBoolField(TEXT("openAfterCreate"), bOpenAfterCreate);
			Arguments.TryGetBoolField(TEXT("compile"), bCompile);

			UClass* ParentClass = ResolveClassPath(ParentClassPath, EditorAssetSubsystem);
			if (!ParentClass)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to resolve parent class '%s'."), *ParentClassPath), nullptr, true);
			}

			if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Cannot create a Blueprint from class '%s'."), *ParentClass->GetPathName()), nullptr, true);
			}

			FString FailureReason;
			const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AssetPath, FailureReason);
			if (ObjectPath.IsEmpty())
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to resolve asset path: %s"), *FailureReason), nullptr, true);
			}

			if (!EditorScriptingHelpers::IsAValidPathForCreateNewAsset(ObjectPath, FailureReason))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Invalid asset path '%s': %s"), *ObjectPath, *FailureReason), nullptr, true);
			}

			if (EditorAssetSubsystem->DoesAssetExist(ObjectPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Asset '%s' already exists."), *ObjectPath), nullptr, true);
			}

			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			const FName AssetName(*FPackageName::GetLongPackageAssetName(PackageName));
			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to create package '%s'."), *PackageName), nullptr, true);
			}

			UClass* BlueprintClass = nullptr;
			UClass* BlueprintGeneratedClass = nullptr;
			IKismetCompilerInterface& KismetCompilerModule = FModuleManager::LoadModuleChecked<IKismetCompilerInterface>(KISMET_COMPILER_MODULENAME);
			KismetCompilerModule.GetBlueprintTypesForClass(ParentClass, BlueprintClass, BlueprintGeneratedClass);

			UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				AssetName,
				BPTYPE_Normal,
				BlueprintClass,
				BlueprintGeneratedClass,
				FName(TEXT("UnrealMcp")));

			if (!NewBlueprint)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to create Blueprint '%s'."), *ObjectPath), nullptr, true);
			}

			FAssetRegistryModule::AssetCreated(NewBlueprint);
			Package->MarkPackageDirty();

			if (bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(NewBlueprint);
			}

			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			if (bOpenAfterCreate && AssetEditorSubsystem)
			{
				AssetEditorSubsystem->OpenEditorForAsset(NewBlueprint);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
			StructuredContent->SetStringField(TEXT("packageName"), PackageName);
			StructuredContent->SetStringField(TEXT("parentClass"), ParentClass->GetPathName());
			StructuredContent->SetBoolField(TEXT("compiled"), bCompile);
			StructuredContent->SetBoolField(TEXT("opened"), bOpenAfterCreate);
			if (NewBlueprint->GeneratedClass)
			{
				StructuredContent->SetStringField(TEXT("generatedClass"), NewBlueprint->GeneratedClass->GetPathName());
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Created Blueprint %s with parent %s."), *ObjectPath, *ParentClass->GetPathName()),
				StructuredContent,
				false);
		}
	}

	FUnrealMcpExecutionResult ListGraphNodesTool(const FJsonObject& Arguments)
	{
		FString BlueprintPath;
		FString GraphName;
		bool bIncludePins = true;
		Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetBoolField(TEXT("includePins"), bIncludePins);

		if (BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide blueprintPath."), nullptr, true);
		}

		UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TArray<UEdGraph*> Graphs;
		if (GraphName.TrimStartAndEnd().IsEmpty())
		{
			GatherBlueprintGraphs(Blueprint, Graphs);
		}
		else
		{
			UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
			if (!Graph)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			Graphs.Add(Graph);
		}

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		int32 NodeCount = 0;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			NodeCount += Graph->Nodes.Num();
			GraphValues.Add(MakeShared<FJsonValueObject>(DescribeBlueprintGraph(Graph, bIncludePins)));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("bp_list_graph_nodes"));
		StructuredContent->SetStringField(TEXT("blueprintPath"), ObjectPath);
		StructuredContent->SetNumberField(TEXT("graphCount"), GraphValues.Num());
		StructuredContent->SetNumberField(TEXT("nodeCount"), NodeCount);
		StructuredContent->SetBoolField(TEXT("includePins"), bIncludePins);
		StructuredContent->SetArrayField(TEXT("graphs"), GraphValues);
		return MakeExecutionResult(FString::Printf(TEXT("Listed %d Blueprint graph node(s) across %d graph(s)."), NodeCount, GraphValues.Num()), StructuredContent, false);
	}

	FUnrealMcpExecutionResult TracePinConnectionsTool(const FJsonObject& Arguments)
	{
		FString BlueprintPath;
		FString GraphName = TEXT("EventGraph");
		FString NodeGuid;
		FString PinName;
		Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetStringField(TEXT("nodeGuid"), NodeGuid);
		Arguments.TryGetStringField(TEXT("pinName"), PinName);

		if (BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide blueprintPath."), nullptr, true);
		}
		if (NodeGuid.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide nodeGuid."), nullptr, true);
		}

		UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
		if (!Graph)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		UEdGraphNode* Node = FindBlueprintNodeByGuid(Graph, NodeGuid);
		if (!Node)
		{
			return MakeExecutionResult(FString::Printf(TEXT("Node '%s' was not found in graph '%s'."), *NodeGuid, *Graph->GetName()), nullptr, true);
		}

		TArray<TSharedPtr<FJsonValue>> PinValues;
		if (PinName.TrimStartAndEnd().IsEmpty())
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					PinValues.Add(MakeShared<FJsonValueObject>(DescribeBlueprintPin(Pin)));
				}
			}
		}
		else
		{
			UEdGraphPin* Pin = FindBlueprintPinByName(Node, PinName);
			if (!Pin)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Pin '%s' was not found on node '%s'."), *PinName, *NodeGuid), nullptr, true);
			}
			PinValues.Add(MakeShared<FJsonValueObject>(DescribeBlueprintPin(Pin)));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("bp_trace_pin_connections"));
		StructuredContent->SetStringField(TEXT("blueprintPath"), ObjectPath);
		StructuredContent->SetStringField(TEXT("graph"), Graph->GetName());
		StructuredContent->SetObjectField(TEXT("node"), DescribeBlueprintNode(Node));
		StructuredContent->SetArrayField(TEXT("pins"), PinValues);
		return MakeExecutionResult(FString::Printf(TEXT("Traced %d pin(s) on node %s."), PinValues.Num(), *NodeGuid), StructuredContent, false);
	}

	bool TryExecuteBlueprintTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.bp_list_graph_nodes"))
		{
			OutResult = ListGraphNodesTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_trace_pin_connections"))
		{
			OutResult = TracePinConnectionsTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_add_variable"))
		{
			OutResult = AddVariableTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_add_function"))
		{
			OutResult = AddFunctionTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_add_event_node"))
		{
			OutResult = AddEventNodeTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_add_call_function_node"))
		{
			OutResult = AddCallFunctionNodeTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_add_branch_node"))
		{
			OutResult = AddBranchNodeTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_add_for_each_node"))
		{
			OutResult = AddForEachNodeTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_connect_pins"))
		{
			OutResult = ConnectPinsTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_set_pin_default"))
		{
			OutResult = SetPinDefaultTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_arrange_graph"))
		{
			OutResult = ArrangeGraphTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.bp_compile_save"))
		{
			OutResult = CompileSaveTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.compile_blueprint"))
		{
			OutResult = CompileBlueprintTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.compile_blueprints_in_path"))
		{
			OutResult = CompileBlueprintsInPathTool(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.create_blueprint_class"))
		{
			OutResult = CreateBlueprintClassTool(Arguments);
			return true;
		}

		return false;
	}
}

#undef LOCTEXT_NAMESPACE
