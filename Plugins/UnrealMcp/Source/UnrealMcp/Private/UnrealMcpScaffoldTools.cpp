#include "UnrealMcpScaffoldTools.h"

#include "UnrealMcpModule.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"

namespace UnrealMcp
{
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FUnrealMcpExecutionResult ScaffoldRoundSystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ScaffoldShopSystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ScaffoldEconomySystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ScaffoldAutobattlerAi(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ScaffoldResultUi(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ScaffoldMcpTool(const FJsonObject& Arguments);

	namespace
	{
		UEditorAssetSubsystem* GetEditorAssetSubsystem()
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

			UEditorAssetSubsystem* EditorAssetSubsystem = GetEditorAssetSubsystem();
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

		return false;
	}
}
