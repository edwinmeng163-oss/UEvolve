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

void UUnrealMcpSettings::PostInitProperties()
{
	Super::PostInitProperties();

	if (OpenAIApiKey.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	const bool bHasMigratedDefaultProvider = Providers.ContainsByPredicate(
		[](const FAiProviderConfig& Provider)
		{
			return Provider.Id == TEXT("openai-default");
		});
	if (bHasMigratedDefaultProvider || Providers.Num() != 0)
	{
		return;
	}

	FAiProviderConfig& Provider = Providers.AddDefaulted_GetRef();
	Provider.Id = TEXT("openai-default");
	Provider.DisplayName = TEXT("OpenAI (migrated)");
	Provider.Kind = EAiProviderKind::OpenAiResponses;
	Provider.BaseUrl = OpenAIResponsesUrl;
	Provider.ApiKey = OpenAIApiKey;
	Provider.Model = OpenAIModel;
	Provider.ReasoningEffort = OpenAIReasoningEffort;
	Provider.MaxOutputTokens = AiMaxOutputTokens;
	ActiveProviderId = TEXT("openai-default");

	SaveConfig();
	UE_LOG(LogTemp, Display, TEXT("[UnrealMcp] Migrated legacy OpenAI settings to Providers[0]."));
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

const FAiProviderConfig* UUnrealMcpSettings::FindActiveProvider() const
{
	for (const FAiProviderConfig& Provider : Providers)
	{
		if (Provider.Id == ActiveProviderId)
		{
			return &Provider;
		}
	}

	return nullptr;
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
