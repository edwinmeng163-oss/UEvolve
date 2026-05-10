#pragma once

#include "Providers/IAssistantProvider.h"

namespace UnrealMcp
{
	class FOpenAiChatCompletionsProvider final : public IAssistantProvider
	{
	public:
		virtual EAiProviderKind GetKind() const override;
		virtual bool ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const override;
		virtual TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> StartTurn(
			const FAiProviderConfig& Config,
			const FUnrealMcpModule* Module,
			const FString& UserPrompt,
			const FString& ConversationContext,
			const FString& PreviousResponseId,
			TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
			TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete) override;
	};
}
