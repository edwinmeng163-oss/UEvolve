#include "Providers/CodexAppServerProvider.h"

#include "Providers/ProviderHelpers.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IWebSocket.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WebSocketsModule.h"

#include <atomic>

namespace
{
	class FCodexAppServerRun final : public IUnrealMcpAssistantHandle, public TSharedFromThis<FCodexAppServerRun, ESPMode::ThreadSafe>
	{
	public:
		FCodexAppServerRun(
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

		void Start()
		{
			check(Settings);
			static_cast<void>(Module);
			static_cast<void>(PreviousResponseId);

			if (!Settings->bEnableAiAssistant)
			{
				Finish(TEXT("AI assistant is disabled. Enable it in Project Settings > Plugins > Unreal MCP > AI."), true);
				return;
			}

			CurrentRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
			const FString BaseUrl = Config.BaseUrl.TrimStartAndEnd();

			FWebSocketsModule& WebSocketsModule = FModuleManager::Get().LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
			TSharedRef<IWebSocket> CreatedSocket = WebSocketsModule.CreateWebSocket(BaseUrl);
			TWeakPtr<FCodexAppServerRun, ESPMode::ThreadSafe> WeakRun = AsShared();
			CreatedSocket->OnConnected().AddLambda([WeakRun]()
			{
				if (const TSharedPtr<FCodexAppServerRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin())
				{
					Pinned->SendStartTurn();
				}
			});
			CreatedSocket->OnConnectionError().AddLambda([WeakRun](const FString& Error)
			{
				if (const TSharedPtr<FCodexAppServerRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin())
				{
					Pinned->HandleConnectionError(Error);
				}
			});
			CreatedSocket->OnClosed().AddLambda([WeakRun](int32 StatusCode, const FString& Reason, bool bWasClean)
			{
				if (const TSharedPtr<FCodexAppServerRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin())
				{
					Pinned->HandleSocketClosed(StatusCode, Reason, bWasClean);
				}
			});
			CreatedSocket->OnMessage().AddLambda([WeakRun](const FString& MessageString)
			{
				if (const TSharedPtr<FCodexAppServerRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin())
				{
					Pinned->HandleSocketMessage(MessageString);
				}
			});

			{
				const FScopeLock Lock(&StateMutex);
				Socket = CreatedSocket;
			}
			CreatedSocket->Connect();
		}

		virtual void Cancel() override
		{
			if (bCompleted.load(std::memory_order_acquire))
			{
				return;
			}
			bCancellationRequested.store(true, std::memory_order_relaxed);

			const TSharedPtr<IWebSocket> SocketToCancel = GetSocket();
			if (SocketToCancel.IsValid() && SocketToCancel->IsConnected())
			{
				TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("type"), TEXT("cancel"));
				Payload->SetStringField(TEXT("requestId"), CurrentRequestId);
				SocketToCancel->Send(UnrealMcp::JsonObjectToString(Payload));
				SocketToCancel->Close();
			}

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

			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("type"), TEXT("steer"));
			Payload->SetStringField(TEXT("requestId"), CurrentRequestId);
			Payload->SetStringField(TEXT("instruction"), Trimmed);
			return SendJson(Payload);
		}

