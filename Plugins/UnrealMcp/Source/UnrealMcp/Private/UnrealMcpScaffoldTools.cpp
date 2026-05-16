#include "UnrealMcpScaffoldTools.h"

#include "UnrealMcpMemoryTools.h"
#include "UnrealMcpModule.h"
#include "UnrealMcpSharedPathResolver.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorScriptingHelpers.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace UnrealMcp
{
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	UClass* ResolveClassPath(const FString& ClassPath, UEditorAssetSubsystem* EditorAssetSubsystem);
	UBlueprint* LoadBlueprintAsset(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& BlueprintPath, FString& OutObjectPath, FString& OutFailureReason);
	UWidgetBlueprint* LoadOrCreateWidgetBlueprintAsset(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& WidgetBlueprintPath, FString& OutObjectPath, bool& bOutCreated, FString& OutFailureReason);
	bool BuildShopWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason);
	bool BuildResultWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason);
	void MarkWidgetBlueprintModified(UWidgetBlueprint* WidgetBlueprint, bool bStructurallyModified);
	void AddNextStep(TArray<TSharedPtr<FJsonValue>>& NextSteps, const FString& Text);
	FUnrealMcpExecutionResult ScaffoldMcpTool(const FJsonObject& Arguments);
	FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject);

	FString NormalizeScaffoldRootPath(const FString& RequestedRootPath)
	{
		FString RootPath = RequestedRootPath.TrimStartAndEnd();
		if (RootPath.IsEmpty())
		{
			RootPath = TEXT("/Game/MCPDemo");
		}
		while (RootPath.EndsWith(TEXT("/")))
		{
			RootPath.LeftChopInline(1);
		}
		return RootPath;
	}

	FString MakeScaffoldPath(const FString& RootPath, const FString& RelativePath)
	{
		FString CleanRelative = RelativePath.TrimStartAndEnd();
		while (CleanRelative.StartsWith(TEXT("/")))
		{
			CleanRelative.RightChopInline(1);
		}
		return RootPath / CleanRelative;
	}

	void AddScaffoldRecord(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Kind, const FString& Path, bool bCreated)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), Kind);
		Object->SetStringField(TEXT("path"), Path);
		Object->SetBoolField(TEXT("created"), bCreated);
		Array.Add(MakeShared<FJsonValueObject>(Object));
	}

	void AddScaffoldNamedRecord(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Kind, const FString& OwnerPath, const FString& Name, bool bCreated)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), Kind);
		Object->SetStringField(TEXT("owner"), OwnerPath);
		Object->SetStringField(TEXT("name"), Name);
		Object->SetBoolField(TEXT("created"), bCreated);
		Array.Add(MakeShared<FJsonValueObject>(Object));
	}

	FString NormalizeRecipeName(FString RecipeName)
	{
		RecipeName = RecipeName.TrimStartAndEnd().ToLower();
		RecipeName.ReplaceInline(TEXT("-"), TEXT("_"));
		RecipeName.ReplaceInline(TEXT(" "), TEXT("_"));
		if (RecipeName.IsEmpty() || RecipeName == TEXT("first_person") || RecipeName == TEXT("first_person_ground") || RecipeName == TEXT("fps"))
		{
			return TEXT("first_person_ground_character");
		}
		if (RecipeName == TEXT("hud") || RecipeName == TEXT("widget") || RecipeName == TEXT("widget_hud") || RecipeName == TEXT("ui_hud"))
		{
			return TEXT("widget_hud");
		}
		if (RecipeName == TEXT("mcp_pipeline") || RecipeName == TEXT("mcp_self_extension") || RecipeName == TEXT("self_extension_pipeline"))
		{
			return TEXT("mcp_self_extension_pipeline");
		}
		return RecipeName;
	}

	TArray<TSharedPtr<FJsonValue>> MakeRecipeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	void AddRecipeStep(
		TArray<TSharedPtr<FJsonValue>>& Steps,
		const FString& Name,
		const FString& Goal,
		const TArray<FString>& Tools,
		const TArray<FString>& Verification,
		const FString& Notes)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("name"), Name);
		Step->SetStringField(TEXT("goal"), Goal);
		Step->SetArrayField(TEXT("tools"), MakeRecipeStringArray(Tools));
		Step->SetArrayField(TEXT("verification"), MakeRecipeStringArray(Verification));
		Step->SetStringField(TEXT("notes"), Notes);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
	}

	void AddRecipeToolCall(
		TArray<TSharedPtr<FJsonValue>>& ToolCalls,
		const FString& ToolName,
		const FString& Purpose,
		const FString& ExampleArgumentsJson)
	{
		TSharedPtr<FJsonObject> ToolCall = MakeShared<FJsonObject>();
		ToolCall->SetStringField(TEXT("toolName"), ToolName);
		ToolCall->SetStringField(TEXT("purpose"), Purpose);
		ToolCall->SetStringField(TEXT("exampleArgumentsJson"), ExampleArgumentsJson);
		ToolCalls.Add(MakeShared<FJsonValueObject>(ToolCall));
	}

	bool EnsureScaffoldDirectory(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& DirectoryPath, TArray<TSharedPtr<FJsonValue>>& Directories, FString& OutFailureReason)
	{
		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return false;
		}
		if (!DirectoryPath.StartsWith(TEXT("/Game")))
		{
			OutFailureReason = FString::Printf(TEXT("Scaffold directory '%s' must be under /Game."), *DirectoryPath);
			return false;
		}

		const bool bExists = EditorAssetSubsystem->DoesDirectoryExist(DirectoryPath);
		if (!bExists && !EditorAssetSubsystem->MakeDirectory(DirectoryPath))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create directory '%s'."), *DirectoryPath);
			return false;
		}
		AddScaffoldRecord(Directories, TEXT("directory"), DirectoryPath, !bExists);
		return true;
	}

	UBlueprint* LoadOrCreateBlueprintScaffoldAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& AssetPath,
		const FString& ParentClassPath,
		FString& OutObjectPath,
		bool& bOutCreated,
		TArray<TSharedPtr<FJsonValue>>& Assets,
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

		const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AssetPath, OutFailureReason);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		OutObjectPath = ObjectPath;

		if (EditorAssetSubsystem->DoesAssetExist(ObjectPath))
		{
			UBlueprint* ExistingBlueprint = LoadBlueprintAsset(EditorAssetSubsystem, ObjectPath, OutObjectPath, OutFailureReason);
			if (!ExistingBlueprint)
			{
				return nullptr;
			}
			AddScaffoldRecord(Assets, TEXT("blueprint"), OutObjectPath, false);
			return ExistingBlueprint;
		}

		UClass* ParentClass = ResolveClassPath(ParentClassPath, EditorAssetSubsystem);
		if (!ParentClass)
		{
			OutFailureReason = FString::Printf(TEXT("Unable to resolve parent class '%s'."), *ParentClassPath);
			return nullptr;
		}
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutFailureReason = FString::Printf(TEXT("Cannot create a Blueprint from class '%s'."), *ParentClass->GetPathName());
			return nullptr;
		}
		if (!EditorScriptingHelpers::IsAValidPathForCreateNewAsset(ObjectPath, OutFailureReason))
		{
			OutFailureReason = FString::Printf(TEXT("Invalid asset path '%s': %s"), *ObjectPath, *OutFailureReason);
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
			FName(TEXT("UnrealMcpScaffold")));

		if (!NewBlueprint)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create Blueprint '%s'."), *ObjectPath);
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(NewBlueprint);
		Package->MarkPackageDirty();
		bOutCreated = true;
		AddScaffoldRecord(Assets, TEXT("blueprint"), ObjectPath, true);
		return NewBlueprint;
	}

	FEdGraphPinType MakeScaffoldPinType(const FName& PinCategory, EPinContainerType ContainerType = EPinContainerType::None, UObject* SubCategoryObject = nullptr)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = PinCategory;
		PinType.ContainerType = ContainerType;
		PinType.PinSubCategoryObject = SubCategoryObject;
		if (PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		return PinType;
	}

	bool EnsureBlueprintScaffoldVariable(
		UBlueprint* Blueprint,
		const FName& VariableName,
		const FEdGraphPinType& PinType,
		const FString& DefaultValue,
		TArray<TSharedPtr<FJsonValue>>& Variables,
		FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return false;
		}
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) != INDEX_NONE)
		{
			AddScaffoldNamedRecord(Variables, TEXT("variable"), Blueprint->GetPathName(), VariableName.ToString(), false);
			return true;
		}

		Blueprint->Modify();
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, DefaultValue))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to add variable '%s' to %s."), *VariableName.ToString(), *Blueprint->GetPathName());
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();
		AddScaffoldNamedRecord(Variables, TEXT("variable"), Blueprint->GetPathName(), VariableName.ToString(), true);
		return true;
	}

	bool EnsureBlueprintScaffoldFunction(UBlueprint* Blueprint, const FString& FunctionName, TArray<TSharedPtr<FJsonValue>>& Functions, FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return false;
		}

		const FString CleanFunctionName = FunctionName.TrimStartAndEnd();
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(CleanFunctionName, ESearchCase::IgnoreCase))
			{
				AddScaffoldNamedRecord(Functions, TEXT("function"), Blueprint->GetPathName(), Graph->GetName(), false);
				return true;
			}
		}

		Blueprint->Modify();
		UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FName(*CleanFunctionName),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!FunctionGraph)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create function graph '%s' in %s."), *CleanFunctionName, *Blueprint->GetPathName());
			return false;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();
		AddScaffoldNamedRecord(Functions, TEXT("function"), Blueprint->GetPathName(), FunctionGraph->GetName(), true);
		return true;
	}

	void SetBlueprintClassDefault(UBlueprint* Blueprint, const FName& PropertyName, UClass* ClassValue, TArray<TSharedPtr<FJsonValue>>& Defaults)
	{
		if (!Blueprint || !Blueprint->GeneratedClass || !ClassValue)
		{
			return;
		}
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		FClassProperty* ClassProperty = CDO ? FindFProperty<FClassProperty>(CDO->GetClass(), PropertyName) : nullptr;
		if (!CDO || !ClassProperty)
		{
			return;
		}

		CDO->Modify();
		CDO->PreEditChange(ClassProperty);
		ClassProperty->SetPropertyValue_InContainer(CDO, ClassValue);
		FPropertyChangedEvent PropertyChangedEvent(ClassProperty, EPropertyChangeType::ValueSet);
		CDO->PostEditChangeProperty(PropertyChangedEvent);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Object->SetStringField(TEXT("property"), PropertyName.ToString());
		Object->SetStringField(TEXT("class"), ClassValue->GetPathName());
		Defaults.Add(MakeShared<FJsonValueObject>(Object));
	}

	void FinalizeScaffoldBlueprint(UBlueprint* Blueprint, UEditorAssetSubsystem* EditorAssetSubsystem, bool bCompile, bool bSavePackage, TArray<TSharedPtr<FJsonValue>>& Finalized)
	{
		if (!Blueprint)
		{
			return;
		}

		bool bCompileSucceeded = true;
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			bCompileSucceeded = Blueprint->Status != BS_Error;
		}

		bool bSaved = false;
		if (bSavePackage && EditorAssetSubsystem)
		{
			bSaved = EditorAssetSubsystem->SaveLoadedAsset(Blueprint, false);
		}

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Object->SetBoolField(TEXT("compiled"), bCompile);
		Object->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
		Object->SetBoolField(TEXT("saved"), bSaved);
		Finalized.Add(MakeShared<FJsonValueObject>(Object));
	}

	TSharedPtr<FJsonObject> MakeScaffoldStructuredContent(
		const FString& Action,
		const FString& RootPath,
		const TArray<TSharedPtr<FJsonValue>>& Directories,
		const TArray<TSharedPtr<FJsonValue>>& Assets,
		const TArray<TSharedPtr<FJsonValue>>& Variables,
		const TArray<TSharedPtr<FJsonValue>>& Functions,
		const TArray<TSharedPtr<FJsonValue>>& Defaults,
		const TArray<TSharedPtr<FJsonValue>>& Finalized,
		const TArray<TSharedPtr<FJsonValue>>& NextSteps)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), Action);
		StructuredContent->SetStringField(TEXT("rootPath"), RootPath);
		StructuredContent->SetArrayField(TEXT("directories"), Directories);
		StructuredContent->SetArrayField(TEXT("assets"), Assets);
		StructuredContent->SetArrayField(TEXT("variables"), Variables);
		StructuredContent->SetArrayField(TEXT("functions"), Functions);
		StructuredContent->SetArrayField(TEXT("classDefaults"), Defaults);
		StructuredContent->SetArrayField(TEXT("finalized"), Finalized);
		StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);
		return StructuredContent;
	}

	FUnrealMcpExecutionResult ScaffoldRoundSystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Core")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
			MakeScaffoldPath(RootPath, TEXT("Data")),
			MakeScaffoldPath(RootPath, TEXT("Maps")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		auto CreateBlueprint = [&](const FString& RelativePath, const FString& ParentClassPath) -> UBlueprint*
		{
			FString ObjectPath;
			bool bCreated = false;
			return LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, RelativePath), ParentClassPath, ObjectPath, bCreated, Assets, FailureReason);
		};

		UBlueprint* GameMode = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_GameMode"), TEXT("/Script/Engine.GameModeBase"));
		UBlueprint* GameState = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_GameState"), TEXT("/Script/Engine.GameStateBase"));
		UBlueprint* PlayerState = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_PlayerState"), TEXT("/Script/Engine.PlayerState"));
		UBlueprint* PlayerController = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_PlayerController"), TEXT("/Script/Engine.PlayerController"));
		UBlueprint* RoundManager = CreateBlueprint(TEXT("Blueprints/Systems/BP_IT_RoundManagerComponent"), TEXT("/Script/Engine.ActorComponent"));
		if (!GameMode || !GameState || !PlayerState || !PlayerController || !RoundManager)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(GameState, TEXT("CurrentRound"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(GameState, TEXT("CurrentPhase"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), TEXT("Initialization"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(GameState, TEXT("PreparationDuration"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("45.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(GameState, TEXT("CombatDuration"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("90.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(PlayerState, TEXT("PlayerHealth"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("100"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(PlayerState, TEXT("bEliminated"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Boolean), TEXT("false"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(RoundManager, TEXT("CurrentRound"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(RoundManager, TEXT("CurrentPhase"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), TEXT("Initialization"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(RoundManager, TEXT("RoundPairings"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);

		const FString RoundFunctions[] = {
			TEXT("StartMatchFlow"),
			TEXT("AdvanceRound"),
			TEXT("BeginPreparationPhase"),
			TEXT("BeginCombatPhase"),
			TEXT("BeginResolutionPhase"),
			TEXT("PairPlayersForCombat"),
			TEXT("ApplyRoundDamage"),
			TEXT("CheckVictoryCondition"),
		};
		for (const FString& FunctionName : RoundFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(RoundManager, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		TArray<UBlueprint*> Blueprints = { GameState, PlayerState, PlayerController, RoundManager, GameMode };
		for (UBlueprint* Blueprint : Blueprints)
		{
			FinalizeScaffoldBlueprint(Blueprint, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		}

		SetBlueprintClassDefault(GameMode, TEXT("GameStateClass"), GameState->GeneratedClass, Defaults);
		SetBlueprintClassDefault(GameMode, TEXT("PlayerStateClass"), PlayerState->GeneratedClass, Defaults);
		SetBlueprintClassDefault(GameMode, TEXT("PlayerControllerClass"), PlayerController->GeneratedClass, Defaults);
		if (bSavePackage)
		{
			EditorAssetSubsystem->SaveLoadedAsset(GameMode, false);
		}

		AddNextStep(NextSteps, TEXT("Attach BP_IT_RoundManagerComponent to BP_IT_GameMode or a dedicated arena actor, then implement phase transition logic in the generated function graphs."));
		AddNextStep(NextSteps, TEXT("Set the prototype map World Settings to BP_IT_GameMode if it is not already selected."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_round_system"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded round system under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldShopSystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		bool bReplaceWidgetRoot = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);
		Arguments.TryGetBoolField(TEXT("replaceWidgetRoot"), bReplaceWidgetRoot);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/UI")),
			MakeScaffoldPath(RootPath, TEXT("Data")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* ShopManager = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems/BP_IT_ShopManagerComponent")), TEXT("/Script/Engine.ActorComponent"), ObjectPath, bCreated, Assets, FailureReason);
		if (!ShopManager)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("ShopOfferCardIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("OwnedCardInstanceIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("RefreshCostFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("1"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("CardCostFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("3"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("ShopSize"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("5"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("PityRefreshLimit"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("13"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("RefreshesWithoutDuplicate"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);

		const FString ShopFunctions[] = {
			TEXT("RefreshShop"),
			TEXT("GenerateShopOffers"),
			TEXT("BuyShopOffer"),
			TEXT("SellOwnedCard"),
			TEXT("MoveCardBetweenZones"),
			TEXT("CheckTripleCandidates"),
			TEXT("ApplyPityOffer"),
			TEXT("ClearShopOffers"),
		};
		for (const FString& FunctionName : ShopFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(ShopManager, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}
		FinalizeScaffoldBlueprint(ShopManager, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		FString WidgetObjectPath;
		bool bWidgetCreated = false;
		UWidgetBlueprint* ShopWidget = LoadOrCreateWidgetBlueprintAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/UI/WBP_IT_ShopPanel")), WidgetObjectPath, bWidgetCreated, FailureReason);
		if (!ShopWidget)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		AddScaffoldRecord(Assets, TEXT("widget_blueprint"), WidgetObjectPath, bWidgetCreated);
		if (bReplaceWidgetRoot || !ShopWidget->WidgetTree || !ShopWidget->WidgetTree->RootWidget)
		{
			if (!BuildShopWidgetTemplate(ShopWidget, TEXT("Demo Shop"), FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			MarkWidgetBlueprintModified(ShopWidget, true);
		}
		FinalizeScaffoldBlueprint(ShopWidget, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		AddNextStep(NextSteps, TEXT("Connect RefreshButton.OnClicked to BP_IT_ShopManagerComponent.RefreshShop with unreal.widget_bind_event or manual Blueprint wiring."));
		AddNextStep(NextSteps, TEXT("Replace Name arrays with FCardInstanceData/FCardData structs once the C++ or Blueprint struct layer exists."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_shop_system"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded shop system under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldEconomySystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Core")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* EconomyComponent = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems/BP_IT_EconomyComponent")), TEXT("/Script/Engine.ActorComponent"), ObjectPath, bCreated, Assets, FailureReason);
		UBlueprint* PlayerState = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Core/BP_IT_PlayerState")), TEXT("/Script/Engine.PlayerState"), ObjectPath, bCreated, Assets, FailureReason);
		if (!EconomyComponent || !PlayerState)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		UBlueprint* EconomyTargets[] = { EconomyComponent, PlayerState };
		for (UBlueprint* Target : EconomyTargets)
		{
			EnsureBlueprintScaffoldVariable(Target, TEXT("CurrentFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("3"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("MaxFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("10"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("CurrentGold"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("1"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("HubLevel"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("1"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("WinStreak"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("LossStreak"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		}

		const FString EconomyFunctions[] = {
			TEXT("InitializeEconomy"),
			TEXT("ApplyRoundIncome"),
			TEXT("ResetRoundFood"),
			TEXT("SpendFood"),
			TEXT("SpendGold"),
			TEXT("AddFood"),
			TEXT("AddGold"),
			TEXT("UpgradeHub"),
			TEXT("ApplyStreakBonus"),
		};
		for (const FString& FunctionName : EconomyFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(EconomyComponent, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FinalizeScaffoldBlueprint(EconomyComponent, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		FinalizeScaffoldBlueprint(PlayerState, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		AddNextStep(NextSteps, TEXT("Attach BP_IT_EconomyComponent to PlayerController or PlayerState once ownership is decided."));
		AddNextStep(NextSteps, TEXT("Move server validation into PlayerController RPCs after the resource functions have Blueprint bodies."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_economy_system"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded economy system under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldAutobattlerAi(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Units")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/AI")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Combat")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* UnitBase = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Units/BP_IT_UnitBase")), TEXT("/Script/Engine.Character"), ObjectPath, bCreated, Assets, FailureReason);
		UBlueprint* AIController = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/AI/BP_IT_AutoBattleAIController")), TEXT("/Script/AIModule.AIController"), ObjectPath, bCreated, Assets, FailureReason);
		UBlueprint* CombatManager = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Combat/BP_IT_CombatManager")), TEXT("/Script/Engine.Actor"), ObjectPath, bCreated, Assets, FailureReason);
		if (!UnitBase || !AIController || !CombatManager)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("TeamId"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("UnitHealth"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("100.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("UnitDamage"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("10.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("AttackRange"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("160.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("AttackCooldown"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("1.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("SourceCardInstanceId"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(CombatManager, TEXT("TeamAUnitIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(CombatManager, TEXT("TeamBUnitIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(CombatManager, TEXT("bCombatRunning"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Boolean), TEXT("false"), Variables, FailureReason);

		const FString UnitFunctions[] = {
			TEXT("FindNearestEnemy"),
			TEXT("MoveToEnemy"),
			TEXT("PerformAttack"),
			TEXT("ApplyDamage"),
			TEXT("HandleDeath"),
		};
		for (const FString& FunctionName : UnitFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(UnitBase, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		const FString CombatFunctions[] = {
			TEXT("SpawnTeamsFromFieldCards"),
			TEXT("StartCombat"),
			TEXT("TickCombatState"),
			TEXT("ResolveCombat"),
			TEXT("CalculateSurvivorDamage"),
			TEXT("CleanupCombatUnits"),
		};
		for (const FString& FunctionName : CombatFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(CombatManager, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FinalizeScaffoldBlueprint(AIController, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		FinalizeScaffoldBlueprint(UnitBase, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		FinalizeScaffoldBlueprint(CombatManager, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		SetBlueprintClassDefault(UnitBase, TEXT("AIControllerClass"), AIController->GeneratedClass, Defaults);
		if (bSavePackage)
		{
			EditorAssetSubsystem->SaveLoadedAsset(UnitBase, false);
		}

		AddNextStep(NextSteps, TEXT("Implement FindNearestEnemy with TeamId filtering, then wire MoveToEnemy and PerformAttack from Tick or a Behavior Tree."));
		AddNextStep(NextSteps, TEXT("Keep spawned unit counts small until combat rules are proven; move to pooling or Mass later if needed."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_autobattler_ai"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded autobattler AI under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldResultUi(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		bool bReplaceWidgetRoot = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);
		Arguments.TryGetBoolField(TEXT("replaceWidgetRoot"), bReplaceWidgetRoot);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/UI")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* ResultPresenter = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems/BP_IT_ResultPresenterComponent")), TEXT("/Script/Engine.ActorComponent"), ObjectPath, bCreated, Assets, FailureReason);
		if (!ResultPresenter)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(ResultPresenter, TEXT("LastOutcome"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ResultPresenter, TEXT("LastDamage"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ResultPresenter, TEXT("bResultVisible"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Boolean), TEXT("false"), Variables, FailureReason);

		const FString ResultFunctions[] = {
			TEXT("ShowCombatResult"),
			TEXT("HideCombatResult"),
			TEXT("SetResultSummary"),
			TEXT("BindContinueButton"),
			TEXT("ContinueToPreparation"),
		};
		for (const FString& FunctionName : ResultFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(ResultPresenter, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}
		FinalizeScaffoldBlueprint(ResultPresenter, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		FString WidgetObjectPath;
		bool bWidgetCreated = false;
		UWidgetBlueprint* ResultWidget = LoadOrCreateWidgetBlueprintAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/UI/WBP_IT_ResultPanel")), WidgetObjectPath, bWidgetCreated, FailureReason);
		if (!ResultWidget)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		AddScaffoldRecord(Assets, TEXT("widget_blueprint"), WidgetObjectPath, bWidgetCreated);
		if (bReplaceWidgetRoot || !ResultWidget->WidgetTree || !ResultWidget->WidgetTree->RootWidget)
		{
			if (!BuildResultWidgetTemplate(ResultWidget, TEXT("Combat Result"), FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			MarkWidgetBlueprintModified(ResultWidget, true);
		}
		FinalizeScaffoldBlueprint(ResultWidget, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		AddNextStep(NextSteps, TEXT("Bind ContinueButton.OnClicked to ContinueToPreparation, then have RoundManager call ShowCombatResult after ResolveCombat."));
		AddNextStep(NextSteps, TEXT("Feed OutcomeText and DamageText from combat resolution data once the result struct exists."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_result_ui"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded result UI under %s."), *RootPath), StructuredContent, false);
	}

	void AddNextStep(TArray<TSharedPtr<FJsonValue>>& NextSteps, const FString& Text)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(Text));
	}

	FString SanitizeMcpToolIdForPath(const FString& ToolName)
	{
		FString CleanName = ToolName.TrimStartAndEnd();
		if (CleanName.StartsWith(TEXT("unreal."), ESearchCase::IgnoreCase))
		{
			CleanName.RightChopInline(7);
		}

		FString Result;
		for (const TCHAR Character : CleanName)
		{
			const bool bIsAlphaNumeric =
				(Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'));
			Result.AppendChar(bIsAlphaNumeric ? Character : TEXT('_'));
		}

		while (Result.Contains(TEXT("__")))
		{
			Result.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		Result.TrimStartAndEndInline();
		while (Result.StartsWith(TEXT("_")))
		{
			Result.RightChopInline(1);
		}
		while (Result.EndsWith(TEXT("_")))
		{
			Result.LeftChopInline(1);
		}

		return Result.IsEmpty() ? TEXT("custom_tool") : Result;
	}

	FString EscapeForCppTextLiteral(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Value.ReplaceInline(TEXT("\r"), TEXT(""));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		return Value;
	}

	bool ResolveProjectOutputDirectory(const FString& RequestedOutputRoot, FString& OutDirectory, FString& OutFailureReason)
	{
		FString OutputRoot = RequestedOutputRoot.TrimStartAndEnd();
		if (OutputRoot.IsEmpty())
		{
			OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
		}

		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectDir);
		FPaths::CollapseRelativeDirectories(ProjectDir);

		FString ResolvedDirectory = FPaths::IsRelative(OutputRoot)
			? FPaths::Combine(ProjectDir, OutputRoot)
			: OutputRoot;
		ResolvedDirectory = FPaths::ConvertRelativePathToFull(ResolvedDirectory);
		FPaths::NormalizeDirectoryName(ResolvedDirectory);
		FPaths::CollapseRelativeDirectories(ResolvedDirectory);

		const FString ProjectDirPrefix = ProjectDir.EndsWith(TEXT("/")) ? ProjectDir : ProjectDir + TEXT("/");
		if (!ResolvedDirectory.Equals(ProjectDir, ESearchCase::IgnoreCase)
			&& !ResolvedDirectory.StartsWith(ProjectDirPrefix, ESearchCase::IgnoreCase))
		{
			OutFailureReason = FString::Printf(
				TEXT("outputRoot '%s' resolves outside the project directory '%s'."),
				*ResolvedDirectory,
				*ProjectDir);
			return false;
		}

		OutDirectory = ResolvedDirectory;
		return true;
	}

	void AppendUniqueScaffoldReadCandidates(TArray<FString>& Target, const TArray<FString>& Source)
	{
		for (const FString& Candidate : Source)
		{
			bool bAlreadyPresent = false;
			for (const FString& Existing : Target)
			{
				if (Existing.Equals(Candidate, ESearchCase::IgnoreCase))
				{
					bAlreadyPresent = true;
					break;
				}
			}
			if (!bAlreadyPresent)
			{
				Target.Add(Candidate);
			}
		}
	}

	FToolsReadResolution ResolveScaffoldReadDirectory_Pure(
		const FString& ProjectDir,
		const FString& PluginBaseDir,
		const FString& ToolId,
		TFunctionRef<bool(const FString&)> FileOrDirExists)
	{
		FToolsReadResolution Resolution;
		const FString CleanToolId = SanitizeMcpToolIdForPath(ToolId).TrimStartAndEnd();
		if (CleanToolId.IsEmpty())
		{
			Resolution.Warning = TEXT("toolId must not be empty.");
			return Resolution;
		}

		Resolution = ResolveToolsReadSubpath_Pure(
			ProjectDir,
			PluginBaseDir,
			FPaths::Combine(TEXT("UnrealMcpToolScaffolds"), CleanToolId),
			FileOrDirExists);
		if (Resolution.bFound)
		{
			return Resolution;
		}

		FToolsReadResolution StarterResolution = ResolveToolsReadSubpath_Pure(
			ProjectDir,
			PluginBaseDir,
			FPaths::Combine(TEXT("UnrealMcpToolScaffoldStarters"), CleanToolId),
			FileOrDirExists);
		AppendUniqueScaffoldReadCandidates(Resolution.Candidates, StarterResolution.Candidates);
		if (StarterResolution.bFound)
		{
			Resolution.Path = StarterResolution.Path;
			Resolution.bFound = true;
			Resolution.SourceKind = FToolsReadResolution::ESource::CanonicalStarter;
			Resolution.Warning = StarterResolution.Warning;
			return Resolution;
		}

		if (Resolution.Path.IsEmpty() && Resolution.Candidates.Num() > 0)
		{
			Resolution.Path = Resolution.Candidates[0];
		}
		if (Resolution.Warning.IsEmpty())
		{
			Resolution.Warning = StarterResolution.Warning;
		}
		return Resolution;
	}

	bool ResolveScaffoldReadDirectory(
		const FString& ToolId,
		FString& OutScaffoldDirectory,
		FString& OutFailureReason,
		FToolsReadResolution* OutResolution)
	{
		OutScaffoldDirectory.Reset();
		OutFailureReason.Reset();
		if (OutResolution)
		{
			*OutResolution = FToolsReadResolution();
		}

		const FString CleanToolId = SanitizeMcpToolIdForPath(ToolId).TrimStartAndEnd();
		if (CleanToolId.IsEmpty())
		{
			OutFailureReason = TEXT("toolId must not be empty.");
			return false;
		}

		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectDir);
		FPaths::CollapseRelativeDirectories(ProjectDir);
		FToolsReadResolution Resolution = ResolveScaffoldReadDirectory_Pure(
			ProjectDir,
			ResolvePluginBaseDir().Path,
			CleanToolId,
			[](const FString& Candidate)
			{
				return SharedRepoRootHasAny(Candidate, { TEXT("ScaffoldMetadata.json") });
			});
		if (OutResolution)
		{
			*OutResolution = Resolution;
		}
		if (Resolution.bFound)
		{
			OutScaffoldDirectory = Resolution.Path;
			return true;
		}

		OutFailureReason = FString::Printf(
			TEXT("Could not find MCP scaffold '%s' with ScaffoldMetadata.json. Checked: %s."),
			*CleanToolId,
			*FString::Join(Resolution.Candidates, TEXT(", ")));
		return false;
	}

	bool WriteMcpScaffoldFile(
		const FString& FilePath,
		const FString& Content,
		bool bOverwrite,
		TArray<TSharedPtr<FJsonValue>>& Files,
		FString& OutFailureReason)
	{
		const FString Directory = FPaths::GetPath(FilePath);
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create directory '%s'."), *Directory);
			return false;
		}

		const bool bExists = FPaths::FileExists(FilePath);
		if (bExists && !bOverwrite)
		{
			OutFailureReason = FString::Printf(TEXT("Refusing to overwrite existing file '%s'. Set overwrite=true to replace it."), *FilePath);
			return false;
		}

		if (!FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to write file '%s'."), *FilePath);
			return false;
		}

		TSharedPtr<FJsonObject> FileObject = MakeShared<FJsonObject>();
		FileObject->SetStringField(TEXT("path"), FilePath);
		FileObject->SetBoolField(TEXT("created"), !bExists);
		FileObject->SetBoolField(TEXT("overwritten"), bExists && bOverwrite);
		Files.Add(MakeShared<FJsonValueObject>(FileObject));
		return true;
	}

	FString BuildMcpToolDefinitionSnippet(const FString& ToolName, const FString& Title, const FString& Description)
	{
		return FString::Printf(
			TEXT("// Insert inside FUnrealMcpModule::AppendToolDefinitions.\n")
			TEXT("{\n")
			TEXT("\tTSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();\n")
			TEXT("\t// TODO: Add fixed-schema fields, for example:\n")
			TEXT("\t// PropertiesObject->SetObjectField(TEXT(\"message\"), UnrealMcp::MakeStringProperty(TEXT(\"Message to process.\")));\n\n")
			TEXT("\tTSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();\n")
			TEXT("\tInputSchema->SetObjectField(TEXT(\"properties\"), PropertiesObject);\n\n")
			TEXT("\tUnrealMcp::AddToolDefinition(\n")
			TEXT("\t\tToolsArray,\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tInputSchema);\n")
			TEXT("}\n"),
			*EscapeForCppTextLiteral(ToolName),
			*EscapeForCppTextLiteral(Title),
			*EscapeForCppTextLiteral(Description));
	}

	FString BuildMcpToolHandlerSnippet(const FString& ToolName, const FString& Title)
	{
		return FString::Printf(
			TEXT("// Insert inside FUnrealMcpModule::ExecuteTool before the final unknown-tool result.\n")
			TEXT("if (ToolName == TEXT(\"%s\"))\n")
			TEXT("{\n")
			TEXT("\t// TODO: Validate Arguments and implement the editor operation.\n")
			TEXT("\tFString Message;\n")
			TEXT("\tArguments.TryGetStringField(TEXT(\"message\"), Message);\n\n")
			TEXT("\tTSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();\n")
			TEXT("\tStructuredContent->SetStringField(TEXT(\"message\"), Message);\n")
			TEXT("\tStructuredContent->SetStringField(TEXT(\"tool\"), TEXT(\"%s\"));\n\n")
			TEXT("\treturn UnrealMcp::MakeExecutionResult(\n")
			TEXT("\t\tFString::Printf(TEXT(\"%s completed. message=%%s\"), *Message),\n")
			TEXT("\t\tStructuredContent,\n")
			TEXT("\t\tfalse);\n")
			TEXT("}\n"),
			*EscapeForCppTextLiteral(ToolName),
			*EscapeForCppTextLiteral(ToolName),
			*EscapeForCppTextLiteral(Title));
	}

	FString BuildMcpToolChatCommandSnippet(const FString& ToolName)
	{
		return FString::Printf(
			TEXT("// Optional: insert inside FUnrealMcpModule::ExecuteChatCommand.\n")
			TEXT("if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT(\"/%s\")))\n")
			TEXT("{\n")
			TEXT("\tTSharedPtr<FJsonObject> ArgumentsObject = MakeShared<FJsonObject>();\n")
			TEXT("\tArgumentsObject->SetStringField(TEXT(\"message\"), UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT(\"/%s\")));\n")
			TEXT("\treturn ExecuteTool(TEXT(\"%s\"), *ArgumentsObject);\n")
			TEXT("}\n"),
			*EscapeForCppTextLiteral(SanitizeMcpToolIdForPath(ToolName)),
			*EscapeForCppTextLiteral(SanitizeMcpToolIdForPath(ToolName)),
			*EscapeForCppTextLiteral(ToolName));
	}

	FString NormalizeMcpToolCategory(const FString& RequestedCategory)
	{
		const FString Category = RequestedCategory.TrimStartAndEnd().ToLower();
		if (Category == TEXT("actors")
			|| Category == TEXT("blueprint")
			|| Category == TEXT("editor")
			|| Category == TEXT("memory")
			|| Category == TEXT("scaffold")
			|| Category == TEXT("self-extension")
			|| Category == TEXT("skills")
			|| Category == TEXT("widget"))
		{
			return Category;
		}
		return TEXT("self-extension");
	}

	FString NormalizeMcpToolRiskLevel(const FString& RequestedRiskLevel)
	{
		const FString RiskLevel = RequestedRiskLevel.TrimStartAndEnd().ToLower();
		if (RiskLevel == TEXT("read_only")
			|| RiskLevel == TEXT("low")
			|| RiskLevel == TEXT("medium")
			|| RiskLevel == TEXT("high")
			|| RiskLevel == TEXT("critical"))
		{
			return RiskLevel;
		}
		return TEXT("low");
	}

	FString GetMcpToolCategorySourceFile(const FString& Category)
	{
		if (Category == TEXT("actors"))
		{
			return TEXT("UnrealMcpActorTools.cpp");
		}
		if (Category == TEXT("blueprint"))
		{
			return TEXT("UnrealMcpBlueprintTools.cpp");
		}
		if (Category == TEXT("editor"))
		{
			return TEXT("UnrealMcpEditorTools.cpp");
		}
		if (Category == TEXT("memory"))
		{
			return TEXT("UnrealMcpMemoryTools.cpp");
		}
		if (Category == TEXT("scaffold"))
		{
			return TEXT("UnrealMcpScaffoldTools.cpp");
		}
		if (Category == TEXT("skills"))
		{
			return TEXT("UnrealMcpSkillTools.cpp");
		}
		if (Category == TEXT("widget"))
		{
			return TEXT("UnrealMcpWidgetTools.cpp");
		}
		return TEXT("UnrealMcpSelfExtensionTools.cpp");
	}

	FString GetMcpToolCategoryTryExecuteName(const FString& Category)
	{
		if (Category == TEXT("actors"))
		{
			return TEXT("TryExecuteActorTool");
		}
		if (Category == TEXT("blueprint"))
		{
			return TEXT("TryExecuteBlueprintTool");
		}
		if (Category == TEXT("editor"))
		{
			return TEXT("TryExecuteEditorTool");
		}
		if (Category == TEXT("memory"))
		{
			return TEXT("TryExecuteMemoryTool");
		}
		if (Category == TEXT("scaffold"))
		{
			return TEXT("TryExecuteScaffoldTool");
		}
		if (Category == TEXT("skills"))
		{
			return TEXT("TryExecuteSkillTool");
		}
		if (Category == TEXT("widget"))
		{
			return TEXT("TryExecuteWidgetTool");
		}
		return TEXT("TryExecuteSelfExtensionTool");
	}

	FString MakeMcpGeneratedFunctionSuffix(const FString& ToolName)
	{
		FString Suffix = SanitizeMcpToolIdForPath(ToolName);
		Suffix.RemoveFromStart(TEXT("unreal_"));
		TArray<FString> Parts;
		Suffix.ParseIntoArray(Parts, TEXT("_"), true);
		FString Result;
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty())
			{
				continue;
			}
			FString CleanPart = Part.ToLower();
			CleanPart[0] = FChar::ToUpper(CleanPart[0]);
			Result += CleanPart;
		}
		return Result.IsEmpty() ? TEXT("GeneratedTool") : Result;
	}

	FString MakeCppBoolLiteral(bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	FString MakeMcpToolRiskEnumLiteral(const FString& RiskLevel)
	{
		if (RiskLevel == TEXT("read_only"))
		{
			return TEXT("EUnrealMcpToolRisk::ReadOnly");
		}
		if (RiskLevel == TEXT("medium"))
		{
			return TEXT("EUnrealMcpToolRisk::Medium");
		}
		if (RiskLevel == TEXT("high"))
		{
			return TEXT("EUnrealMcpToolRisk::High");
		}
		if (RiskLevel == TEXT("critical"))
		{
			return TEXT("EUnrealMcpToolRisk::Critical");
		}
		return TEXT("EUnrealMcpToolRisk::Low");
	}

	FString BuildMcpToolRegistrarFunctionSnippet(
		const FString& ToolName,
		const FString& Title,
		const FString& Description,
		const FString& Category,
		const FString& SourceFile,
		const FString& RiskLevel,
		bool bRequiresWrite,
		bool bRequiresBuild,
		bool bRequiresExternalProcess,
		bool bRequiresRestart,
		bool bRequiresProjectMemory,
		bool bRequiresLock,
		bool bDryRunSupport)
	{
		const FString FunctionSuffix = MakeMcpGeneratedFunctionSuffix(ToolName);
		return FString::Printf(
			TEXT("// Apply to UnrealMcpToolRegistrar.cpp before RegisterAllMcpToolDescriptors.\n")
			TEXT("void RegisterGenerated%sDescriptor(FUnrealMcpToolRegistrar& Registrar)\n")
			TEXT("{\n")
			TEXT("\tTSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();\n")
			TEXT("\tProperties->SetObjectField(TEXT(\"message\"), MakeStringProperty(TEXT(\"Message or payload for the tool.\"), FString()));\n")
			TEXT("\tTSharedPtr<FJsonObject> Schema = MakeObjectSchema();\n")
			TEXT("\tSchema->SetObjectField(TEXT(\"properties\"), Properties);\n\n")
			TEXT("\tFUnrealMcpToolDescriptor Descriptor = MakeDescriptor(\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\tTEXT(\"%s\"),\n")
			TEXT("\t\t%s);\n")
			TEXT("\tDescriptor.bRequiresWrite = %s;\n")
			TEXT("\tDescriptor.bRequiresBuild = %s;\n")
			TEXT("\tDescriptor.bRequiresExternalProcess = %s;\n")
			TEXT("\tDescriptor.bRequiresRestart = %s;\n")
			TEXT("\tDescriptor.bRequiresProjectMemory = %s;\n")
			TEXT("\tDescriptor.bRequiresLock = %s;\n")
			TEXT("\tDescriptor.bDryRunSupport = %s;\n")
			TEXT("\tDescriptor.bPreflightSupport = Descriptor.bRequiresWrite || Descriptor.bRequiresBuild || Descriptor.bRequiresExternalProcess || Descriptor.bRequiresRestart || Descriptor.bRequiresProjectMemory || Descriptor.bRequiresLock;\n")
			TEXT("\tDescriptor.bPostcheckSupport = Descriptor.bPreflightSupport;\n")
			TEXT("\tRegistrar.Add(Descriptor, Schema);\n")
			TEXT("}\n"),
			*FunctionSuffix,
			*EscapeForCppTextLiteral(ToolName),
			*EscapeForCppTextLiteral(Title),
			*EscapeForCppTextLiteral(Description),
			*EscapeForCppTextLiteral(Category),
			*EscapeForCppTextLiteral(SourceFile),
			*MakeMcpToolRiskEnumLiteral(RiskLevel),
			*MakeCppBoolLiteral(bRequiresWrite),
			*MakeCppBoolLiteral(bRequiresBuild),
			*MakeCppBoolLiteral(bRequiresExternalProcess),
			*MakeCppBoolLiteral(bRequiresRestart),
			*MakeCppBoolLiteral(bRequiresProjectMemory),
			*MakeCppBoolLiteral(bRequiresLock),
			*MakeCppBoolLiteral(bDryRunSupport));
	}

	FString BuildMcpToolRegistrarCallSnippet(const FString& ToolName)
	{
		return FString::Printf(
			TEXT("// Apply inside RegisterAllMcpToolDescriptors after the relevant category registrar call.\n")
			TEXT("RegisterGenerated%sDescriptor(Registrar);\n"),
			*MakeMcpGeneratedFunctionSuffix(ToolName));
	}

	FString BuildMcpToolCategoryHandlerFunctionSnippet(const FString& ToolName, const FString& Title)
	{
		const FString FunctionSuffix = MakeMcpGeneratedFunctionSuffix(ToolName);
		return FString::Printf(
			TEXT("// Apply in the selected category source file before its TryExecute*Tool dispatcher.\n")
			TEXT("FUnrealMcpExecutionResult ExecuteGenerated%sTool(const FString& ToolName, const FJsonObject& Arguments)\n")
			TEXT("{\n")
			TEXT("\tFString Message;\n")
			TEXT("\tArguments.TryGetStringField(TEXT(\"message\"), Message);\n\n")
			TEXT("\tTSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();\n")
			TEXT("\tStructuredContent->SetStringField(TEXT(\"action\"), TEXT(\"%s\"));\n")
			TEXT("\tStructuredContent->SetStringField(TEXT(\"toolName\"), ToolName);\n")
			TEXT("\tStructuredContent->SetStringField(TEXT(\"message\"), Message);\n\n")
			TEXT("\tFUnrealMcpExecutionResult Result;\n")
			TEXT("\tResult.Text = FString::Printf(TEXT(\"%s completed. message=%%s\"), *Message);\n")
			TEXT("\tResult.StructuredContent = StructuredContent;\n")
			TEXT("\tResult.bIsError = false;\n")
			TEXT("\treturn Result;\n")
			TEXT("}\n"),
			*FunctionSuffix,
			*EscapeForCppTextLiteral(SanitizeMcpToolIdForPath(ToolName)),
			*EscapeForCppTextLiteral(Title));
	}

	FString BuildMcpToolCategoryDispatcherBranchSnippet(const FString& ToolName)
	{
		return FString::Printf(
			TEXT("// Apply near the top of the selected category TryExecute*Tool dispatcher.\n")
			TEXT("if (ToolName == TEXT(\"%s\"))\n")
			TEXT("{\n")
			TEXT("\tOutResult = ExecuteGenerated%sTool(ToolName, Arguments);\n")
			TEXT("\treturn true;\n")
			TEXT("}\n"),
			*EscapeForCppTextLiteral(ToolName),
			*MakeMcpGeneratedFunctionSuffix(ToolName));
	}

	FString BuildMcpToolRegistryPatchJson(
		const FString& ToolName,
		const FString& Category,
		const FString& RiskLevel,
		bool bRequiresWrite,
		bool bRequiresBuild,
		bool bRequiresExternalProcess,
		bool bRequiresRestart,
		bool bRequiresProjectMemory,
		bool bRequiresLock,
		bool bDryRunSupport)
	{
		const bool bNeedsChecks = bRequiresWrite || bRequiresBuild || bRequiresExternalProcess || bRequiresRestart || bRequiresProjectMemory || bRequiresLock;
		return FString::Printf(
			TEXT("{\n")
			TEXT("  \"name\": \"%s\",\n")
			TEXT("  \"category\": \"%s\",\n")
			TEXT("  \"handlerName\": \"%s\",\n")
			TEXT("  \"exposure\": \"visible\",\n")
			TEXT("  \"riskLevel\": \"%s\",\n")
			TEXT("  \"requiresWrite\": %s,\n")
			TEXT("  \"requiresBuild\": %s,\n")
			TEXT("  \"requiresExternalProcess\": %s,\n")
			TEXT("  \"requiresRestart\": %s,\n")
			TEXT("  \"requiresProjectMemory\": %s,\n")
			TEXT("  \"requiresLock\": %s,\n")
			TEXT("  \"dryRunSupport\": %s,\n")
			TEXT("  \"preflightSupport\": %s,\n")
			TEXT("  \"postcheckSupport\": %s,\n")
			TEXT("  \"testCoverage\": \"missing\",\n")
			TEXT("  \"owner\": \"UEvolve Core\",\n")
			TEXT("  \"docsPath\": \"README.md#tool-coverage\",\n")
			TEXT("  \"reason\": \"Generated descriptor-first scaffold. Review before merging.\",\n")
			TEXT("  \"notes\": \"Generated by unreal.scaffold_mcp_tool.\"\n")
			TEXT("}\n"),
			*ToolName,
			*Category,
			*ToolName,
			*RiskLevel,
			*MakeCppBoolLiteral(bRequiresWrite),
			*MakeCppBoolLiteral(bRequiresBuild),
			*MakeCppBoolLiteral(bRequiresExternalProcess),
			*MakeCppBoolLiteral(bRequiresRestart),
			*MakeCppBoolLiteral(bRequiresProjectMemory),
			*MakeCppBoolLiteral(bRequiresLock),
			*MakeCppBoolLiteral(bDryRunSupport),
			*MakeCppBoolLiteral(bNeedsChecks),
			*MakeCppBoolLiteral(bNeedsChecks));
	}

		FUnrealMcpExecutionResult ScaffoldMcpTool(const FJsonObject& Arguments)
		{
			FString ToolName;
			if (!Arguments.TryGetStringField(TEXT("toolName"), ToolName) || ToolName.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Missing required field 'toolName'."), nullptr, true);
		}
		ToolName = ToolName.TrimStartAndEnd();
		if (!ToolName.StartsWith(TEXT("unreal."), ESearchCase::CaseSensitive))
		{
			return MakeExecutionResult(TEXT("toolName must start with 'unreal.'."), nullptr, true);
		}

		FString Title;
		FString Description;
		FString OutputRoot;
		FString ArgumentSchemaJson;
		FString ExampleArgumentsJson;
		FString ImplementationNotes;
		FString Category;
		FString RiskLevel;
		bool bOverwrite = false;
		bool bIncludeChatCommandSnippet = true;
		bool bIncludeLegacyCompatibility = false;
		bool bRequiresWrite = false;
		bool bRequiresBuild = false;
		bool bRequiresExternalProcess = false;
		bool bRequiresRestart = false;
		bool bRequiresProjectMemory = false;
		bool bRequiresLock = false;
		bool bDryRunSupport = false;
		Arguments.TryGetStringField(TEXT("title"), Title);
		Arguments.TryGetStringField(TEXT("description"), Description);
		Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
		Arguments.TryGetStringField(TEXT("argumentSchemaJson"), ArgumentSchemaJson);
		Arguments.TryGetStringField(TEXT("exampleArgumentsJson"), ExampleArgumentsJson);
		Arguments.TryGetStringField(TEXT("implementationNotes"), ImplementationNotes);
		Arguments.TryGetStringField(TEXT("category"), Category);
		Arguments.TryGetStringField(TEXT("riskLevel"), RiskLevel);
		Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
		Arguments.TryGetBoolField(TEXT("includeChatCommandSnippet"), bIncludeChatCommandSnippet);
		Arguments.TryGetBoolField(TEXT("includeLegacyCompatibility"), bIncludeLegacyCompatibility);
		Arguments.TryGetBoolField(TEXT("requiresWrite"), bRequiresWrite);
		Arguments.TryGetBoolField(TEXT("requiresBuild"), bRequiresBuild);
		Arguments.TryGetBoolField(TEXT("requiresExternalProcess"), bRequiresExternalProcess);
		Arguments.TryGetBoolField(TEXT("requiresRestart"), bRequiresRestart);
		Arguments.TryGetBoolField(TEXT("requiresProjectMemory"), bRequiresProjectMemory);
		Arguments.TryGetBoolField(TEXT("requiresLock"), bRequiresLock);
		Arguments.TryGetBoolField(TEXT("dryRunSupport"), bDryRunSupport);

		if (Title.TrimStartAndEnd().IsEmpty())
		{
			Title = SanitizeMcpToolIdForPath(ToolName).Replace(TEXT("_"), TEXT(" "));
		}
		if (Description.TrimStartAndEnd().IsEmpty())
		{
			Description = FString::Printf(TEXT("Custom Unreal MCP tool scaffold for %s."), *ToolName);
		}
		if (ArgumentSchemaJson.TrimStartAndEnd().IsEmpty())
		{
			ArgumentSchemaJson = TEXT("{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\",\"description\":\"Message or payload for the tool.\"}}}");
		}
		if (ExampleArgumentsJson.TrimStartAndEnd().IsEmpty())
		{
			ExampleArgumentsJson = TEXT("{\"message\":\"hello\"}");
		}
		Category = NormalizeMcpToolCategory(Category);
		RiskLevel = NormalizeMcpToolRiskLevel(RiskLevel);
		if (bRequiresWrite && RiskLevel == TEXT("read_only"))
		{
			RiskLevel = TEXT("medium");
		}

		FString ResolvedOutputRoot;
		FString FailureReason;
		if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		const FString ToolId = SanitizeMcpToolIdForPath(ToolName);
		const FString ToolDirectory = FPaths::Combine(ResolvedOutputRoot, ToolId);
		const FString CategorySourceFile = GetMcpToolCategorySourceFile(Category);
		const FString TryExecuteName = GetMcpToolCategoryTryExecuteName(Category);

		const FString DefinitionSnippet = BuildMcpToolDefinitionSnippet(ToolName, Title, Description);
		const FString HandlerSnippet = BuildMcpToolHandlerSnippet(ToolName, Title);
		const FString ChatCommandSnippet = BuildMcpToolChatCommandSnippet(ToolName);
		const FString RegistrarFunctionSnippet = BuildMcpToolRegistrarFunctionSnippet(
			ToolName,
			Title,
			Description,
			Category,
			CategorySourceFile,
			RiskLevel,
			bRequiresWrite,
			bRequiresBuild,
			bRequiresExternalProcess,
			bRequiresRestart,
			bRequiresProjectMemory,
			bRequiresLock,
			bDryRunSupport);
		const FString RegistrarCallSnippet = BuildMcpToolRegistrarCallSnippet(ToolName);
		const FString CategoryHandlerFunctionSnippet = BuildMcpToolCategoryHandlerFunctionSnippet(ToolName, Title);
		const FString CategoryDispatcherBranchSnippet = BuildMcpToolCategoryDispatcherBranchSnippet(ToolName);
		const FString ToolRegistryPatchJson = BuildMcpToolRegistryPatchJson(
			ToolName,
			Category,
			RiskLevel,
			bRequiresWrite,
			bRequiresBuild,
			bRequiresExternalProcess,
			bRequiresRestart,
			bRequiresProjectMemory,
			bRequiresLock,
			bDryRunSupport);
		const FString ScaffoldMetadata = FString::Printf(
			TEXT("{\n")
			TEXT("  \"schemaVersion\": 1,\n")
			TEXT("  \"toolName\": \"%s\",\n")
			TEXT("  \"toolId\": \"%s\",\n")
			TEXT("  \"category\": \"%s\",\n")
			TEXT("  \"riskLevel\": \"%s\",\n")
			TEXT("  \"categorySourceFile\": \"%s\",\n")
			TEXT("  \"categoryTryExecute\": \"%s\",\n")
			TEXT("  \"descriptorFirst\": true,\n")
			TEXT("  \"requiresWrite\": %s,\n")
			TEXT("  \"requiresBuild\": %s,\n")
			TEXT("  \"requiresExternalProcess\": %s,\n")
			TEXT("  \"requiresRestart\": %s,\n")
			TEXT("  \"requiresProjectMemory\": %s,\n")
			TEXT("  \"requiresLock\": %s,\n")
			TEXT("  \"dryRunSupport\": %s,\n")
			TEXT("  \"buildRequirements\": {\n")
			TEXT("    \"requiredIncludes\": [],\n")
			TEXT("    \"requiredModules\": []\n")
			TEXT("  },\n")
			TEXT("  \"dependsOn\": []\n")
			TEXT("}\n"),
			*ToolName,
			*ToolId,
			*Category,
			*RiskLevel,
			*CategorySourceFile,
			*TryExecuteName,
			*MakeCppBoolLiteral(bRequiresWrite),
			*MakeCppBoolLiteral(bRequiresBuild),
			*MakeCppBoolLiteral(bRequiresExternalProcess),
			*MakeCppBoolLiteral(bRequiresRestart),
			*MakeCppBoolLiteral(bRequiresProjectMemory),
			*MakeCppBoolLiteral(bRequiresLock),
			*MakeCppBoolLiteral(bDryRunSupport));
		const FString TestRequest = FString::Printf(
			TEXT("{\n")
			TEXT("  \"jsonrpc\": \"2.0\",\n")
			TEXT("  \"id\": 1,\n")
			TEXT("  \"method\": \"tools/call\",\n")
			TEXT("  \"params\": {\n")
			TEXT("    \"name\": \"%s\",\n")
			TEXT("    \"arguments\": %s\n")
			TEXT("  }\n")
			TEXT("}\n"),
			*ToolName,
			*ExampleArgumentsJson);

		const FString Readme = FString::Printf(
			TEXT("# %s MCP Tool Scaffold\n\n")
			TEXT("Tool name: `%s`\n\n")
			TEXT("Title: `%s`\n\n")
			TEXT("Description: %s\n\n")
			TEXT("Category: `%s`\n\n")
			TEXT("Risk: `%s`\n\n")
			TEXT("## Important\n\n")
			TEXT("This scaffold does not hot-load a C++ MCP tool into the running editor. Review the descriptor-first patch files, apply them through `unreal.mcp_apply_scaffold`, rebuild the current `<ProjectName>Editor` target, and restart Unreal Editor if needed.\n\n")
			TEXT("Descriptor-first patch files:\n\n")
			TEXT("- `ToolRegistrar.patch.cpp`\n")
			TEXT("- `ToolRegistrarCall.patch.cpp`\n")
			TEXT("- `CategoryHandlerFunction.patch.cpp`\n")
			TEXT("- `CategoryDispatcherBranch.patch.cpp`\n")
			TEXT("- `ToolRegistryPatch.json`\n\n")
			TEXT("Legacy `ToolDefinition` / `ExecuteToolHandler` fragments are no longer the default path and should not be used for new tools.\n\n")
			TEXT("## Requested Argument Schema\n\n")
			TEXT("```json\n%s\n```\n\n")
			TEXT("## Example Arguments\n\n")
			TEXT("```json\n%s\n```\n\n")
			TEXT("## Implementation Notes\n\n")
			TEXT("%s\n"),
			*ToolId,
			*ToolName,
			*Title,
			*Description,
			*Category,
			*RiskLevel,
			*ArgumentSchemaJson,
			*ExampleArgumentsJson,
			ImplementationNotes.TrimStartAndEnd().IsEmpty() ? TEXT("- Add validation, editor safety checks, structured content, docs, and tests before shipping.") : *ImplementationNotes);

		const FString Checklist = FString::Printf(
			TEXT("# Integration Checklist\n\n")
			TEXT("- Run `unreal.mcp_apply_scaffold` dryRun first; it applies descriptor, handler, dispatcher, and ToolRegistry patches together.\n")
			TEXT("- `ToolRegistrar.patch.cpp` targets `UnrealMcpToolRegistrar.cpp` before `RegisterAllMcpToolDescriptors`.\n")
			TEXT("- `ToolRegistrarCall.patch.cpp` targets `RegisterAllMcpToolDescriptors`.\n")
			TEXT("- `CategoryHandlerFunction.patch.cpp` targets `%s` before `%s`.\n")
			TEXT("- `CategoryDispatcherBranch.patch.cpp` targets `%s`.\n")
			TEXT("- `ToolRegistryPatch.json` is merged into `Tools/UnrealMcpToolRegistry/tools.json` and mirrored to plugin resources.\n")
			TEXT("- Fill `ScaffoldMetadata.json` `buildRequirements.requiredIncludes` with `{ file, includes }` entries when a patch needs headers in a target under `Plugins/UnrealMcp/Source/UnrealMcp/Private` or `Public`; dry-run reports these under `buildRequirements.includesPlanned[]`.\n")
			TEXT("- Fill `buildRequirements.requiredModules` with any Unreal module names that must be present in `UnrealMcp.Build.cs` `PrivateDependencyModuleNames`; dry-run reports these under `buildRequirements.modulesPlanned[]`.\n")
			TEXT("- Fill `dependsOn` with prerequisite scaffold/tool IDs when this scaffold should be applied after another generated tool.\n")
			TEXT("- Optionally apply `ChatCommand.patch.cpp` to `FUnrealMcpModule::ExecuteChatCommand`.\n")
			TEXT("- Add a short example to `Plugins/UnrealMcp/README.md`.\n")
			TEXT("- Run `python3 Tools/validate_tool_registry.py`.\n")
			TEXT("- Rebuild the current `<ProjectName>Editor` target.\n")
			TEXT("- Start Unreal Editor and call `/tool %s %s` from Unreal MCP Chat.\n")
			TEXT("- Verify `tools/list` includes `%s`.\n"),
			*CategorySourceFile,
			*TryExecuteName,
			*TryExecuteName,
			*ToolName,
			*ExampleArgumentsJson,
			*ToolName);

		const FString ExtensionReport = FString::Printf(
			TEXT("# Extension Report: %s\n\n")
			TEXT("## Summary\n\n")
			TEXT("- Tool: `%s`\n")
			TEXT("- Category: `%s`\n")
			TEXT("- Handler: `%s`\n")
			TEXT("- Category source: `%s`\n")
			TEXT("- Risk: `%s`\n")
			TEXT("- Requires write/build/process/restart/memory/lock: `%s/%s/%s/%s/%s/%s`\n\n")
			TEXT("## Generated Integration Files\n\n")
			TEXT("- `ToolRegistrar.patch.cpp`\n")
			TEXT("- `ToolRegistrarCall.patch.cpp`\n")
			TEXT("- `CategoryHandlerFunction.patch.cpp`\n")
			TEXT("- `CategoryDispatcherBranch.patch.cpp`\n")
			TEXT("- `ToolRegistryPatch.json`\n")
			TEXT("- `TestRequest.json`\n")
			TEXT("- `IntegrationChecklist.md`\n\n")
			TEXT("## Required Gates\n\n")
			TEXT("- preview_change_plan\n")
			TEXT("- mcp_validate_tool_schema\n")
			TEXT("- mcp_apply_scaffold dryRun/apply descriptor-first patches\n")
			TEXT("- mcp_build_editor\n")
			TEXT("- mcp_run_test_suite\n")
			TEXT("- verify_task_outcome\n\n")
			TEXT("## Review Notes\n\n")
			TEXT("%s\n"),
			*ToolId,
			*ToolName,
			*Category,
			*ToolName,
			*CategorySourceFile,
			*RiskLevel,
			*MakeCppBoolLiteral(bRequiresWrite),
			*MakeCppBoolLiteral(bRequiresBuild),
			*MakeCppBoolLiteral(bRequiresExternalProcess),
			*MakeCppBoolLiteral(bRequiresRestart),
			*MakeCppBoolLiteral(bRequiresProjectMemory),
			*MakeCppBoolLiteral(bRequiresLock),
			ImplementationNotes.TrimStartAndEnd().IsEmpty() ? TEXT("- No custom notes provided.") : *ImplementationNotes);

		TArray<TSharedPtr<FJsonValue>> Files;
		if (!WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("README.md")), Readme, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("ScaffoldMetadata.json")), ScaffoldMetadata, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("ToolRegistrar.patch.cpp")), RegistrarFunctionSnippet, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("ToolRegistrarCall.patch.cpp")), RegistrarCallSnippet, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("CategoryHandlerFunction.patch.cpp")), CategoryHandlerFunctionSnippet, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("CategoryDispatcherBranch.patch.cpp")), CategoryDispatcherBranchSnippet, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("ToolRegistryPatch.json")), ToolRegistryPatchJson, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("ExtensionReport.md")), ExtensionReport, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("TestRequest.json")), TestRequest, bOverwrite, Files, FailureReason)
			|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("IntegrationChecklist.md")), Checklist, bOverwrite, Files, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		if (bIncludeChatCommandSnippet
			&& !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("ChatCommand.patch.cpp")), ChatCommandSnippet, bOverwrite, Files, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		if (bIncludeLegacyCompatibility)
		{
			if (!WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("LegacyToolDefinition.legacy.cpp")), DefinitionSnippet, bOverwrite, Files, FailureReason)
				|| !WriteMcpScaffoldFile(FPaths::Combine(ToolDirectory, TEXT("LegacyExecuteToolHandler.legacy.cpp")), HandlerSnippet, bOverwrite, Files, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		TArray<TSharedPtr<FJsonValue>> NextSteps;
		AddNextStep(NextSteps, TEXT("Review descriptor-first patch files before integrating them into the plugin source."));
		AddNextStep(NextSteps, TEXT("Run unreal.mcp_apply_scaffold dryRun, then apply; it will merge ToolRegistryPatch.json and source patches together."));
		AddNextStep(NextSteps, TEXT("Prefer fixed JSON schemas; avoid additionalProperties=true when the tool should be visible to OpenAI function calling."));
		AddNextStep(NextSteps, TEXT("Rebuild the current <ProjectName>Editor target after applying the descriptor-first patches."));

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("scaffold_mcp_tool"));
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("toolId"), ToolId);
		StructuredContent->SetStringField(TEXT("category"), Category);
		StructuredContent->SetStringField(TEXT("riskLevel"), RiskLevel);
		StructuredContent->SetStringField(TEXT("categorySourceFile"), CategorySourceFile);
		StructuredContent->SetStringField(TEXT("categoryTryExecute"), TryExecuteName);
		StructuredContent->SetBoolField(TEXT("descriptorFirst"), true);
		StructuredContent->SetStringField(TEXT("directory"), ToolDirectory);
		StructuredContent->SetArrayField(TEXT("files"), Files);
		StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);

			return MakeExecutionResult(
				FString::Printf(TEXT("Scaffolded MCP tool extension files for %s under %s."), *ToolName, *ToolDirectory),
				StructuredContent,
					false);
			}

		FUnrealMcpExecutionResult ScaffoldRecipe(const FJsonObject& Arguments)
		{
			FString RecipeName;
			FString RootPath;
			FString TaskName;
			FString MemoryKey = TEXT("chat.active_task");
			bool bWriteMemory = true;
			bool bIncludeToolCalls = true;
			Arguments.TryGetStringField(TEXT("recipeName"), RecipeName);
			Arguments.TryGetStringField(TEXT("rootPath"), RootPath);
			Arguments.TryGetStringField(TEXT("taskName"), TaskName);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetBoolField(TEXT("writeMemory"), bWriteMemory);
			Arguments.TryGetBoolField(TEXT("includeToolCalls"), bIncludeToolCalls);

			RecipeName = NormalizeRecipeName(RecipeName);
			RootPath = NormalizeScaffoldRootPath(RootPath.IsEmpty() ? TEXT("/Game/UEvolveRecipes") : RootPath);
			MemoryKey = MemoryKey.TrimStartAndEnd().IsEmpty() ? TEXT("chat.active_task") : MemoryKey.TrimStartAndEnd();

			FString Title;
			FString Objective;
			FString SuggestedNextStep;
			TArray<TSharedPtr<FJsonValue>> Steps;
			TArray<TSharedPtr<FJsonValue>> ToolCalls;
			TArray<TSharedPtr<FJsonValue>> Warnings;

			if (RecipeName == TEXT("first_person_ground_character"))
			{
				Title = TEXT("First-person ground character recipe");
				Objective = TEXT("Create a playable, ground-walking first-person setup with Character, PlayerController, GameMode, map assignment, input, compile, and verification.");
				SuggestedNextStep = TEXT("Create/verify the Character, Controller, and GameMode assets first; do not start PIE until possession and map GameMode are verified.");
				AddRecipeStep(
					Steps,
					TEXT("Inspect project and choose target paths"),
					TEXT("Find maps, existing input assets, and whether Enhanced Input is already used before creating new assets."),
					{ TEXT("unreal.editor_status"), TEXT("unreal.list_maps"), TEXT("unreal.list_assets"), TEXT("unreal.capture_project_snapshot") },
					{ TEXT("Selected map is known."), TEXT("Existing FirstPerson/EnhancedInput assets are identified or confirmed absent.") },
					TEXT("Avoid blindly creating a SpectatorPawn; this recipe requires a ground-walking Character."));
				AddRecipeStep(
					Steps,
					TEXT("Create gameplay Blueprint assets"),
					TEXT("Create BP_FirstPersonGroundCharacter, BP_FirstPersonGroundController, and BP_FirstPersonGroundGameMode under the chosen root."),
					{ TEXT("unreal.create_blueprint_class"), TEXT("unreal.bp_add_variable"), TEXT("unreal.bp_compile_save") },
					{ TEXT("All three Blueprints exist."), TEXT("Character parent class is /Script/Engine.Character."), TEXT("GameMode uses the new Character and Controller defaults.") },
					TEXT("If defaults cannot be set with Blueprint tools, use one small execute_python call and immediately verify with read-only tools."));
				AddRecipeStep(
					Steps,
					TEXT("Configure input and possession"),
					TEXT("Bind movement/look/jump controls and assign the GameMode to the target map or place a matching PlayerStart."),
					{ TEXT("unreal.execute_python"), TEXT("unreal.open_map"), TEXT("unreal.list_level_actors"), TEXT("unreal.map_check") },
					{ TEXT("Target map WorldSettings uses the recipe GameMode."), TEXT("PlayerStart exists."), TEXT("Map check has no blocking errors.") },
					TEXT("Keep Python scoped to project/editor APIs; do not use broad exploratory Python loops."));
				AddRecipeStep(
					Steps,
					TEXT("Verify playable result"),
					TEXT("Compile, save, optionally start PIE, then verify the controlled pawn is the ground Character and not SpectatorPawn."),
					{ TEXT("unreal.bp_compile_save"), TEXT("unreal.save_dirty_packages"), TEXT("unreal.start_pie"), TEXT("unreal.editor_status"), TEXT("unreal.stop_pie") },
					{ TEXT("PIE starts without compile errors."), TEXT("Possessed pawn class matches BP_FirstPersonGroundCharacter."), TEXT("Movement is ground constrained.") },
					TEXT("If tool rounds get high, pause after compile/save and continue from chat.active_task."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.create_blueprint_class"), TEXT("Create the ground Character Blueprint."), FString::Printf(TEXT("{\"assetPath\":\"%s/Blueprints/BP_FirstPersonGroundCharacter\",\"parentClass\":\"/Script/Engine.Character\",\"openAfterCreate\":false,\"compile\":true}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.create_blueprint_class"), TEXT("Create the PlayerController Blueprint."), FString::Printf(TEXT("{\"assetPath\":\"%s/Blueprints/BP_FirstPersonGroundController\",\"parentClass\":\"/Script/Engine.PlayerController\",\"openAfterCreate\":false,\"compile\":true}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.create_blueprint_class"), TEXT("Create the GameMode Blueprint."), FString::Printf(TEXT("{\"assetPath\":\"%s/Blueprints/BP_FirstPersonGroundGameMode\",\"parentClass\":\"/Script/Engine.GameModeBase\",\"openAfterCreate\":false,\"compile\":true}"), *RootPath));
				}
			}
			else if (RecipeName == TEXT("widget_hud"))
			{
				Title = TEXT("Widget/HUD recipe");
				Objective = TEXT("Create a Widget Blueprint HUD, add visible sections, layout them, bind/compile, then verify the designer tree.");
				SuggestedNextStep = TEXT("Build the HUD widget template first, dump the widget tree, then add or tune one widget at a time.");
				AddRecipeStep(
					Steps,
					TEXT("Create HUD widget shell"),
					TEXT("Create or rebuild a Widget Blueprint root with a practical HUD template."),
					{ TEXT("unreal.widget_build_template"), TEXT("unreal.widget_dump_tree") },
					{ TEXT("Widget tree has a root canvas."), TEXT("Primary title/status widgets are present.") },
					TEXT("Prefer compile=false while composing the tree, then compile once near the end."));
				AddRecipeStep(
					Steps,
					TEXT("Add HUD elements incrementally"),
					TEXT("Add TextBlock, ProgressBar, Button, or panel widgets with deterministic names and slot layout."),
					{ TEXT("unreal.widget_add"), TEXT("unreal.widget_set_property"), TEXT("unreal.widget_set_slot_layout") },
					{ TEXT("widget_dump_tree shows each expected widget name."), TEXT("Slot positions and sizes match the requested layout.") },
					TEXT("After each small batch, inspect with widget_dump_tree instead of guessing."));
				AddRecipeStep(
					Steps,
					TEXT("Bind events and compile"),
					TEXT("Bind minimal button events when needed, compile and save the Widget Blueprint."),
					{ TEXT("unreal.widget_bind_event"), TEXT("unreal.bp_compile_save"), TEXT("unreal.save_dirty_packages") },
					{ TEXT("Compile succeeds."), TEXT("EventGraph contains expected generated event nodes."), TEXT("Saved packages include the widget package.") },
					TEXT("Use bp_list_graph_nodes only after binding events, not before."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.widget_build_template"), TEXT("Create a practical HUD shell."), FString::Printf(TEXT("{\"widgetBlueprintPath\":\"%s/Blueprints/UI/WBP_HUD\",\"templateName\":\"mcp_demo_hud\",\"title\":\"HUD\",\"replaceRoot\":true,\"compile\":false,\"savePackage\":false}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.widget_dump_tree"), TEXT("Verify the generated widget hierarchy."), FString::Printf(TEXT("{\"widgetBlueprintPath\":\"%s/Blueprints/UI/WBP_HUD\",\"includeDesignerTree\":true,\"includeGraphNodes\":false}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.bp_compile_save"), TEXT("Compile and save after layout edits."), FString::Printf(TEXT("{\"path\":\"%s/Blueprints/UI/WBP_HUD\"}"), *RootPath));
				}
			}
			else if (RecipeName == TEXT("mcp_self_extension_pipeline"))
			{
				Title = TEXT("MCP self-extension pipeline recipe");
				Objective = TEXT("Add a new MCP tool through the safe self-extension loop: plan, scaffold, schema validate, dry-run, apply, build, test, verify, and rollback/fix if needed.");
				SuggestedNextStep = TEXT("Start with preview_change_plan and scaffold_mcp_tool; do not patch source directly until dryRun is clean.");
				AddRecipeStep(
					Steps,
					TEXT("Plan and scaffold"),
					TEXT("Turn the tool request into a structured plan and generate descriptor-first scaffold files."),
					{ TEXT("unreal.preview_change_plan"), TEXT("unreal.scaffold_mcp_tool"), TEXT("unreal.mcp_inspect_scaffold") },
					{ TEXT("Scaffold has ToolRegistryPatch.json."), TEXT("Descriptor/handler/category patch files exist."), TEXT("TestRequest.json is valid JSON.") },
					TEXT("Keep the tool schema fixed and OpenAI-compatible."));
				AddRecipeStep(
					Steps,
					TEXT("Validate, dry-run, and apply"),
					TEXT("Validate schema/patch safety, generate tests, dry-run source integration, then apply with backups."),
					{ TEXT("unreal.mcp_validate_tool_schema"), TEXT("unreal.mcp_validate_cpp_patch"), TEXT("unreal.mcp_generate_tests"), TEXT("unreal.mcp_apply_scaffold"), TEXT("unreal.mcp_backup_project_state") },
					{ TEXT("Dry-run reports descriptor registration, handler branch, registry merge, and tests."), TEXT("Apply manifest is created."), TEXT("Rollback path exists.") },
					TEXT("If dryRun reports conflicts, patch scaffold fragments before applying."));
				AddRecipeStep(
					Steps,
					TEXT("Build, test, and verify"),
					TEXT("Build the editor target, restart if required, run tool tests, classify failures, and verify outcome."),
					{ TEXT("unreal.mcp_build_editor"), TEXT("unreal.mcp_run_test_suite"), TEXT("unreal.mcp_classify_error"), TEXT("unreal.verify_task_outcome"), TEXT("unreal.mcp_workbench_status") },
					{ TEXT("tools/list shows the new tool."), TEXT("Handler registry has an entry."), TEXT("Test suite passes or failure has a fix/rollback plan.") },
					TEXT("When Editor restart is required, rely on project memory handoff before continuing."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.preview_change_plan"), TEXT("Preview risks and verification gates."), TEXT("{\"task\":\"Add a new MCP tool safely through descriptor-first self-extension.\"}"));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.scaffold_mcp_tool"), TEXT("Generate descriptor-first extension files."), TEXT("{\"toolName\":\"unreal.my_tool\",\"title\":\"My Tool\",\"description\":\"Describe the tool.\",\"argumentSchemaJson\":\"{\\\"type\\\":\\\"object\\\",\\\"properties\\\":{},\\\"additionalProperties\\\":false}\",\"exampleArgumentsJson\":\"{}\",\"category\":\"self-extension\",\"riskLevel\":\"low\",\"overwrite\":false}"));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.mcp_extension_pipeline"), TEXT("Run the gated validate/dryRun/apply/build/test pipeline once scaffold is reviewed."), TEXT("{\"toolName\":\"unreal.my_tool\",\"mode\":\"auto\",\"enforceGate\":true,\"generateTests\":true,\"apply\":true,\"build\":true,\"runTest\":true,\"runTestSuite\":true}"));
				}
			}
			else if (RecipeName == TEXT("rts_camera"))
			{
				Title = TEXT("RTS top-down camera recipe");
				Objective = TEXT("Stand up a top-down camera setup with a pawn that strafes via WASD, edge-scrolls near screen borders, and zooms with the mouse wheel, suitable for an RTS / strategy gameplay prototype.");
				SuggestedNextStep = TEXT("Inventory the current pawn/camera setup first, then create the RTS pawn, controller, and GameMode under the chosen root before wiring input.");
				AddRecipeStep(
					Steps,
					TEXT("Inventory camera and pawn assets"),
					TEXT("Inspect editor state, list existing assets under the target root, and read relevant actor properties before creating replacements."),
					{ TEXT("unreal.editor_status"), TEXT("unreal.list_assets"), TEXT("unreal.actor_get_property") },
					{ TEXT("Chosen root path is known."), TEXT("Existing pawn, controller, camera, or GameMode assets are identified."), TEXT("Any active camera actors or pawn defaults are documented.") },
					TEXT("Do not assume the current map already uses a strategy camera; verify the existing possession path before editing."));
				AddRecipeStep(
					Steps,
					TEXT("Create RTS Blueprint assets"),
					TEXT("Create BP_RTSCameraPawn with SpringArm/Camera intent, BP_RTSPlayerController, and a matching GameMode under the chosen root."),
					{ TEXT("unreal.create_blueprint_class"), TEXT("unreal.bp_add_variable"), TEXT("unreal.bp_compile_save") },
					{ TEXT("BP_RTSCameraPawn parent class is /Script/Engine.Pawn."), TEXT("BP_RTSPlayerController parent class is /Script/Engine.PlayerController."), TEXT("Blueprint assets compile successfully.") },
					TEXT("If component defaults cannot be expressed with Blueprint node tools, use one scoped execute_python call and verify with read-back tools."));
				AddRecipeStep(
					Steps,
					TEXT("Add movement and zoom input"),
					TEXT("Wire WASD strafe, screen-edge scrolling, and mouse-wheel zoom through existing Blueprint node tools or the execute_python fallback."),
					{ TEXT("unreal.execute_python"), TEXT("unreal.bp_add_call_function_node"), TEXT("unreal.bp_connect_pins") },
					{ TEXT("Movement speed, edge-scroll margin, zoom speed, and zoom clamp variables exist."), TEXT("Input execution paths reach movement and spring-arm length updates."), TEXT("Blueprint compile succeeds after wiring.") },
					TEXT("Clamp zoom distance; otherwise the spring arm can collapse into the pawn or push the camera outside the useful framing range."));
				AddRecipeStep(
					Steps,
					TEXT("Verify possession and framing"),
					TEXT("Open the target map, start PIE when ready, confirm the RTS pawn is possessed, inspect the camera pawn transform, then stop PIE."),
					{ TEXT("unreal.open_map"), TEXT("unreal.start_pie"), TEXT("unreal.editor_status"), TEXT("unreal.actor_get_transform"), TEXT("unreal.stop_pie") },
					{ TEXT("PIE starts without compile errors."), TEXT("Possessed pawn class matches BP_RTSCameraPawn."), TEXT("Camera transform frames the intended playable area.") },
					TEXT("If PIE verification is deferred, write the next step to project memory before pausing."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.create_blueprint_class"), TEXT("Create the RTS camera pawn Blueprint."), FString::Printf(TEXT("{\"assetPath\":\"%s/Blueprints/RTSCamera/BP_RTSCameraPawn\",\"parentClass\":\"/Script/Engine.Pawn\",\"openAfterCreate\":false,\"compile\":true}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.create_blueprint_class"), TEXT("Create the RTS PlayerController Blueprint."), FString::Printf(TEXT("{\"assetPath\":\"%s/Blueprints/RTSCamera/BP_RTSPlayerController\",\"parentClass\":\"/Script/Engine.PlayerController\",\"openAfterCreate\":false,\"compile\":true}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.create_blueprint_class"), TEXT("Create the RTS GameMode Blueprint."), FString::Printf(TEXT("{\"assetPath\":\"%s/Blueprints/RTSCamera/BP_RTSGameMode\",\"parentClass\":\"/Script/Engine.GameModeBase\",\"openAfterCreate\":false,\"compile\":true}"), *RootPath));
				}
			}
			else if (RecipeName == TEXT("top_down_input"))
			{
				Title = TEXT("Top-down click-to-move input recipe");
				Objective = TEXT("Add click-to-move plus WASD-strafe input to an existing top-down Character using Enhanced Input or legacy PlayerInput axis mappings, and verify the navigation system projects clicks onto the navmesh.");
				SuggestedNextStep = TEXT("Confirm the current Character, controller, input stack, and navmesh first; do not assume Enhanced Input is enabled.");
				AddRecipeStep(
					Steps,
					TEXT("Verify navmesh and character setup"),
					TEXT("Inspect editor state, list level actors, read target Character or controller properties, and run map check before changing input."),
					{ TEXT("unreal.editor_status"), TEXT("unreal.list_level_actors"), TEXT("unreal.actor_get_property"), TEXT("unreal.map_check") },
					{ TEXT("Target top-down Character is identified."), TEXT("NavMeshBoundsVolume or equivalent navigation setup is present."), TEXT("Map check has no blocking navigation or pawn errors.") },
					TEXT("If no navmesh is present, add or fix navigation before wiring click-to-move logic."));
				AddRecipeStep(
					Steps,
					TEXT("Wire input bindings"),
					TEXT("Read the input project setting, then wire click and WASD behavior with scoped Python or Blueprint function-call nodes."),
					{ TEXT("unreal.project_settings_get"), TEXT("unreal.execute_python"), TEXT("unreal.bp_add_call_function_node"), TEXT("unreal.bp_connect_pins") },
					{ TEXT("Input stack is classified as Enhanced Input or legacy PlayerInput."), TEXT("Click handling projects the cursor hit location onto navigation."), TEXT("WASD paths add strafe movement without breaking click-to-move.") },
					TEXT("Do not assume Enhanced Input is enabled; check project settings and existing assets first."));
				AddRecipeStep(
					Steps,
					TEXT("Compile and verify movement"),
					TEXT("Compile and save the edited Blueprint, save dirty packages, run PIE, verify movement changes the Character transform, then stop PIE."),
					{ TEXT("unreal.bp_compile_save"), TEXT("unreal.save_dirty_packages"), TEXT("unreal.start_pie"), TEXT("unreal.actor_get_transform"), TEXT("unreal.stop_pie") },
					{ TEXT("Blueprint compile succeeds."), TEXT("PIE starts without input setup errors."), TEXT("Click or WASD movement changes the Character transform as expected.") },
					TEXT("For large input rewires, verify one binding at a time before adding the next."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.project_settings_get"), TEXT("Read the legacy input class setting before deciding the wiring path."), TEXT("{\"category\":\"input\",\"key\":\"DefaultPlayerInputClass\"}"));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.bp_add_call_function_node"), TEXT("Add a navigation move-to call node in the controller or Character EventGraph."), FString::Printf(TEXT("{\"blueprintPath\":\"%s/Blueprints/TopDown/BP_TopDownController\",\"graphName\":\"EventGraph\",\"functionClassPath\":\"/Script/AIModule.AIBlueprintHelperLibrary\",\"functionName\":\"SimpleMoveToLocation\",\"x\":500,\"y\":120}"), *RootPath));
				}
			}
			else if (RecipeName == TEXT("hud_dashboard"))
			{
				Title = TEXT("HUD dashboard widget recipe");
				Objective = TEXT("Build a multi-panel Widget Blueprint dashboard with a status header, two metrics panels such as health and score, and a footer with two buttons, then verify layout and event binding.");
				SuggestedNextStep = TEXT("Create the dashboard widget shell with compile=false, inspect the tree, then add panels and bind events before one final compile/save.");
				AddRecipeStep(
					Steps,
					TEXT("Create dashboard widget shell"),
					TEXT("Build a Widget Blueprint template for the dashboard root and inspect the resulting designer tree."),
					{ TEXT("unreal.widget_build_template"), TEXT("unreal.widget_dump_tree") },
					{ TEXT("Widget Blueprint exists."), TEXT("Root canvas or panel is present."), TEXT("Initial header/status widgets are visible in widget_dump_tree.") },
					TEXT("Keep compile=false while composing the layout; compile once near the end."));
				AddRecipeStep(
					Steps,
					TEXT("Add metric panels and variables"),
					TEXT("Add health and score panels, set display properties, position their slots, and expose designer widgets as Blueprint variables."),
					{ TEXT("unreal.widget_add"), TEXT("unreal.widget_set_property"), TEXT("unreal.widget_set_slot_layout"), TEXT("unreal.widget_bind_blueprint_variable") },
					{ TEXT("widget_dump_tree shows the two metric panels and named children."), TEXT("Panel positions and sizes match the requested dashboard layout."), TEXT("Bound widget variables are exposed for Blueprint use.") },
					TEXT("Use deterministic widget names so later binding and verification can target exact nodes."));
				AddRecipeStep(
					Steps,
					TEXT("Wire footer buttons and compile"),
					TEXT("Bind footer button click events, compile the Widget Blueprint, and save dirty packages."),
					{ TEXT("unreal.widget_bind_event"), TEXT("unreal.bp_compile_save"), TEXT("unreal.save_dirty_packages") },
					{ TEXT("Button OnClicked event nodes exist."), TEXT("Widget Blueprint compile succeeds."), TEXT("Saved packages include the dashboard widget package.") },
					TEXT("Bind button events after exposing button widgets as variables; otherwise the generated event may not resolve."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.widget_build_template"), TEXT("Create the dashboard widget shell."), FString::Printf(TEXT("{\"widgetBlueprintPath\":\"%s/Blueprints/UI/WBP_HUDDashboard\",\"templateName\":\"mcp_demo_hud\",\"title\":\"Dashboard\",\"replaceRoot\":true,\"compile\":false,\"savePackage\":false}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.widget_add"), TEXT("Add a footer button to the dashboard root canvas."), FString::Printf(TEXT("{\"widgetBlueprintPath\":\"%s/Blueprints/UI/WBP_HUDDashboard\",\"parentWidgetName\":\"Root\",\"widgetName\":\"FooterConfirmButton\",\"widgetClass\":\"Button\",\"index\":-1,\"isVariable\":true}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.widget_bind_event"), TEXT("Bind the footer button click event."), FString::Printf(TEXT("{\"widgetBlueprintPath\":\"%s/Blueprints/UI/WBP_HUDDashboard\",\"widgetName\":\"FooterConfirmButton\",\"eventName\":\"OnClicked\",\"functionName\":\"OnFooterConfirmClicked\",\"x\":300,\"y\":160,\"compile\":true}"), *RootPath));
				}
			}
			else if (RecipeName == TEXT("asset_naming_audit"))
			{
				Title = TEXT("Asset naming convention audit recipe");
				Objective = TEXT("Walk a project folder, list assets, classify by type, and flag assets whose names do not follow the project's prefix convention such as BP_, T_, M_, A_, etc. for batch rename triage.");
				SuggestedNextStep = TEXT("List the target folder and read the team's naming-convention memory before generating a read-only classification report.");
				AddRecipeStep(
					Steps,
					TEXT("List the target tree"),
					TEXT("List assets and maps under the chosen root, and inspect relevant actor properties only when map actors are part of the audit context."),
					{ TEXT("unreal.list_assets"), TEXT("unreal.list_maps"), TEXT("unreal.actor_get_property") },
					{ TEXT("Target content tree has been enumerated."), TEXT("Map assets are separated from other asset classes."), TEXT("Any actor-specific audit scope is explicit.") },
					TEXT("Keep this recipe read-only; asset renames are a separate migration pass."));
				AddRecipeStep(
					Steps,
					TEXT("Read naming convention memory"),
					TEXT("Read the team's naming convention from project memory or a project skill, and default to common UE Style Guide prefixes if no local convention is found."),
					{ TEXT("unreal.project_memory_read"), TEXT("unreal.skill_read") },
					{ TEXT("Convention source is documented."), TEXT("Prefix map covers the asset classes being audited."), TEXT("Fallback prefixes are clearly marked when memory is absent.") },
					TEXT("Do not silently mix team-specific conventions with a generic style guide; report the source of the rule set."));
				AddRecipeStep(
					Steps,
					TEXT("Report flagged assets"),
					TEXT("Generate a read-only classification report with flagged asset names and optionally write a pause-trail memory entry when the result set is large."),
					{ TEXT("unreal.list_assets"), TEXT("unreal.execute_python"), TEXT("unreal.project_memory_write") },
					{ TEXT("Report groups assets by class or inferred type."), TEXT("Flagged assets include expected prefix and current name."), TEXT("No asset rename operation was performed.") },
					TEXT("This recipe does not rename assets; use a reviewed migration workflow or future rename tools for the rename pass."));
				if (bIncludeToolCalls)
				{
					AddRecipeToolCall(ToolCalls, TEXT("unreal.list_assets"), TEXT("List candidate assets under the audit root."), FString::Printf(TEXT("{\"path\":\"%s\",\"recursive\":true,\"limit\":500}"), *RootPath));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.project_memory_read"), TEXT("Read the team's asset prefix convention if one has been saved."), TEXT("{\"key\":\"asset.naming_convention\",\"includeContent\":true}"));
					AddRecipeToolCall(ToolCalls, TEXT("unreal.execute_python"), TEXT("Generate a read-only asset-prefix classification report."), FString::Printf(TEXT("{\"command\":\"import unreal\\nroot = '%s'\\nprefix_by_class = {'Blueprint':'BP_', 'Texture2D':'T_', 'Material':'M_', 'AnimationSequence':'A_', 'SoundWave':'S_'}\\nregistry = unreal.AssetRegistryHelpers.get_asset_registry()\\nassets = registry.get_assets_by_path(root, recursive=True)\\nflagged = []\\nfor data in assets:\\n    name = str(data.asset_name)\\n    cls = str(data.asset_class_path.asset_name)\\n    expected = prefix_by_class.get(cls)\\n    if expected and not name.startswith(expected):\\n        flagged.append((str(data.package_name), cls, expected, name))\\nprint('Asset naming audit root:', root)\\nprint('Assets scanned:', len(assets))\\nprint('Flagged assets:', len(flagged))\\nfor package, cls, expected, name in flagged[:100]:\\n    print(f'{package}: class={cls} expected={expected} name={name}')\\nif len(flagged) > 100:\\n    print(f'... {len(flagged) - 100} more flagged assets')\",\"mode\":\"ExecuteFile\",\"unattended\":true}"), *RootPath));
				}
			}
			else
			{
				return MakeExecutionResult(
					TEXT("Unknown recipeName. Supported recipes: first_person_ground_character, widget_hud, mcp_self_extension_pipeline, rts_camera, top_down_input, hud_dashboard, asset_naming_audit."),
					nullptr,
					true);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("scaffold_recipe"));
			StructuredContent->SetStringField(TEXT("recipeName"), RecipeName);
			StructuredContent->SetStringField(TEXT("title"), Title);
			StructuredContent->SetStringField(TEXT("objective"), Objective);
			StructuredContent->SetStringField(TEXT("rootPath"), RootPath);
			StructuredContent->SetStringField(TEXT("taskName"), TaskName);
			StructuredContent->SetArrayField(TEXT("steps"), Steps);
			StructuredContent->SetArrayField(TEXT("toolCallPlan"), ToolCalls);
			StructuredContent->SetArrayField(TEXT("warnings"), Warnings);
			StructuredContent->SetStringField(TEXT("suggestedNextStep"), SuggestedNextStep);

			if (bWriteMemory)
			{
				TSharedPtr<FJsonObject> MemoryArguments = MakeShared<FJsonObject>();
				MemoryArguments->SetStringField(TEXT("key"), MemoryKey);
				MemoryArguments->SetStringField(TEXT("summary"), Title);
				MemoryArguments->SetStringField(TEXT("status"), TEXT("recipe_ready"));
				MemoryArguments->SetStringField(TEXT("nextStep"), SuggestedNextStep);
				MemoryArguments->SetStringField(TEXT("contentJson"), JsonObjectToString(StructuredContent));
				MemoryArguments->SetArrayField(TEXT("tags"), MakeRecipeStringArray({ TEXT("chat"), TEXT("recipe"), RecipeName }));
				const FUnrealMcpExecutionResult MemoryResult = ProjectMemoryWrite(*MemoryArguments);
				StructuredContent->SetBoolField(TEXT("memoryWritten"), !MemoryResult.bIsError);
				StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
				StructuredContent->SetStringField(TEXT("memoryWriteText"), MemoryResult.Text);
			}
			else
			{
				StructuredContent->SetBoolField(TEXT("memoryWritten"), false);
				StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Prepared recipe '%s'. Next step: %s"), *RecipeName, *SuggestedNextStep),
				StructuredContent,
				false);
		}

		namespace
		{
		UEditorAssetSubsystem* GetScaffoldEditorAssetSubsystem()
		{
			return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		}

		bool IsGameplayScaffoldTool(const FString& ToolName)
		{
			return ToolName == TEXT("unreal.scaffold_round_system")
				|| ToolName == TEXT("unreal.scaffold_shop_system")
				|| ToolName == TEXT("unreal.scaffold_economy_system")
				|| ToolName == TEXT("unreal.scaffold_autobattler_ai")
				|| ToolName == TEXT("unreal.scaffold_result_ui");
		}

		FUnrealMcpExecutionResult ExecuteGameplayScaffoldTool(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorAssetSubsystem* EditorAssetSubsystem = GetScaffoldEditorAssetSubsystem();
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			if (ToolName == TEXT("unreal.scaffold_round_system"))
			{
				return ScaffoldRoundSystem(EditorAssetSubsystem, Arguments);
			}

			if (ToolName == TEXT("unreal.scaffold_shop_system"))
			{
				return ScaffoldShopSystem(EditorAssetSubsystem, Arguments);
			}

			if (ToolName == TEXT("unreal.scaffold_economy_system"))
			{
				return ScaffoldEconomySystem(EditorAssetSubsystem, Arguments);
			}

			if (ToolName == TEXT("unreal.scaffold_autobattler_ai"))
			{
				return ScaffoldAutobattlerAi(EditorAssetSubsystem, Arguments);
			}

			return ScaffoldResultUi(EditorAssetSubsystem, Arguments);
		}
	}

	bool TryExecuteScaffoldTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (IsGameplayScaffoldTool(ToolName))
		{
			OutResult = ExecuteGameplayScaffoldTool(ToolName, Arguments);
			return true;
		}

			if (ToolName == TEXT("unreal.scaffold_mcp_tool"))
			{
				if (IsEditorPlaying())
				{
					OutResult = MakePieBlockedResult(ToolName);
				return true;
			}

				OutResult = ScaffoldMcpTool(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.scaffold_recipe"))
			{
				OutResult = ScaffoldRecipe(Arguments);
				return true;
			}

			return false;
		}
	}
