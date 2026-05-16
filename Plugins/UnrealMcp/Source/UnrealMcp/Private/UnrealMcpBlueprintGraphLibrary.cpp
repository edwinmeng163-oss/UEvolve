#include "UnrealMcpBlueprintGraphLibrary.h"

#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAxisEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "UnrealMcpBlueprintGraphLibrary"

namespace
{
	constexpr const TCHAR* GraphMutationBlockedReason = TEXT("graph mutation blocked while PIE is active");

	void ResetNodeOutputs(FString& OutNodeGuid, TArray<FString>& OutPinNames, FString& OutFailureReason)
	{
		OutNodeGuid.Reset();
		OutPinNames.Reset();
		OutFailureReason.Reset();
	}

	void ResetFailureReason(FString& OutFailureReason)
	{
		OutFailureReason.Reset();
	}

	bool IsPieActive(FString& OutFailureReason)
	{
		if (GEditor && GEditor->PlayWorld != nullptr)
		{
			OutFailureReason = GraphMutationBlockedReason;
			return true;
		}

		return false;
	}

	bool ValidateBlueprint(UBlueprint* Blueprint, FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return false;
		}

		return true;
	}

	UEdGraph* GetEventGraph(UBlueprint* Blueprint, FString& OutFailureReason)
	{
		if (!ValidateBlueprint(Blueprint, OutFailureReason))
		{
			return nullptr;
		}

		if (Blueprint->UbergraphPages.Num() > 0 && Blueprint->UbergraphPages[0])
		{
			return Blueprint->UbergraphPages[0];
		}

		if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
		{
			return EventGraph;
		}

		UEdGraph* NewEventGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			UEdGraphSchema_K2::GN_EventGraph,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!NewEventGraph)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create EventGraph for Blueprint '%s'."), *Blueprint->GetPathName());
			return nullptr;
		}

		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewEventGraph);
		return NewEventGraph;
	}

	const UEdGraphSchema_K2* GetK2Schema(UEdGraph* Graph, FString& OutFailureReason)
	{
		if (!Graph)
		{
			OutFailureReason = TEXT("Graph is null.");
			return nullptr;
		}

		const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(Graph->GetSchema());
		if (!K2Schema)
		{
			OutFailureReason = FString::Printf(TEXT("Graph '%s' does not use UEdGraphSchema_K2."), *Graph->GetName());
			return nullptr;
		}

		return K2Schema;
	}

	UClass* GetBlueprintCallingClass(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		if (Blueprint->SkeletonGeneratedClass)
		{
			return Blueprint->SkeletonGeneratedClass;
		}
		if (Blueprint->GeneratedClass)
		{
			return Blueprint->GeneratedClass;
		}

		return Blueprint->ParentClass;
	}

	UClass* ResolveClassPath(const FString& ClassPath)
	{
		const FString TrimmedPath = ClassPath.TrimStartAndEnd();
		if (TrimmedPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *TrimmedPath))
		{
			return LoadedClass;
		}

		if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *TrimmedPath))
		{
			return LoadedClass;
		}

		if (UObject* LoadedObject = LoadObject<UObject>(nullptr, *TrimmedPath))
		{
			if (UClass* LoadedClass = Cast<UClass>(LoadedObject))
			{
				return LoadedClass;
			}
			if (UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject))
			{
				if (LoadedBlueprint->SkeletonGeneratedClass)
				{
					return LoadedBlueprint->SkeletonGeneratedClass;
				}
				return LoadedBlueprint->GeneratedClass;
			}
		}

		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* CandidateClass = *ClassIt;
			if (!CandidateClass)
			{
				continue;
			}

			if (CandidateClass->GetPathName().Equals(TrimmedPath, ESearchCase::IgnoreCase)
				|| CandidateClass->GetFullName().Equals(TrimmedPath, ESearchCase::IgnoreCase)
				|| CandidateClass->GetName().Equals(TrimmedPath, ESearchCase::IgnoreCase))
			{
				return CandidateClass;
			}
		}

		return nullptr;
	}

	void AddCandidateClass(TArray<UClass*>& CandidateClasses, UClass* CandidateClass)
	{
		if (!CandidateClass)
		{
			return;
		}

		if (UClass* MostUpToDateClass = FBlueprintEditorUtils::GetMostUpToDateClass(CandidateClass))
		{
			CandidateClass = MostUpToDateClass;
		}

		CandidateClasses.AddUnique(CandidateClass);
	}

	UFunction* FindFunctionInCandidateClasses(const TArray<UClass*>& CandidateClasses, const FName FunctionName)
	{
		for (UClass* CandidateClass : CandidateClasses)
		{
			if (!CandidateClass)
			{
				continue;
			}

			if (UFunction* Function = CandidateClass->FindFunctionByName(FunctionName))
			{
				if (UFunction* MostUpToDateFunction = FBlueprintEditorUtils::GetMostUpToDateFunction(Function))
				{
					return MostUpToDateFunction;
				}
				return Function;
			}
		}

		return nullptr;
	}

	UFunction* ResolveFunction(UBlueprint* Blueprint, const FName FunctionName, const FString& OptionalContextClassPath, FString& OutFailureReason)
	{
		if (FunctionName.IsNone())
		{
			OutFailureReason = TEXT("FunctionName is None.");
			return nullptr;
		}

		TArray<UClass*> CandidateClasses;
		const FString TrimmedContextClassPath = OptionalContextClassPath.TrimStartAndEnd();
		if (!TrimmedContextClassPath.IsEmpty())
		{
			UClass* ContextClass = ResolveClassPath(TrimmedContextClassPath);
			if (!ContextClass)
			{
				OutFailureReason = FString::Printf(TEXT("Unable to resolve OptionalContextClassPath '%s'."), *TrimmedContextClassPath);
				return nullptr;
			}

			AddCandidateClass(CandidateClasses, ContextClass);
		}
		else
		{
			AddCandidateClass(CandidateClasses, Blueprint ? Blueprint->SkeletonGeneratedClass : nullptr);
			AddCandidateClass(CandidateClasses, Blueprint ? Blueprint->GeneratedClass : nullptr);
			AddCandidateClass(CandidateClasses, Blueprint ? Blueprint->ParentClass : nullptr);
		}

		if (UFunction* Function = FindFunctionInCandidateClasses(CandidateClasses, FunctionName))
		{
			return Function;
		}

		if (TrimmedContextClassPath.IsEmpty() && Blueprint)
		{
			bool bInvalidInterface = false;
			if (UFunction* InterfaceFunction = FBlueprintEditorUtils::FindFunctionInImplementedInterfaces(Blueprint, FunctionName, &bInvalidInterface, true))
			{
				return InterfaceFunction;
			}
		}

		FString CandidateClassList;
		for (UClass* CandidateClass : CandidateClasses)
		{
			if (!CandidateClass)
			{
				continue;
			}

			if (!CandidateClassList.IsEmpty())
			{
				CandidateClassList += TEXT(", ");
			}
			CandidateClassList += CandidateClass->GetPathName();
		}

		if (CandidateClassList.IsEmpty())
		{
			CandidateClassList = TEXT("<none>");
		}

		OutFailureReason = FString::Printf(
			TEXT("Function '%s' was not found in candidate classes: %s."),
			*FunctionName.ToString(),
			*CandidateClassList);
		return nullptr;
	}

	bool ValidateFunctionForGraph(UBlueprint* Blueprint, UEdGraph* Graph, UFunction* Function, FString& OutFailureReason)
	{
		if (!Function)
		{
			OutFailureReason = TEXT("Function is null.");
			return false;
		}

		const UEdGraphSchema_K2* K2Schema = GetK2Schema(Graph, OutFailureReason);
		if (!K2Schema)
		{
			return false;
		}

		UClass* CallingClass = GetBlueprintCallingClass(Blueprint);
		if (!CallingClass)
		{
			OutFailureReason = FString::Printf(TEXT("Blueprint '%s' has no generated, skeleton, or parent class for function validation."), Blueprint ? *Blueprint->GetPathName() : TEXT("<null>"));
			return false;
		}

		uint32 AllowedFunctionTypes = UEdGraphSchema_K2::FT_Pure | UEdGraphSchema_K2::FT_Const | UEdGraphSchema_K2::FT_Protected;
		if (K2Schema->DoesGraphSupportImpureFunctions(Graph))
		{
			AllowedFunctionTypes |= UEdGraphSchema_K2::FT_Imperative;
		}

		FText SchemaReason;
		if (!K2Schema->CanFunctionBeUsedInGraph(CallingClass, Function, Graph, AllowedFunctionTypes, false, &SchemaReason))
		{
			OutFailureReason = SchemaReason.IsEmpty()
				? FString::Printf(TEXT("Function '%s' cannot be used in graph '%s'."), *Function->GetPathName(), *Graph->GetName())
				: FString::Printf(TEXT("Function '%s' cannot be used in graph '%s': %s"), *Function->GetPathName(), *Graph->GetName(), *SchemaReason.ToString());
			return false;
		}

		return true;
	}

	void GatherPinNames(const UEdGraphNode* Node, TArray<FString>& OutPinNames)
	{
		OutPinNames.Reset();
		if (!Node)
		{
			return;
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				OutPinNames.Add(Pin->PinName.ToString());
			}
		}
	}

	bool FinalizeCreatedNode(UBlueprint* Blueprint, UEdGraph* Graph, UEdGraphNode* Node, bool bStructurallyModified, FString& OutNodeGuid, TArray<FString>& OutPinNames, FString& OutFailureReason)
	{
		if (!Graph)
		{
			OutFailureReason = TEXT("Graph is null after node creation.");
			return false;
		}
		if (!Node)
		{
			OutFailureReason = TEXT("Failed to create graph node.");
			return false;
		}

		if (!Node->NodeGuid.IsValid())
		{
			Node->CreateNewGuid();
		}

		Graph->NotifyGraphChanged();
		if (bStructurallyModified)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
		Blueprint->MarkPackageDirty();

		OutNodeGuid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
		GatherPinNames(Node, OutPinNames);
		OutFailureReason.Reset();
		return true;
	}

	bool AddCallFunctionNodeInternal(
		UBlueprint* Blueprint,
		const FName FunctionName,
		const FString& OptionalContextClassPath,
		const FVector2D Location,
		const FText& TransactionText,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason)
	{
		ResetNodeOutputs(OutNodeGuid, OutPinNames, OutFailureReason);
		if (IsPieActive(OutFailureReason) || !ValidateBlueprint(Blueprint, OutFailureReason))
		{
			return false;
		}

		UEdGraph* Graph = GetEventGraph(Blueprint, OutFailureReason);
		if (!Graph)
		{
			return false;
		}

		UFunction* Function = ResolveFunction(Blueprint, FunctionName, OptionalContextClassPath, OutFailureReason);
		if (!Function)
		{
			return false;
		}

		if (!ValidateFunctionForGraph(Blueprint, Graph, Function, OutFailureReason))
		{
			return false;
		}

		const FScopedTransaction Transaction(TransactionText);
		Graph->Modify();
		Blueprint->Modify();

		UK2Node_CallFunction* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
			Graph,
			Location,
			EK2NewNodeFlags::None,
			[Function](UK2Node_CallFunction* NewInstance)
			{
				NewInstance->SetFromFunction(Function);
			});

		return FinalizeCreatedNode(Blueprint, Graph, NewNode, false, OutNodeGuid, OutPinNames, OutFailureReason);
	}

	struct FLocatedGraphNode
	{
		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = nullptr;
	};

	bool FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuidString, FLocatedGraphNode& OutLocatedNode, FString& OutFailureReason)
	{
		OutLocatedNode = FLocatedGraphNode();

		const FString TrimmedGuid = NodeGuidString.TrimStartAndEnd();
		FGuid NodeGuid;
		if (!FGuid::Parse(TrimmedGuid, NodeGuid))
		{
			OutFailureReason = FString::Printf(TEXT("Invalid node GUID '%s'."), *NodeGuidString);
			return false;
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == NodeGuid)
				{
					OutLocatedNode.Graph = Graph;
					OutLocatedNode.Node = Node;
					return true;
				}
			}
		}

		OutFailureReason = FString::Printf(TEXT("Unable to find node GUID '%s' in Blueprint '%s'."), *TrimmedGuid, *Blueprint->GetPathName());
		return false;
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

	FString PinDirectionToString(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("<null>");
		}

		return Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	bool IsDisallowedConnectionResponse(const FPinConnectionResponse& Response)
	{
		return Response.Response == CONNECT_RESPONSE_DISALLOW;
	}
}

