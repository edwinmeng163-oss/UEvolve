#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpSettings.h"
#include "UnrealMcpModule.h"

class FUnrealMcpModule;
class IUnrealMcpAssistantHandle;
struct FUnrealMcpAssistantEvent;
struct FUnrealMcpAssistantTurnResult;

namespace UnrealMcp
{
	class IAssistantProvider
	{
	public:
		virtual ~IAssistantProvider() = default;
		virtual EAiProviderKind GetKind() const = 0;
		virtual bool ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const = 0;
		virtual TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> StartTurn(
			const FAiProviderConfig& Config,
			const FUnrealMcpModule* Module,
			const FString& UserPrompt,
			const FString& ConversationContext,
			const FString& PreviousResponseId,
			TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
			TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete) = 0;
	};

	namespace Providers
	{
		TUniquePtr<IAssistantProvider> CreateOpenAiResponsesProvider();
		TUniquePtr<IAssistantProvider> CreateOpenAiChatCompletionsProvider();
		TUniquePtr<IAssistantProvider> CreateAnthropicMessagesProvider();
		TUniquePtr<IAssistantProvider> CreateCodexProvider();
		TUniquePtr<IAssistantProvider> CreateCodexAppServerProvider();
		// Returned pointer is owned by an internal static map; caller must NOT delete.
		IAssistantProvider* ResolveActiveProvider(const UUnrealMcpSettings& Settings, FString& OutError);
	}
}
