#include "Providers/CodexProvider.h"

#include "Providers/ProviderHelpers.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include <atomic>

namespace
{
	const TCHAR* ForcedCodexModel = TEXT("gpt-5.5");
	const TCHAR* ForcedCodexReasoning = TEXT("xhigh");
	const TCHAR* ForcedCodexSandbox = TEXT("workspace-write");
#if PLATFORM_WINDOWS
	const TCHAR* CodexCliWindowsUnsupportedMessage = TEXT("Codex CLI provider is not supported on Windows. Use the CodexAppServer (Codex Desktop bridge) provider instead. See Docs/Release-2026-05.md.");

	void ReportCodexCliWindowsUnsupported(FString& OutError)
	{
		OutError = CodexCliWindowsUnsupportedMessage;
		UE_LOG(LogUnrealMcp, Warning, TEXT("%s"), CodexCliWindowsUnsupportedMessage);
	}
#endif

	bool ContainsDangerousShellCharacters(const FString& Value, FString& OutFailureReason)
	{
		TArray<FString> DisallowedCharacters;
		for (const TCHAR Character : Value)
		{
			if (Character == TEXT('\n'))
			{
				DisallowedCharacters.AddUnique(TEXT("\\n"));
				continue;
			}
			if (Character == TEXT('\r'))
			{
				DisallowedCharacters.AddUnique(TEXT("\\r"));
				continue;
			}
			if (FCString::Strchr(TEXT(";|&`$()><"), Character) != nullptr)
			{
				DisallowedCharacters.AddUnique(FString::Printf(TEXT("'%c'"), Character));
			}
		}
		if (DisallowedCharacters.Num() > 0)
		{
			OutFailureReason = FString::Printf(
				TEXT("contains disallowed shell metacharacters: %s"),
				*FString::Join(DisallowedCharacters, TEXT(", ")));
			return true;
		}

		return false;
	}

