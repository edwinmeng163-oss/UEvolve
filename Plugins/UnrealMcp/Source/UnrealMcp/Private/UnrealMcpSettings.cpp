#include "UnrealMcpSettings.h"

#define LOCTEXT_NAMESPACE "UnrealMcpSettings"

UUnrealMcpSettings::UUnrealMcpSettings()
{
	CategoryName = TEXT("Plugins");
	AllowedOrigins =
	{
		TEXT("http://localhost"),
		TEXT("http://127.0.0.1"),
		TEXT("https://localhost"),
		TEXT("https://127.0.0.1")
	};
}

FName UUnrealMcpSettings::GetContainerName() const
{
	return TEXT("Project");
}

FName UUnrealMcpSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UUnrealMcpSettings::GetSectionName() const
{
	return TEXT("UnrealMcp");
}

#if WITH_EDITOR
FText UUnrealMcpSettings::GetSectionText() const
{
	return LOCTEXT("SectionText", "Unreal MCP");
}

FText UUnrealMcpSettings::GetSectionDescription() const
{
	return LOCTEXT("SectionDescription", "Runs a local MCP server inside Unreal Editor and can optionally connect the in-editor chat panel to an AI model for tool-using assistant workflows.");
}
#endif

#undef LOCTEXT_NAMESPACE
