#include "UnrealMcpModule.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Providers/IAssistantProvider.h"
#include "ToolMenus.h"
#include "UnrealMcpAssistantRun.h"
#include "UnrealMcpSession.h"
#include "UnrealMcpSkillTools.h"
#include "Runtime/Launch/Resources/Version.h"

#if !defined(ENGINE_MAJOR_VERSION) || !defined(ENGINE_MINOR_VERSION)
	#error "UnrealMcp requires Runtime/Launch/Resources/Version.h to define ENGINE_MAJOR_VERSION and ENGINE_MINOR_VERSION."
#endif
#if (ENGINE_MAJOR_VERSION < 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 6)
	#error "UnrealMcp requires Unreal Engine 5.6 or later. See Docs/Release-2026-05.md for the supported version matrix."
#endif
static_assert(
	(ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6),
	"UnrealMcp requires Unreal Engine 5.6 or later. See Docs/Release-2026-05.md for the supported version matrix.");

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
	UnrealMcp::InitializeLaunchSession();
	StartServer();
	UE_LOG(LogUnrealMcp, Display, TEXT("UnrealMcp plugin built against UE %d.%d.%d (built-time engine version)"),
		ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION);
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
	UnrealMcp::ShutdownLaunchSession();
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