	bool TokenizeExtraArgs(const FString& InExtraArgs, TArray<FString>& OutTokens, FString& OutError)
	{
		FString FailureReason;
		if (ContainsDangerousShellCharacters(InExtraArgs, FailureReason))
		{
			OutError = FString::Printf(TEXT("CodexExtraArgs %s"), *FailureReason);
			return false;
		}

		FString Current;
		bool bInSingleQuote = false;
		bool bInDoubleQuote = false;
		for (int32 Index = 0; Index < InExtraArgs.Len(); ++Index)
		{
			const TCHAR Character = InExtraArgs[Index];
			if (Character == TEXT('\'') && !bInDoubleQuote)
			{
				bInSingleQuote = !bInSingleQuote;
				continue;
			}
			if (Character == TEXT('"') && !bInSingleQuote)
			{
				bInDoubleQuote = !bInDoubleQuote;
				continue;
			}
			if (FChar::IsWhitespace(Character) && !bInSingleQuote && !bInDoubleQuote)
			{
				if (!Current.IsEmpty())
				{
					OutTokens.Add(Current);
					Current.Reset();
				}
				continue;
			}
			Current.AppendChar(Character);
		}
		if (bInSingleQuote || bInDoubleQuote)
		{
			OutError = TEXT("CodexExtraArgs contains an unterminated quoted argument.");
			return false;
		}
		if (!Current.IsEmpty())
		{
			OutTokens.Add(Current);
		}
		return true;
	}
	bool ValidateFlagValue(
		const FAiProviderConfig& Config,
		const FString& FlagName,
		const FString& Value,
		const FString& RequiredValue,
		FString& OutError)
	{
		if (!Value.Equals(RequiredValue, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Provider '%s': CodexExtraArgs must not override %s away from '%s' (got '%s')."),
				*UnrealMcp::Providers::ProviderIdForError(Config),
				*FlagName,
				*RequiredValue,
				*Value);
			return false;
		}
		return true;
	}
	bool ValidateAndFilterExtraArgs(const FAiProviderConfig& Config, TArray<FString>& OutFilteredArgs, FString& OutError)
	{
		TArray<FString> Tokens;
		if (!TokenizeExtraArgs(Config.CodexExtraArgs.TrimStartAndEnd(), Tokens, OutError))
		{
			OutError = FString::Printf(TEXT("Provider '%s': %s"), *UnrealMcp::Providers::ProviderIdForError(Config), *OutError);
			return false;
		}

		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			const FString& Token = Tokens[Index];
			auto ConsumeNextValue = [&](const FString& FlagName, FString& OutValue) -> bool
			{
				if (Index + 1 >= Tokens.Num())
				{
					OutError = FString::Printf(TEXT("Provider '%s': CodexExtraArgs flag %s requires a value."), *UnrealMcp::Providers::ProviderIdForError(Config), *FlagName);
					return false;
				}
				OutValue = Tokens[++Index];
				return true;
			};

			if (Token == TEXT("-m") || Token == TEXT("--model"))
			{
				FString Value;
				if (!ConsumeNextValue(Token, Value)) { return false; }
				if (!ValidateFlagValue(Config, Token, Value, ForcedCodexModel, OutError)) { return false; }
				continue;
			}
			if (Token.StartsWith(TEXT("--model=")))
			{
				const FString Value = Token.RightChop(8);
				if (!ValidateFlagValue(Config, TEXT("--model"), Value, ForcedCodexModel, OutError)) { return false; }
				continue;
			}
			if (Token.StartsWith(TEXT("-m")) && Token.Len() > 2)
			{
				FString Value = Token.RightChop(2);
				if (Value.StartsWith(TEXT("="))) { Value.RightChopInline(1, EAllowShrinking::No); }
				if (!ValidateFlagValue(Config, TEXT("-m"), Value, ForcedCodexModel, OutError)) { return false; }
				continue;
			}
			if (Token == TEXT("-r") || Token == TEXT("--reasoning"))
			{
				FString Value;
				if (!ConsumeNextValue(Token, Value)) { return false; }
				if (!ValidateFlagValue(Config, Token, Value, ForcedCodexReasoning, OutError)) { return false; }
				continue;
			}
			if (Token.StartsWith(TEXT("--reasoning=")))
			{
				const FString Value = Token.RightChop(12);
				if (!ValidateFlagValue(Config, TEXT("--reasoning"), Value, ForcedCodexReasoning, OutError)) { return false; }
				continue;
			}
			if (Token.StartsWith(TEXT("-r")) && Token.Len() > 2)
			{
				FString Value = Token.RightChop(2);
				if (Value.StartsWith(TEXT("="))) { Value.RightChopInline(1, EAllowShrinking::No); }
				if (!ValidateFlagValue(Config, TEXT("-r"), Value, ForcedCodexReasoning, OutError)) { return false; }
				continue;
			}
			if (Token == TEXT("--fast"))
			{
				OutError = FString::Printf(TEXT("Provider '%s': CodexExtraArgs must not use --fast because this provider forces model '%s'."), *UnrealMcp::Providers::ProviderIdForError(Config), ForcedCodexModel);
				return false;
			}
			if (Token == TEXT("-s") || Token == TEXT("--sandbox"))
			{
				FString Value;
				if (!ConsumeNextValue(Token, Value)) { return false; }
				if (!ValidateFlagValue(Config, Token, Value, ForcedCodexSandbox, OutError)) { return false; }
				continue;
			}
			if (Token.StartsWith(TEXT("--sandbox=")))
			{
				const FString Value = Token.RightChop(10);
				if (!ValidateFlagValue(Config, TEXT("--sandbox"), Value, ForcedCodexSandbox, OutError)) { return false; }
				continue;
			}
			if (Token.StartsWith(TEXT("-s")) && Token.Len() > 2)
			{
				FString Value = Token.RightChop(2);
				if (Value.StartsWith(TEXT("="))) { Value.RightChopInline(1, EAllowShrinking::No); }
				if (!ValidateFlagValue(Config, TEXT("-s"), Value, ForcedCodexSandbox, OutError)) { return false; }
				continue;
			}

			OutFilteredArgs.Add(Token);
		}
		return true;
	}
	FString QuoteForBashWordNoSpaces(const FString& Value)
	{
		FString Escaped;
		Escaped.Reserve(Value.Len() + 8);
		for (const TCHAR Character : Value)
		{
			switch (Character)
			{
			case TEXT('\\'):
				Escaped += TEXT("\\\\");
				break;
			case TEXT('\''):
				Escaped += TEXT("\\'");
				break;
			case TEXT(' '):
				Escaped += TEXT("\\x20");
				break;
			case TEXT('\t'):
				Escaped += TEXT("\\t");
				break;
			case TEXT('\n'):
				Escaped += TEXT("\\n");
				break;
			case TEXT('\r'):
				Escaped += TEXT("\\r");
				break;
			default:
				Escaped.AppendChar(Character);
				break;
			}
		}
		return FString::Printf(TEXT("$'%s'"), *Escaped);
	}
	FString JoinShellArgumentsNoSpaces(const TArray<FString>& Args)
	{
		TArray<FString> QuotedArgs;
		QuotedArgs.Reserve(Args.Num());
		for (const FString& Arg : Args)
		{
			QuotedArgs.Add(QuoteForBashWordNoSpaces(Arg));
		}
		return FString::Join(QuotedArgs, TEXT("${IFS}"));
	}
	FString ComposePrompt(const FString& UserPrompt, const FString& ConversationContext)
	{
		const FString TrimmedContext = ConversationContext.TrimStartAndEnd();
		if (TrimmedContext.IsEmpty())
		{
			return UserPrompt;
		}
		return FString::Printf(
			TEXT("--- conversation context ---\n\n%s\n\n--- latest user prompt ---\n\n%s"),
			*TrimmedContext,
			*UserPrompt);
	}
	bool LooksLikeJobId(const FString& Candidate)
	{
		const FString Trimmed = Candidate.TrimStartAndEnd();
		if (Trimmed.Len() < 4 || Trimmed.Len() > 96)
		{
			return false;
		}
		for (const TCHAR Character : Trimmed)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('-') && Character != TEXT('_'))
			{
				return false;
			}
		}
		return true;
	}
	FString ExtractLikelyJobId(const FString& Chunk)
	{
		TArray<FString> Lines;
		Chunk.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			FString Candidate;
			if (Line.Split(TEXT("Job started:"), nullptr, &Candidate, ESearchCase::IgnoreCase))
			{
				TArray<FString> Parts;
				Candidate.ParseIntoArrayWS(Parts);
				if (Parts.Num() > 0 && LooksLikeJobId(Parts[0]))
				{
					return Parts[0];
					}
				}
				const FString Trimmed = Line.TrimStartAndEnd();
			if (LooksLikeJobId(Trimmed))
			{
				return Trimmed;
			}
		}
		return FString();
	}
	class FCodexRun final : public IUnrealMcpAssistantHandle, public FRunnable, public TSharedFromThis<FCodexRun, ESPMode::ThreadSafe>
	{
	public:
		FCodexRun(
			FAiProviderConfig InConfig,
			const UUnrealMcpSettings& InSettings,
			const FUnrealMcpModule* InModule,
			FString InUserPrompt,
			FString InConversationContext,
			FString InPreviousResponseId,
			TFunction<void(const FUnrealMcpAssistantEvent&)> InOnEvent,
			TFunction<void(const FUnrealMcpAssistantTurnResult&)> InOnComplete)
			: Config(MoveTemp(InConfig))
			, Settings(&InSettings)
			, Module(InModule)
			, UserPrompt(MoveTemp(InUserPrompt))
			, ConversationContext(MoveTemp(InConversationContext))
			, PreviousResponseId(MoveTemp(InPreviousResponseId))
			, OnEvent(MoveTemp(InOnEvent))
			, OnComplete(MoveTemp(InOnComplete))
		{
		}

		virtual ~FCodexRun() override
		{
			bCancellationRequested.store(true, std::memory_order_relaxed);
			TerminateProcessIfRunning();
			if (PumpThread)
			{
				PumpThread->WaitForCompletion();
				delete PumpThread;
				PumpThread = nullptr;
			}
			CleanupProcessResources();
		}

		void Start()
		{
			SelfKeepAlive = AsShared();
			check(Settings);
			static_cast<void>(Module);
			// Codex CLI is currently single-shot: previous_response_id is ignored and any compressed chat history is folded into the prompt text.
			static_cast<void>(PreviousResponseId);

			if (!Settings->bEnableAiAssistant)
			{
				Finish(TEXT("AI assistant is disabled. Enable it in Project Settings > Plugins > Unreal MCP > AI."), true);
				return;
			}

			FString Error;
			TArray<FString> FilteredExtraArgs;
			if (!ValidateCodexConfig(Config, FilteredExtraArgs, Error))
			{
				Finish(Error, true);
				return;
			}

			if (!WritePromptTempFile(Error))
			{
				Finish(Error, true);
				return;
			}

			if (!SpawnProcess(FilteredExtraArgs, Error))
			{
				Finish(Error, true);
				return;
			}

			EmitStatus(TEXT("Started local Codex agent."));
			PumpThread = FRunnableThread::Create(this, TEXT("UnrealMcpCodexStdoutPump"));
			if (!PumpThread)
			{
				TerminateProcessIfRunning();
				Finish(TEXT("Failed to start Codex stdout pump thread."), true);
			}
		}

		virtual void Cancel() override
		{
			if (bCompleted.load(std::memory_order_acquire))
			{
				return;
			}
			bCancellationRequested.store(true, std::memory_order_relaxed);
			TerminateProcessIfRunning();
			TryKillCapturedJob();
			Finish(TEXT("Generation stopped."), false, true);
		}

		virtual bool Steer(const FString& Instruction) override
		{
			const FString Trimmed = Instruction.TrimStartAndEnd();
			if (Trimmed.IsEmpty()
				|| bCompleted.load(std::memory_order_acquire)
				|| bCancellationRequested.load(std::memory_order_relaxed))
			{
				return false;
			}

			{
				const FScopeLock Lock(&StateMutex);
				PendingSteerInstructions.Add(Trimmed);
			}
			EmitStatus(TEXT("Steering is queued for the next turn; the Codex provider does not currently support mid-run guidance."));
			return false;
		}

		virtual uint32 Run() override
		{
			while (!bCancellationRequested.load(std::memory_order_relaxed))
			{
				DrainStdoutOnce();
				if (!IsProcessRunning())
				{
					break;
				}
				FPlatformProcess::Sleep(0.05f);
			}

			for (int32 DrainAttempt = 0; DrainAttempt < 10; ++DrainAttempt)
			{
				if (!DrainStdoutOnce())
				{
					break;
				}
			}

			if (bCompleted.load(std::memory_order_acquire))
			{
				return 0;
			}

			if (bCancellationRequested.load(std::memory_order_relaxed))
			{
				Finish(TEXT("Generation stopped."), false, true);
				return 0;
			}

			int32 ReturnCode = 0;
			const bool bHasReturnCode = GetProcessReturnCode(ReturnCode);
			FString FinalText;
			{
				const FScopeLock Lock(&StateMutex);
				FinalText = AccumulatedText;
			}

			if (bHasReturnCode && ReturnCode != 0)
			{
				if (FinalText.TrimStartAndEnd().IsEmpty())
				{
					FinalText = FString::Printf(TEXT("Codex process exited with code %d."), ReturnCode);
				}
				else
				{
					FinalText += FString::Printf(TEXT("\n\nCodex process exited with code %d."), ReturnCode);
				}
				Finish(FinalText, true);
				return 0;
			}

			if (FinalText.TrimStartAndEnd().IsEmpty())
			{
				FinalText = TEXT("Codex process completed without producing output.");
			}
			Finish(FinalText, false);
			return 0;
		}

		virtual void Stop() override
		{
			bCancellationRequested.store(true, std::memory_order_relaxed);
		}

		static bool ValidateCodexConfig(const FAiProviderConfig& InConfig, TArray<FString>& OutFilteredExtraArgs, FString& OutError)
		{
#if PLATFORM_WINDOWS
			static_cast<void>(InConfig);
			static_cast<void>(OutFilteredExtraArgs);
			ReportCodexCliWindowsUnsupported(OutError);
			return false;
#else
			const FString ProviderId = UnrealMcp::Providers::ProviderIdForError(InConfig);
			const FString BinaryPath = InConfig.CodexBinaryPath.TrimStartAndEnd();
			if (BinaryPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Provider '%s': Codex binary path is empty."), *ProviderId);
				return false;
			}

			FString FailureReason;
			if (ContainsDangerousShellCharacters(BinaryPath, FailureReason))
			{
				OutError = FString::Printf(TEXT("Provider '%s': Codex binary path %s"), *ProviderId, *FailureReason);
				return false;
			}

			if (!FPaths::FileExists(BinaryPath))
			{
				OutError = FString::Printf(TEXT("Provider '%s': Codex binary path does not exist: %s"), *ProviderId, *BinaryPath);
				return false;
			}

			if (!ValidateAndFilterExtraArgs(InConfig, OutFilteredExtraArgs, OutError))
			{
				return false;
			}

			return true;
#endif
		}

	private:
		bool WritePromptTempFile(FString& OutError)
		{
			const FString PromptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp"), TEXT("codex_prompts"));
			if (!IFileManager::Get().MakeDirectory(*PromptDir, true))
			{
				OutError = FString::Printf(TEXT("Failed to create Codex prompt directory: %s"), *PromptDir);
				return false;
			}

			PromptTempFilePath = FPaths::Combine(
				PromptDir,
				FString::Printf(TEXT("%s.txt"), *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)));

			const FString PromptText = ComposePrompt(UserPrompt, ConversationContext);
			if (!FFileHelper::SaveStringToFile(PromptText, *PromptTempFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("Failed to write Codex prompt file: %s"), *PromptTempFilePath);
				return false;
			}

			return true;
		}

		bool SpawnProcess(const TArray<FString>& FilteredExtraArgs, FString& OutError)
		{
			const FString BashPath = TEXT("/bin/bash");
			if (!FPaths::FileExists(BashPath))
			{
				OutError = TEXT("Codex provider requires /bin/bash for the prompt-file fallback command.");
				return false;
			}

			if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
			{
				OutError = TEXT("Failed to create pipe for Codex stdout.");
				return false;
			}

			TArray<FString> CommandParts;
			CommandParts.Add(QuoteForBashWordNoSpaces(Config.CodexBinaryPath.TrimStartAndEnd()));
			CommandParts.Add(TEXT("start"));
			// codex-agent --help does not currently expose --prompt-file or stdin input. This fallback keeps the user prompt in a temp file, then expands it as one quoted argument inside bash.
			// v1 uses direct wait mode (-w) and pumps this process's stdout; job-control start/watch/await can replace this later for cancellation that survives editor shutdown.
			CommandParts.Add(FString::Printf(TEXT("\"$(cat${IFS}%s)\""), *QuoteForBashWordNoSpaces(PromptTempFilePath)));
			CommandParts.Add(TEXT("-m"));
			CommandParts.Add(QuoteForBashWordNoSpaces(ForcedCodexModel));
			CommandParts.Add(TEXT("-r"));
			CommandParts.Add(QuoteForBashWordNoSpaces(ForcedCodexReasoning));
			CommandParts.Add(TEXT("-s"));
			CommandParts.Add(QuoteForBashWordNoSpaces(ForcedCodexSandbox));
			CommandParts.Add(TEXT("-d"));
			CommandParts.Add(QuoteForBashWordNoSpaces(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
			CommandParts.Add(TEXT("-w"));
			CommandParts.Add(TEXT("--strip-ansi"));
			if (!FilteredExtraArgs.IsEmpty())
			{
				CommandParts.Add(JoinShellArgumentsNoSpaces(FilteredExtraArgs));
			}

			const FString Command = FString::Join(CommandParts, TEXT("${IFS}"));
			const FString Arguments = FString::Printf(TEXT("-c %s"), *Command);

			uint32 ProcessId = 0;
			ProcessHandle = FPlatformProcess::CreateProc(
				*BashPath,
				*Arguments,
				false,
				true,
				true,
				&ProcessId,
				0,
				*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
				WritePipe,
				nullptr,
				WritePipe);

			if (!ProcessHandle.IsValid())
			{
				CleanupProcessResources();
				OutError = TEXT("Failed to launch codex-agent subprocess.");
				return false;
			}

			return true;
		}

		bool DrainStdoutOnce()
		{
			if (!ReadPipe)
			{
				return false;
			}

			const FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
			if (Chunk.IsEmpty())
			{
				return false;
			}

			{
				const FScopeLock Lock(&StateMutex);
				AccumulatedText += Chunk;
				if (CapturedJobId.IsEmpty())
				{
					CapturedJobId = ExtractLikelyJobId(Chunk);
				}
			}

			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::TextDelta;
			Event.Text = Chunk;
			EmitEvent(Event);
			return true;
		}

		bool IsProcessRunning()
		{
			return ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle);
		}

		bool GetProcessReturnCode(int32& OutReturnCode)
		{
			return ProcessHandle.IsValid() && FPlatformProcess::GetProcReturnCode(ProcessHandle, &OutReturnCode);
		}

		void TerminateProcessIfRunning()
		{
			if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				FPlatformProcess::TerminateProc(ProcessHandle, true);
			}
		}

		void TryKillCapturedJob()
		{
			FString JobId;
			{
				const FScopeLock Lock(&StateMutex);
				JobId = CapturedJobId;
			}
			if (JobId.IsEmpty())
			{
				return;
			}

			const FString Args = FString::Printf(TEXT("kill %s"), *JobId);
			FProcHandle KillHandle = FPlatformProcess::CreateProc(
				*Config.CodexBinaryPath.TrimStartAndEnd(),
				*Args,
				true,
				true,
				true,
				nullptr,
				0,
				*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
				nullptr,
				nullptr);
			if (KillHandle.IsValid())
			{
				FPlatformProcess::CloseProc(KillHandle);
			}
		}

		void CleanupProcessResources()
		{
			if (ProcessHandle.IsValid())
			{
				FPlatformProcess::CloseProc(ProcessHandle);
				ProcessHandle.Reset();
			}
			if (ReadPipe || WritePipe)
			{
				FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
				ReadPipe = nullptr;
				WritePipe = nullptr;
			}
			if (!PromptTempFilePath.IsEmpty())
			{
				IFileManager::Get().Delete(*PromptTempFilePath, false, true, true);
				PromptTempFilePath.Reset();
			}
		}

		void EmitStatus(const FString& Text) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::Status;
			Event.Text = Text;
			EmitEvent(Event);
		}

		void EmitEvent(const FUnrealMcpAssistantEvent& Event) const
		{
			if (!OnEvent)
			{
				return;
			}

			const FUnrealMcpAssistantEvent EventCopy = Event;
			TFunction<void(const FUnrealMcpAssistantEvent&)> EventCallback = OnEvent;
			AsyncTask(ENamedThreads::GameThread, [EventCopy, EventCallback = MoveTemp(EventCallback)]() mutable
			{
				if (EventCallback) { EventCallback(EventCopy); }
			});
		}

		void Finish(const FString& Message, bool bIsError, bool bWasCancelled = false)
		{
			bool bExpectedCompleted = false;
			if (!bCompleted.compare_exchange_strong(bExpectedCompleted, true, std::memory_order_acq_rel))
			{
				return;
			}

			FUnrealMcpAssistantTurnResult Result;
			Result.Text = Message;
			Result.bIsError = bIsError;
			Result.bWasCancelled = bWasCancelled;

			TFunction<void(const FUnrealMcpAssistantTurnResult&)> CompleteCallback = OnComplete;
			TSharedRef<FCodexRun, ESPMode::ThreadSafe> SelfForCleanup = AsShared();
			AsyncTask(ENamedThreads::GameThread, [Result = MoveTemp(Result), CompleteCallback = MoveTemp(CompleteCallback), SelfForCleanup]() mutable
			{
				if (CompleteCallback) { CompleteCallback(Result); }
				if (SelfForCleanup->PumpThread)
				{
					SelfForCleanup->PumpThread->WaitForCompletion();
					delete SelfForCleanup->PumpThread;
					SelfForCleanup->PumpThread = nullptr;
				}
				SelfForCleanup->CleanupProcessResources();
				SelfForCleanup->SelfKeepAlive.Reset();
			});
		}

		FAiProviderConfig Config;
		const UUnrealMcpSettings* Settings = nullptr;
		const FUnrealMcpModule* Module = nullptr;
		FString UserPrompt;
		FString ConversationContext;
		FString PreviousResponseId;
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent;
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete;
		FProcHandle ProcessHandle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FRunnableThread* PumpThread = nullptr;
		FCriticalSection StateMutex;
		TSharedPtr<FCodexRun, ESPMode::ThreadSafe> SelfKeepAlive;
		TArray<FString> PendingSteerInstructions;
		FString PromptTempFilePath;
		FString CapturedJobId;
		FString AccumulatedText;
		std::atomic<bool> bCancellationRequested{false};
		std::atomic<bool> bCompleted{false};
	};
}

namespace UnrealMcp
{
	EAiProviderKind FCodexProvider::GetKind() const
	{
		return EAiProviderKind::Codex;
	}

	bool FCodexProvider::ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const
	{
		TArray<FString> FilteredExtraArgs;
		return FCodexRun::ValidateCodexConfig(Config, FilteredExtraArgs, OutError);
	}

	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FCodexProvider::StartTurn(
		const FAiProviderConfig& Config,
		const FUnrealMcpModule* Module,
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		const TSharedRef<FCodexRun, ESPMode::ThreadSafe> Run = MakeShared<FCodexRun, ESPMode::ThreadSafe>(
			Config,
			*Settings,
			Module,
			UserPrompt,
			ConversationContext,
			PreviousResponseId,
			MoveTemp(OnEvent),
			MoveTemp(OnComplete));
		Run->Start();
		return StaticCastSharedRef<IUnrealMcpAssistantHandle>(Run);
	}

	namespace Providers
	{
		TUniquePtr<IAssistantProvider> CreateCodexProvider()
		{
			return MakeUnique<FCodexProvider>();
		}
	}
}
