#include "UnrealMcpToolOutcomeVerifiers.h"

#include "UnrealMcpModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"

namespace UnrealMcp
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> BlueprintMakeStringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TSharedPtr<FJsonObject> BlueprintMakeVerifierResult(const FString& ToolName)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("toolName"), ToolName);
			Object->SetStringField(TEXT("category"), TEXT("blueprint"));
			Object->SetStringField(TEXT("checkLevel"), TEXT("tool_specific_state"));
			Object->SetBoolField(TEXT("toolSpecificVerifierAvailable"), true);
			return Object;
		}

		void BlueprintFinishVerifier(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Evidence,
			const TArray<FString>& Failures)
		{
			Object->SetBoolField(TEXT("verified"), Failures.Num() == 0);
			Object->SetArrayField(TEXT("evidence"), BlueprintMakeStringValues(Evidence));
			Object->SetArrayField(TEXT("failures"), BlueprintMakeStringValues(Failures));
			Object->SetStringField(TEXT("summary"), Failures.Num() == 0
				? TEXT("Blueprint state verifier confirmed the requested asset, graph, node, pin, or variable state.")
				: TEXT("Blueprint state verifier found mismatches; inspect failures for details."));
		}

		FString BlueprintAddObjectNameCandidate(const FString& Path)
		{
			if (!Path.StartsWith(TEXT("/")) || Path.Contains(TEXT(".")))
			{
				return Path;
			}

			const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
			return AssetName.IsEmpty() ? Path : FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
		}

		UObject* BlueprintLoadObjectFromPathCandidates(const FString& RawPath)
		{
			const FString TrimmedPath = RawPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				return nullptr;
			}

			TArray<FString> Candidates;
			Candidates.Add(TrimmedPath);
			Candidates.Add(BlueprintAddObjectNameCandidate(TrimmedPath));

			for (const FString& Candidate : Candidates)
			{
				if (UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Candidate))
				{
					return Object;
				}
			}
			return nullptr;
		}

		UBlueprint* LoadBlueprintFromPaths(const FJsonObject& Arguments, const FUnrealMcpExecutionResult& Result, FString& OutPath)
		{
			TArray<FString> Candidates;
			if (Result.StructuredContent.IsValid())
			{
				FString Value;
				if (Result.StructuredContent->TryGetStringField(TEXT("blueprint"), Value)) { Candidates.Add(Value); }
				if (Result.StructuredContent->TryGetStringField(TEXT("objectPath"), Value)) { Candidates.Add(Value); }
			}

			FString ArgValue;
			if (Arguments.TryGetStringField(TEXT("blueprintPath"), ArgValue)) { Candidates.Add(ArgValue); }
			if (Arguments.TryGetStringField(TEXT("assetPath"), ArgValue)) { Candidates.Add(ArgValue); }
			if (Arguments.TryGetStringField(TEXT("path"), ArgValue)) { Candidates.Add(ArgValue); }

			for (const FString& Candidate : Candidates)
			{
				if (UObject* Object = BlueprintLoadObjectFromPathCandidates(Candidate))
				{
					if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
					{
						OutPath = Blueprint->GetPathName();
						return Blueprint;
					}
					if (UClass* Class = Cast<UClass>(Object))
					{
						if (UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
						{
							OutPath = Blueprint->GetPathName();
							return Blueprint;
						}
					}
				}
			}
			return nullptr;
		}

		UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					return Graph;
				}
			}
			return nullptr;
		}

		UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuid)
		{
			if (!Graph || NodeGuid.TrimStartAndEnd().IsEmpty())
			{
				return nullptr;
			}

			const FString TrimmedGuid = NodeGuid.TrimStartAndEnd();
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(TrimmedGuid, ESearchCase::IgnoreCase)
					|| Node->NodeGuid.ToString(EGuidFormats::Digits).Equals(TrimmedGuid, ESearchCase::IgnoreCase))
				{
					return Node;
				}
			}
			return nullptr;
		}

		UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
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

		FString GetGraphName(const FJsonObject& Arguments, const FUnrealMcpExecutionResult& Result)
		{
			FString GraphName;
			if (Result.StructuredContent.IsValid())
			{
				Result.StructuredContent->TryGetStringField(TEXT("graph"), GraphName);
			}
			if (GraphName.TrimStartAndEnd().IsEmpty())
			{
				Arguments.TryGetStringField(TEXT("graphName"), GraphName);
			}
			return GraphName.TrimStartAndEnd().IsEmpty() ? TEXT("EventGraph") : GraphName;
		}

		FString GetNodeGuidFromResult(const FUnrealMcpExecutionResult& Result)
		{
			if (!Result.StructuredContent.IsValid())
			{
				return FString();
			}

			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (Result.StructuredContent->TryGetObjectField(TEXT("node"), NodeObject) && NodeObject && (*NodeObject).IsValid())
			{
				FString NodeGuid;
				(*NodeObject)->TryGetStringField(TEXT("nodeGuid"), NodeGuid);
				return NodeGuid;
			}
			return FString();
		}

		bool HasVariable(UBlueprint* Blueprint, const FString& VariableName)
		{
			if (!Blueprint)
			{
				return false;
			}
			const FString TrimmedVariableName = VariableName.TrimStartAndEnd();
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				if (Variable.VarName.ToString().Equals(TrimmedVariableName, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}
	}

	TSharedPtr<FJsonObject> BuildBlueprintToolPreflight(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TSharedPtr<FJsonObject>& GenericPreflight)
	{
		const bool bIsBlueprintTool = ToolName.StartsWith(TEXT("unreal.bp_"))
			|| ToolName == TEXT("unreal.create_blueprint_class")
			|| ToolName == TEXT("unreal.compile_blueprint")
			|| ToolName == TEXT("unreal.compile_blueprints_in_path");
		if (!bIsBlueprintTool || !GenericPreflight.IsValid())
		{
			return nullptr;
		}

		TArray<FString> Evidence;
		TArray<FString> Failures;
		GenericPreflight->SetBoolField(TEXT("toolSpecificPreflightAvailable"), true);
		GenericPreflight->SetStringField(TEXT("category"), TEXT("blueprint"));
		GenericPreflight->SetStringField(TEXT("checkLevel"), TEXT("tool_specific_preflight"));

		if (ToolName == TEXT("unreal.create_blueprint_class"))
		{
			FString AssetPath;
			Arguments.TryGetStringField(TEXT("assetPath"), AssetPath);
			if (AssetPath.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(TEXT("assetPath is required to create a Blueprint."));
			}
			else if (BlueprintLoadObjectFromPathCandidates(AssetPath))
			{
				Evidence.Add(FString::Printf(TEXT("Target Blueprint asset already exists: %s."), *AssetPath));
			}
			else
			{
				Evidence.Add(FString::Printf(TEXT("Target Blueprint asset does not exist yet and can be created: %s."), *AssetPath));
			}
		}
		else if (ToolName == TEXT("unreal.compile_blueprints_in_path"))
		{
			FString Path;
			Arguments.TryGetStringField(TEXT("path"), Path);
			if (Path.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(TEXT("path is required to compile Blueprints in a folder."));
			}
			else
			{
				Evidence.Add(FString::Printf(TEXT("Blueprint folder argument is present: %s."), *Path));
			}
		}
		else
		{
			FString BlueprintPath;
			FUnrealMcpExecutionResult EmptyResult;
			UBlueprint* Blueprint = LoadBlueprintFromPaths(Arguments, EmptyResult, BlueprintPath);
			if (!Blueprint)
			{
				Failures.Add(TEXT("Target Blueprint could not be loaded from blueprintPath, assetPath, or path."));
			}
			else
			{
				Evidence.Add(FString::Printf(TEXT("Target Blueprint is loadable before execution: %s."), *BlueprintPath));
				if (ToolName != TEXT("unreal.bp_add_function")
					&& ToolName != TEXT("unreal.bp_compile_save")
					&& ToolName != TEXT("unreal.compile_blueprint"))
				{
					const FString GraphName = GetGraphName(Arguments, EmptyResult);
					if (FindGraph(Blueprint, GraphName))
					{
						Evidence.Add(FString::Printf(TEXT("Target graph exists before execution: %s."), *GraphName));
					}
					else
					{
						Failures.Add(FString::Printf(TEXT("Target graph does not exist before execution: %s."), *GraphName));
					}
				}
			}
		}

		GenericPreflight->SetBoolField(TEXT("ready"), Failures.Num() == 0);
		GenericPreflight->SetArrayField(TEXT("evidence"), BlueprintMakeStringValues(Evidence));
		GenericPreflight->SetArrayField(TEXT("failures"), BlueprintMakeStringValues(Failures));
		GenericPreflight->SetStringField(TEXT("summary"), Failures.Num() == 0
			? TEXT("Blueprint preflight confirmed required target state or creation arguments.")
			: TEXT("Blueprint preflight found missing target state; inspect failures before applying."));
		return GenericPreflight;
	}

	TSharedPtr<FJsonObject> VerifyBlueprintToolOutcome(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const FUnrealMcpExecutionResult& Result)
	{
		const bool bIsBlueprintTool = ToolName.StartsWith(TEXT("unreal.bp_"))
			|| ToolName == TEXT("unreal.create_blueprint_class")
			|| ToolName == TEXT("unreal.compile_blueprint")
			|| ToolName == TEXT("unreal.compile_blueprints_in_path");
		if (!bIsBlueprintTool)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Verifier = BlueprintMakeVerifierResult(ToolName);
		Verifier->SetBoolField(TEXT("toolReturnedError"), Result.bIsError);
		Verifier->SetBoolField(TEXT("genericResultSucceeded"), !Result.bIsError);
		TArray<FString> Evidence;
		TArray<FString> Failures;

		if (Result.bIsError)
		{
			Failures.Add(TEXT("Tool returned an error; Blueprint success state was not verified."));
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		FString BlueprintPath;
		UBlueprint* Blueprint = LoadBlueprintFromPaths(Arguments, Result, BlueprintPath);
		if (!Blueprint)
		{
			Failures.Add(TEXT("Unable to load Blueprint from result or arguments."));
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}
		Evidence.Add(FString::Printf(TEXT("Loaded Blueprint %s."), *BlueprintPath));

		if (ToolName == TEXT("unreal.create_blueprint_class"))
		{
			Evidence.Add(Blueprint->GeneratedClass
				? FString::Printf(TEXT("Generated class exists: %s."), *Blueprint->GeneratedClass->GetPathName())
				: TEXT("Blueprint asset exists; generated class is not currently available."));
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.bp_add_variable"))
		{
			FString VariableName;
			if (!Result.StructuredContent->TryGetStringField(TEXT("variableName"), VariableName))
			{
				Arguments.TryGetStringField(TEXT("name"), VariableName);
			}
			if (HasVariable(Blueprint, VariableName))
			{
				Evidence.Add(FString::Printf(TEXT("Variable '%s' exists in Blueprint member variables."), *VariableName));
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("Variable '%s' was not found in Blueprint member variables."), *VariableName));
			}
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.bp_compile_save") || ToolName == TEXT("unreal.compile_blueprint"))
		{
			const bool bCompileSucceeded = Blueprint->Status != BS_Error;
			if (bCompileSucceeded)
			{
				Evidence.Add(FString::Printf(TEXT("Blueprint status is %s."), *StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status))));
			}
			else
			{
				Failures.Add(TEXT("Blueprint status is BS_Error after compile."));
			}
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		const FString GraphName = GetGraphName(Arguments, Result);
		UEdGraph* Graph = FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Failures.Add(FString::Printf(TEXT("Graph '%s' was not found."), *GraphName));
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}
		Evidence.Add(FString::Printf(TEXT("Graph '%s' exists."), *Graph->GetName()));

		if (ToolName == TEXT("unreal.bp_add_function"))
		{
			Evidence.Add(TEXT("Function graph verification succeeded."));
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.bp_connect_pins"))
		{
			FString FromNodeGuid;
			FString FromPinName;
			FString ToNodeGuid;
			FString ToPinName;
			Arguments.TryGetStringField(TEXT("fromNodeGuid"), FromNodeGuid);
			Arguments.TryGetStringField(TEXT("fromPin"), FromPinName);
			Arguments.TryGetStringField(TEXT("toNodeGuid"), ToNodeGuid);
			Arguments.TryGetStringField(TEXT("toPin"), ToPinName);

			UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeGuid);
			UEdGraphNode* ToNode = FindNodeByGuid(Graph, ToNodeGuid);
			UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
			UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
			if (FromPin && ToPin && FromPin->LinkedTo.Contains(ToPin))
			{
				Evidence.Add(TEXT("Source pin is linked to target pin."));
			}
			else
			{
				Failures.Add(TEXT("Expected pin connection was not found after tool execution."));
			}
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.bp_set_pin_default"))
		{
			FString NodeGuid;
			FString PinName;
			FString ExpectedValue;
			Arguments.TryGetStringField(TEXT("nodeGuid"), NodeGuid);
			Arguments.TryGetStringField(TEXT("pinName"), PinName);
			Arguments.TryGetStringField(TEXT("value"), ExpectedValue);
			UEdGraphPin* Pin = FindPinByName(FindNodeByGuid(Graph, NodeGuid), PinName);
			if (Pin && Pin->DefaultValue.Equals(ExpectedValue, ESearchCase::CaseSensitive))
			{
				Evidence.Add(FString::Printf(TEXT("Pin default value is '%s'."), *Pin->DefaultValue));
			}
			else
			{
				Failures.Add(TEXT("Expected pin default value was not found after tool execution."));
			}
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.bp_arrange_graph"))
		{
			double ArrangedCount = 0.0;
			if (Result.StructuredContent->TryGetNumberField(TEXT("arrangedCount"), ArrangedCount))
			{
				Evidence.Add(FString::Printf(TEXT("Graph exists with %d nodes; tool reported %d arranged nodes."), Graph->Nodes.Num(), static_cast<int32>(ArrangedCount)));
			}
			else
			{
				Evidence.Add(FString::Printf(TEXT("Graph exists with %d nodes."), Graph->Nodes.Num()));
			}
			BlueprintFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		const FString NodeGuid = GetNodeGuidFromResult(Result);
		if (!NodeGuid.IsEmpty())
		{
			if (FindNodeByGuid(Graph, NodeGuid))
			{
				Evidence.Add(FString::Printf(TEXT("Node %s exists in graph '%s'."), *NodeGuid, *Graph->GetName()));
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("Node %s was not found in graph '%s'."), *NodeGuid, *Graph->GetName()));
			}
		}
		else
		{
			Evidence.Add(TEXT("No node GUID was returned; verified Blueprint and graph existence only."));
		}

		BlueprintFinishVerifier(Verifier, Evidence, Failures);
		return Verifier;
	}
}
