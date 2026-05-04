#include "UnrealMcpModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "ToolMenus.h"
#include "UnrealMcpChatPanel.h"
#include "UnrealMcpWorkbenchPanel.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

namespace UnrealMcp
{
	static const FName ChatTabName(TEXT("UnrealMcp.Chat"));
	static const FName WorkbenchTabName(TEXT("UnrealMcp.Workbench"));
}

void FUnrealMcpModule::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		UnrealMcp::ChatTabName,
		FOnSpawnTab::CreateRaw(this, &FUnrealMcpModule::SpawnChatTab))
		.SetDisplayName(LOCTEXT("ChatTabTitle", "Unreal MCP Chat"))
		.SetTooltipText(LOCTEXT("ChatTabTooltip", "Open the Unreal MCP command chat window."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		UnrealMcp::WorkbenchTabName,
		FOnSpawnTab::CreateRaw(this, &FUnrealMcpModule::SpawnWorkbenchTab))
		.SetDisplayName(LOCTEXT("WorkbenchTabTitle", "Unreal MCP Workbench"))
		.SetTooltipText(LOCTEXT("WorkbenchTabTooltip", "Open the Unreal MCP self-extension workbench."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FUnrealMcpModule::UnregisterTabSpawner()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UnrealMcp::ChatTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UnrealMcp::WorkbenchTabName);
	}
}

void FUnrealMcpModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window")))
	{
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("UnrealMcp"));
		Section.AddMenuEntry(
			TEXT("OpenUnrealMcpChat"),
			LOCTEXT("OpenChatMenuLabel", "Unreal MCP Chat"),
			LOCTEXT("OpenChatMenuTooltip", "Open the Unreal MCP command chat window."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMcpModule::OpenChatTab)));
		Section.AddMenuEntry(
			TEXT("OpenUnrealMcpWorkbench"),
			LOCTEXT("OpenWorkbenchMenuLabel", "Unreal MCP Workbench"),
			LOCTEXT("OpenWorkbenchMenuTooltip", "Open the thin self-extension workbench console."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMcpModule::OpenWorkbenchTab)));
	}
}

void FUnrealMcpModule::OpenChatTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UnrealMcp::ChatTabName);
}

void FUnrealMcpModule::OpenWorkbenchTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UnrealMcp::WorkbenchTabName);
}

TSharedRef<SDockTab> FUnrealMcpModule::SpawnChatTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab =
		SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SUnrealMcpChatPanel, this)
		];

	return Tab;
}

TSharedRef<SDockTab> FUnrealMcpModule::SpawnWorkbenchTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab =
		SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SUnrealMcpWorkbenchPanel, this)
		];

	return Tab;
}

#undef LOCTEXT_NAMESPACE