bool UUnrealMcpBlueprintGraphLibrary::AddCallFunctionNode(
	UBlueprint* Blueprint,
	FName FunctionName,
	const FString& OptionalContextClassPath,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	return AddCallFunctionNodeInternal(
		Blueprint,
		FunctionName,
		OptionalContextClassPath,
		Location,
		LOCTEXT("AddCallFunctionNode", "Unreal MCP Add Call Function Node"),
		OutNodeGuid,
		OutPinNames,
		OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::AddMovementInputCallNode(
	UBlueprint* Blueprint,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	return AddCallFunctionNodeInternal(
		Blueprint,
		GET_FUNCTION_NAME_CHECKED(APawn, AddMovementInput),
		APawn::StaticClass()->GetPathName(),
		Location,
		LOCTEXT("AddMovementInputCallNode", "Unreal MCP Add AddMovementInput Call Node"),
		OutNodeGuid,
		OutPinNames,
		OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::AddControllerYawInputCallNode(
	UBlueprint* Blueprint,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	return AddCallFunctionNodeInternal(
		Blueprint,
		GET_FUNCTION_NAME_CHECKED(APawn, AddControllerYawInput),
		APawn::StaticClass()->GetPathName(),
		Location,
		LOCTEXT("AddControllerYawInputCallNode", "Unreal MCP Add AddControllerYawInput Call Node"),
		OutNodeGuid,
		OutPinNames,
		OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::AddControllerPitchInputCallNode(
	UBlueprint* Blueprint,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	return AddCallFunctionNodeInternal(
		Blueprint,
		GET_FUNCTION_NAME_CHECKED(APawn, AddControllerPitchInput),
		APawn::StaticClass()->GetPathName(),
		Location,
		LOCTEXT("AddControllerPitchInputCallNode", "Unreal MCP Add AddControllerPitchInput Call Node"),
		OutNodeGuid,
		OutPinNames,
		OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::AddInputAxisEventNode(
	UBlueprint* Blueprint,
	FName AxisName,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	ResetNodeOutputs(OutNodeGuid, OutPinNames, OutFailureReason);
	if (IsPieActive(OutFailureReason) || !ValidateBlueprint(Blueprint, OutFailureReason))
	{
		return false;
	}
	if (AxisName.IsNone())
	{
		OutFailureReason = TEXT("AxisName is None.");
		return false;
	}

	UEdGraph* Graph = GetEventGraph(Blueprint, OutFailureReason);
	if (!Graph)
	{
		return false;
	}
	if (!GetK2Schema(Graph, OutFailureReason))
	{
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddInputAxisEventNode", "Unreal MCP Add Input Axis Event Node"));
	Graph->Modify();
	Blueprint->Modify();

	UK2Node_InputAxisEvent* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_InputAxisEvent>(
		Graph,
		Location,
		EK2NewNodeFlags::None,
		[AxisName](UK2Node_InputAxisEvent* NewInstance)
		{
			NewInstance->Initialize(AxisName);
		});

	return FinalizeCreatedNode(Blueprint, Graph, NewNode, true, OutNodeGuid, OutPinNames, OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::AddGetActorForwardVectorNode(
	UBlueprint* Blueprint,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	return AddCallFunctionNodeInternal(
		Blueprint,
		GET_FUNCTION_NAME_CHECKED(AActor, GetActorForwardVector),
		AActor::StaticClass()->GetPathName(),
		Location,
		LOCTEXT("AddGetActorForwardVectorNode", "Unreal MCP Add GetActorForwardVector Node"),
		OutNodeGuid,
		OutPinNames,
		OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::AddGetActorRightVectorNode(
	UBlueprint* Blueprint,
	FVector2D Location,
	FString& OutNodeGuid,
	TArray<FString>& OutPinNames,
	FString& OutFailureReason)
{
	return AddCallFunctionNodeInternal(
		Blueprint,
		GET_FUNCTION_NAME_CHECKED(AActor, GetActorRightVector),
		AActor::StaticClass()->GetPathName(),
		Location,
		LOCTEXT("AddGetActorRightVectorNode", "Unreal MCP Add GetActorRightVector Node"),
		OutNodeGuid,
		OutPinNames,
		OutFailureReason);
}

bool UUnrealMcpBlueprintGraphLibrary::ConnectPinsByGuid(
	UBlueprint* Blueprint,
	const FString& SourceNodeGuid,
	const FString& SourcePinName,
	const FString& TargetNodeGuid,
	const FString& TargetPinName,
	FString& OutFailureReason)
{
	ResetFailureReason(OutFailureReason);
	if (IsPieActive(OutFailureReason) || !ValidateBlueprint(Blueprint, OutFailureReason))
	{
		return false;
	}

	FLocatedGraphNode SourceLocatedNode;
	if (!FindNodeByGuid(Blueprint, SourceNodeGuid, SourceLocatedNode, OutFailureReason))
	{
		return false;
	}

	FLocatedGraphNode TargetLocatedNode;
	if (!FindNodeByGuid(Blueprint, TargetNodeGuid, TargetLocatedNode, OutFailureReason))
	{
		return false;
	}

	if (SourceLocatedNode.Graph != TargetLocatedNode.Graph)
	{
		OutFailureReason = FString::Printf(
			TEXT("Cannot connect pins across different graphs: source node '%s' is in '%s', target node '%s' is in '%s'."),
			*SourceNodeGuid,
			SourceLocatedNode.Graph ? *SourceLocatedNode.Graph->GetName() : TEXT("<null>"),
			*TargetNodeGuid,
			TargetLocatedNode.Graph ? *TargetLocatedNode.Graph->GetName() : TEXT("<null>"));
		return false;
	}

	UEdGraph* Graph = SourceLocatedNode.Graph;
	const UEdGraphSchema_K2* K2Schema = GetK2Schema(Graph, OutFailureReason);
	if (!K2Schema)
	{
		return false;
	}

	UEdGraphPin* SourcePin = FindPinByName(SourceLocatedNode.Node, SourcePinName);
	if (!SourcePin)
	{
		OutFailureReason = FString::Printf(TEXT("Unable to find source pin '%s' on node '%s'."), *SourcePinName, *SourceNodeGuid);
		return false;
	}

	UEdGraphPin* TargetPin = FindPinByName(TargetLocatedNode.Node, TargetPinName);
	if (!TargetPin)
	{
		OutFailureReason = FString::Printf(TEXT("Unable to find target pin '%s' on node '%s'."), *TargetPinName, *TargetNodeGuid);
		return false;
	}

	if (SourcePin == TargetPin)
	{
		OutFailureReason = TEXT("Cannot connect a pin to itself.");
		return false;
	}
	if (SourcePin->Direction != EGPD_Output)
	{
		OutFailureReason = FString::Printf(TEXT("Wrong direction: source pin '%s.%s' is %s, expected output."), *SourceNodeGuid, *SourcePinName, *PinDirectionToString(SourcePin));
		return false;
	}
	if (TargetPin->Direction != EGPD_Input)
	{
		OutFailureReason = FString::Printf(TEXT("Wrong direction: target pin '%s.%s' is %s, expected input."), *TargetNodeGuid, *TargetPinName, *PinDirectionToString(TargetPin));
		return false;
	}

	const FPinConnectionResponse ConnectionResponse = K2Schema->CanCreateConnection(SourcePin, TargetPin);
	if (IsDisallowedConnectionResponse(ConnectionResponse))
	{
		const FString SchemaMessage = ConnectionResponse.Message.ToString();
		OutFailureReason = SchemaMessage.IsEmpty()
			? FString::Printf(TEXT("K2 schema rejected connection %s.%s -> %s.%s."), *SourceNodeGuid, *SourcePinName, *TargetNodeGuid, *TargetPinName)
			: FString::Printf(TEXT("K2 schema rejected connection %s.%s -> %s.%s: %s"), *SourceNodeGuid, *SourcePinName, *TargetNodeGuid, *TargetPinName, *SchemaMessage);
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("ConnectPinsByGuid", "Unreal MCP Connect Pins By GUID"));
	Graph->Modify();
	SourceLocatedNode.Node->Modify();
	TargetLocatedNode.Node->Modify();

	if (!K2Schema->TryCreateConnection(SourcePin, TargetPin))
	{
		OutFailureReason = FString::Printf(TEXT("K2 schema failed to create connection %s.%s -> %s.%s."), *SourceNodeGuid, *SourcePinName, *TargetNodeGuid, *TargetPinName);
		return false;
	}

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->MarkPackageDirty();
	OutFailureReason.Reset();
	return true;
}

bool UUnrealMcpBlueprintGraphLibrary::CompileBlueprint(UBlueprint* Blueprint, FString& OutFailureReason)
{
	ResetFailureReason(OutFailureReason);
	if (IsPieActive(OutFailureReason) || !ValidateBlueprint(Blueprint, OutFailureReason))
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error)
	{
		OutFailureReason = FString::Printf(TEXT("Blueprint compile failed for '%s'."), *Blueprint->GetPathName());
		return false;
	}

	OutFailureReason.Reset();
	return true;
}

#undef LOCTEXT_NAMESPACE
