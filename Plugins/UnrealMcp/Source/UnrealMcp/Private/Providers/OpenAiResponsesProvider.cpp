#include "Providers/OpenAiResponsesProvider.h"
#include "Providers/ProviderHelpers.h"

#include "UnrealMcpAssistantRun.h"

namespace UnrealMcp
{
	EAiProviderKind FOpenAiResponsesProvider::GetKind() const
	{
		return EAiProviderKind::OpenAiResponses;
	}

	bool FOpenAiResponsesProvider::ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const
	{
		const FString ProviderId = UnrealMcp::Providers::ProviderIdForError(Config);
		if (Config.ApiKey.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Provider '%s': API key is empty."), *ProviderId);
			return false;
		}

		if (Config.BaseUrl.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Provider '%s': Base URL is empty."), *ProviderId);
			return false;
		}

		if (Config.Model.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Provider '%s': model is empty."), *ProviderId);
			return false;
		}

		return true;
	}

	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FOpenAiResponsesProvider::StartTurn(
		const FAiProviderConfig& Config,
		const FUnrealMcpModule* Module,
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		return UnrealMcp::CreateAssistantRun(
			Config,
			*Settings,
			Module,
			UserPrompt,
			ConversationContext,
			PreviousResponseId,
			MoveTemp(OnEvent),
			MoveTemp(OnComplete));
	}

	namespace Providers
	{
		TUniquePtr<IAssistantProvider> CreateOpenAiResponsesProvider()
		{
			return MakeUnique<FOpenAiResponsesProvider>();
		}

		IAssistantProvider* ResolveActiveProvider(const UUnrealMcpSettings& Settings, FString& OutError)
		{
			static TMap<EAiProviderKind, TUniquePtr<IAssistantProvider>> ProviderMap;
			if (ProviderMap.IsEmpty())
			{
				ProviderMap.Add(EAiProviderKind::OpenAiResponses, CreateOpenAiResponsesProvider());
				ProviderMap.Add(EAiProviderKind::OpenAiChatCompat, CreateOpenAiChatCompletionsProvider());
				ProviderMap.Add(EAiProviderKind::AnthropicMessages, CreateAnthropicMessagesProvider());
				ProviderMap.Add(EAiProviderKind::Codex, CreateCodexProvider());
				ProviderMap.Add(EAiProviderKind::CodexAppServer, CreateCodexAppServerProvider());
			}

			const FAiProviderConfig* ActiveProvider = Settings.FindActiveProvider();
			if (!ActiveProvider)
			{
				OutError = Settings.ActiveProviderId.TrimStartAndEnd().IsEmpty()
					? TEXT("No active AI provider is configured.")
					: FString::Printf(TEXT("Active AI provider '%s' was not found."), *Settings.ActiveProviderId);
				return nullptr;
			}

			const EAiProviderKind Kind = ActiveProvider->Kind;
			TUniquePtr<IAssistantProvider>* Provider = ProviderMap.Find(Kind);
			if (!Provider || Provider->Get() == nullptr)
			{
				OutError = FString::Printf(TEXT("No provider implementation registered for kind %d"), static_cast<int32>(Kind));
				return nullptr;
			}

			return Provider->Get();
		}
	}
}