	private:
		void SendStartTurn()
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("type"), TEXT("start_turn"));
			Payload->SetStringField(TEXT("requestId"), CurrentRequestId);
			Payload->SetStringField(TEXT("prompt"), UserPrompt);
			Payload->SetStringField(TEXT("context"), ConversationContext);
			if (!SendJson(Payload))
			{
				Finish(TEXT("Codex App Server bridge WebSocket is not connected; could not start turn."), true);
			}
		}

		void HandleSocketMessage(const FString& MessageString)
		{
			if (bCompleted.load(std::memory_order_acquire))
			{
				return;
			}

			TSharedPtr<FJsonObject> MessageObject;
			if (!UnrealMcp::LoadJsonObject(MessageString, MessageObject) || !MessageObject.IsValid())
			{
				Finish(TEXT("Codex App Server bridge sent invalid JSON."), true);
				return;
			}

			FString Type;
			if (!MessageObject->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
			{
				Finish(TEXT("Codex App Server bridge sent a message without a type field."), true);
				return;
			}

			if (Type == TEXT("health"))
			{
				HandleHealthMessage(MessageObject);
				return;
			}

			if (!MatchesCurrentRequest(MessageObject))
			{
				return;
			}

			if (Type == TEXT("text_delta"))
			{
				HandleTextDelta(MessageObject);
			}
			else if (Type == TEXT("tool_started"))
			{
				HandleToolStarted(MessageObject);
			}
			else if (Type == TEXT("tool_finished"))
			{
				HandleToolFinished(MessageObject);
			}
			else if (Type == TEXT("turn_complete"))
			{
				HandleTurnComplete(MessageObject);
			}
			else if (Type == TEXT("error"))
			{
				FString Message;
				MessageObject->TryGetStringField(TEXT("message"), Message);
				Finish(Message.TrimStartAndEnd().IsEmpty() ? TEXT("Codex App Server bridge returned an error.") : Message, true);
			}
		}

		void HandleConnectionError(const FString& Error)
		{
			if (bCancellationRequested.load(std::memory_order_relaxed))
			{
				Finish(TEXT("Generation stopped."), false, true);
				return;
			}
			const FString TrimmedError = Error.TrimStartAndEnd();
			const FString Reason = TrimmedError.IsEmpty() ? FString(TEXT("unknown WebSocket connection error")) : TrimmedError;
			Finish(FString::Printf(TEXT("Failed to connect to Codex App Server bridge at %s: %s"), *Config.BaseUrl.TrimStartAndEnd(), *Reason), true);
		}

		void HandleSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
		{
			if (bCompleted.load(std::memory_order_acquire))
			{
				return;
			}
			if (bCancellationRequested.load(std::memory_order_relaxed))
			{
				Finish(TEXT("Generation stopped."), false, true);
				return;
			}

			const FString TrimmedReason = Reason.TrimStartAndEnd();
			const FString CloseReason = TrimmedReason.IsEmpty()
				? FString(bWasClean ? TEXT("clean close without turn_complete") : TEXT("no close reason"))
				: TrimmedReason;
			Finish(FString::Printf(TEXT("Codex App Server bridge WebSocket closed before turn completion (code %d, reason: %s)."), StatusCode, *CloseReason), true);
		}

		void HandleHealthMessage(const TSharedPtr<FJsonObject>& MessageObject)
		{
			FString State;
			MessageObject->TryGetStringField(TEXT("state"), State);
			if (State == TEXT("ready"))
			{
				return;
			}

			const FString DisplayState = State.IsEmpty() ? FString(TEXT("starting")) : State;
			EmitStatus(FString::Printf(TEXT("Codex App Server bridge state: %s"), *DisplayState));
			if (State == TEXT("failed"))
			{
				Finish(TEXT("Codex App Server bridge health failed."), true);
			}
		}

		void HandleTextDelta(const TSharedPtr<FJsonObject>& MessageObject)
		{
			FString Text;
			if (!MessageObject->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
			{
				return;
			}
			{
				const FScopeLock Lock(&StateMutex);
				AccumulatedText += Text;
			}
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::TextDelta;
			Event.Text = Text;
			EmitEvent(Event);
		}

		void HandleToolStarted(const TSharedPtr<FJsonObject>& MessageObject)
		{
			FString ToolName;
			FString ToolCallId;
			MessageObject->TryGetStringField(TEXT("toolName"), ToolName);
			MessageObject->TryGetStringField(TEXT("toolCallId"), ToolCallId);

			FString ArgumentsJson = TEXT("{}");
			const TSharedPtr<FJsonObject>* ArgsObject = nullptr;
			if (MessageObject->TryGetObjectField(TEXT("args"), ArgsObject) && ArgsObject && (*ArgsObject).IsValid())
			{
				ArgumentsJson = UnrealMcp::JsonObjectToString(*ArgsObject);
			}

			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::ToolCallStarted;
			Event.ToolName = ToolName;
			Event.ToolCallId = ToolCallId;
			Event.ToolArgumentsJson = ArgumentsJson;
			EmitEvent(Event);
		}

		void HandleToolFinished(const TSharedPtr<FJsonObject>& MessageObject)
		{
			FString ToolCallId;
			FString Text;
			bool bIsError = false;
			MessageObject->TryGetStringField(TEXT("toolCallId"), ToolCallId);
			MessageObject->TryGetStringField(TEXT("text"), Text);
			MessageObject->TryGetBoolField(TEXT("isError"), bIsError);

			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::ToolCallFinished;
			Event.ToolCallId = ToolCallId;
			Event.Text = Text;
			Event.bIsError = bIsError;
			EmitEvent(Event);
		}

		void HandleTurnComplete(const TSharedPtr<FJsonObject>& MessageObject)
		{
			FString FullText;
			MessageObject->TryGetStringField(TEXT("fullText"), FullText);
			if (FullText.IsEmpty())
			{
				const FScopeLock Lock(&StateMutex);
				FullText = AccumulatedText;
			}
			Finish(FullText, false);
		}

		bool MatchesCurrentRequest(const TSharedPtr<FJsonObject>& MessageObject) const
		{
			FString RequestId;
			if (!MessageObject->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return true;
			}
			return RequestId.Equals(CurrentRequestId, ESearchCase::CaseSensitive);
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

		bool SendJson(const TSharedPtr<FJsonObject>& Payload)
		{
			const TSharedPtr<IWebSocket> SocketToSend = GetSocket();
			if (!SocketToSend.IsValid() || !SocketToSend->IsConnected())
			{
				return false;
			}
			SocketToSend->Send(UnrealMcp::JsonObjectToString(Payload));
			return true;
		}

		TSharedPtr<IWebSocket> GetSocket() const
		{
			const FScopeLock Lock(&StateMutex);
			return Socket;
		}

		void Finish(const FString& Message, bool bIsError, bool bWasCancelled = false)
		{
			bool bExpectedCompleted = false;
			if (!bCompleted.compare_exchange_strong(bExpectedCompleted, true, std::memory_order_acq_rel))
			{
				return;
			}

			TSharedPtr<IWebSocket> SocketToClose;
			{
				const FScopeLock Lock(&StateMutex);
				SocketToClose = Socket;
				Socket.Reset();
			}
			if (SocketToClose.IsValid() && SocketToClose->IsConnected())
			{
				SocketToClose->Close();
			}

			FUnrealMcpAssistantTurnResult Result;
			Result.Text = Message;
			Result.bIsError = bIsError;
			Result.bWasCancelled = bWasCancelled;

			TFunction<void(const FUnrealMcpAssistantTurnResult&)> CompleteCallback = OnComplete;
			AsyncTask(ENamedThreads::GameThread, [Result = MoveTemp(Result), CompleteCallback = MoveTemp(CompleteCallback)]() mutable
			{
				if (CompleteCallback) { CompleteCallback(Result); }
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
		TSharedPtr<IWebSocket> Socket;
		FString CurrentRequestId;
		FString AccumulatedText;
		std::atomic<bool> bCancellationRequested{false};
		std::atomic<bool> bCompleted{false};
		mutable FCriticalSection StateMutex;
	};
}

namespace UnrealMcp
{
	EAiProviderKind FCodexAppServerProvider::GetKind() const
	{
		return EAiProviderKind::CodexAppServer;
	}

	bool FCodexAppServerProvider::ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const
	{
		const FString ProviderId = UnrealMcp::Providers::ProviderIdForError(Config);
		const FString BaseUrl = Config.BaseUrl.TrimStartAndEnd();
		if (BaseUrl.IsEmpty()
			|| (!BaseUrl.StartsWith(TEXT("ws://"), ESearchCase::IgnoreCase)
				&& !BaseUrl.StartsWith(TEXT("wss://"), ESearchCase::IgnoreCase)))
		{
			OutError = FString::Printf(TEXT("Provider '%s': BaseUrl must be a WebSocket URL (ws:// or wss://)."), *ProviderId);
			return false;
		}

		if (!Config.Model.TrimStartAndEnd().IsEmpty() || !Config.ReasoningEffort.TrimStartAndEnd().IsEmpty())
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("Provider '%s': Model and ReasoningEffort are ignored by the Codex App Server bridge; the bridge hard-codes gpt-5.5 with xhigh reasoning."), *ProviderId);
		}
		return true;
	}

	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FCodexAppServerProvider::StartTurn(
		const FAiProviderConfig& Config,
		const FUnrealMcpModule* Module,
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		const TSharedRef<FCodexAppServerRun, ESPMode::ThreadSafe> Run = MakeShared<FCodexAppServerRun, ESPMode::ThreadSafe>(
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
		TUniquePtr<IAssistantProvider> CreateCodexAppServerProvider()
		{
			return MakeUnique<FCodexAppServerProvider>();
		}
	}
}
