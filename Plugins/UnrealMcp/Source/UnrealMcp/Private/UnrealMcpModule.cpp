#include "UnrealMcpModule.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Providers/IAssistantProvider.h"
#include "ToolMenus.h"
#include "UnrealMcpAssistantRun.h"
#include "UnrealMcpSkillTools.h"

DEFINE_LOG_CATEGORY(LogUnrealMcp);

namespace
{
	class FNoopAssistantHandle final : public IUnrealMcpAssistantHandle
	{
	public:
		virtual void Cancel() override
		{
		}

		virtual bool Steer(const FString& Instruction) override
		{
			static_cast<void>(Instruction);
			return false;
		}
	};

	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> CompleteAssistantTurnWithError(
		FString ErrorMessage,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		AsyncTask(ENamedThreads::GameThread, [ErrorMessage = MoveTemp(ErrorMessage), OnComplete = MoveTemp(OnComplete)]() mutable
		{
			if (OnComplete)
			{
				FUnrealMcpAssistantTurnResult Result;
				Result.Text = ErrorMessage;
				Result.bIsError = true;
				OnComplete(Result);
			}
		});

		const TSharedRef<FNoopAssistantHandle, ESPMode::ThreadSafe> Handle = MakeShared<FNoopAssistantHandle, ESPMode::ThreadSafe>();
		return StaticCastSharedRef<IUnrealMcpAssistantHandle>(Handle);
	}
}

void FUnrealMcpModule::StartupModule()
{
	StartServer();
	SkillActivityTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FUnrealMcpModule::TickSkillActivity), 60.0f);
	RegisterTabSpawner();
	UToolMenus::Get()->RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealMcpModule::RegisterMenus));
}

void FUnrealMcpModule::ShutdownModule()
{
	if (SkillActivityTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SkillActivityTickerHandle);
		SkillActivityTickerHandle.Reset();
	}
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	UnregisterTabSpawner();
	StopServer();
}

bool FUnrealMcpModule::TickSkillActivity(float DeltaTime)
{
	UnrealMcp::TickSkillActivityRecorder();
	return true;
}

TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FUnrealMcpModule::ExecuteAssistantTurnAsync(
	const FString& UserPrompt,
	const FString& ConversationContext,
	const FString& PreviousResponseId,
	TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
	TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete) const
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (!Settings->bEnableAiAssistant)
	{
		return CompleteAssistantTurnWithError(
			TEXT("AI assistant is disabled. Enable it in Project Settings > Plugins > Unreal MCP > AI."),
			MoveTemp(OnComplete));
	}

	const FAiProviderConfig* ActiveProvider = Settings->FindActiveProvider();
	if (!ActiveProvider)
	{
		const FString ErrorMessage = Settings->ActiveProviderId.TrimStartAndEnd().IsEmpty()
			? TEXT("No active AI provider is configured.")
			: FString::Printf(TEXT("Active AI provider '%s' was not found."), *Settings->ActiveProviderId);
		return CompleteAssistantTurnWithError(ErrorMessage, MoveTemp(OnComplete));
	}

	FString ProviderError;
	UnrealMcp::IAssistantProvider* Provider = UnrealMcp::Providers::ResolveActiveProvider(*Settings, ProviderError);
	if (!Provider)
	{
		return CompleteAssistantTurnWithError(ProviderError, MoveTemp(OnComplete));
	}

	if (!Provider->ValidateConfig(*ActiveProvider, ProviderError))
	{
		return CompleteAssistantTurnWithError(ProviderError, MoveTemp(OnComplete));
	}

	return Provider->StartTurn(
		*ActiveProvider,
		this,
		UserPrompt,
		ConversationContext,
		PreviousResponseId,
		MoveTemp(OnEvent),
		MoveTemp(OnComplete));
}

IMPLEMENT_MODULE(FUnrealMcpModule, UnrealMcp)
