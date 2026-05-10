#include "Providers/OpenAiChatCompletionsProvider.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "UnrealMcpAssistantRun.h"
#include "UnrealMcpToolRegistrar.h"
#include <atomic>
namespace UnrealMcp
{
	void ApplyAiHttpTimeoutOverrides(const UUnrealMcpSettings& Settings);
	TSharedPtr<FJsonObject> NormalizeOpenAiSchemaObject(const TSharedPtr<FJsonObject>& InputObject);
	bool IsOpenAiSchemaCompatibleObject(const TSharedPtr<FJsonObject>& InputObject, FString& OutReason);
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
	FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject);
}
namespace
{
	struct FChatToolCall
	{
		int32 Index = 0;
		FString Id;
		FString OpenAiFunctionName;
		FString UnrealToolName;
		FString ArgumentsJson;
	};
	FString ProviderIdForError(const FAiProviderConfig& Config)
	{
		const FString Id = Config.Id.TrimStartAndEnd();
		return Id.IsEmpty() ? TEXT("<unnamed>") : Id;
	}
	TSharedPtr<FJsonObject> ChatMessage(const FString& Role, const FString& Content)
	{
		TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), Role);
		Message->SetStringField(TEXT("content"), Content);
		return Message;
	}
	FString BytesToString(const TArray<uint8>& Bytes)
	{
		if (Bytes.IsEmpty()) { return FString(); }
		const FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converter.Length(), Converter.Get());
	}
	FString SerializeToolResult(const FUnrealMcpExecutionResult& ToolResult)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("text"), ToolResult.Text);
		Object->SetBoolField(TEXT("is_error"), ToolResult.bIsError);
		if (ToolResult.StructuredContent.IsValid()) { Object->SetObjectField(TEXT("structured_content"), ToolResult.StructuredContent); }
		return UnrealMcp::JsonObjectToString(Object);
	}
	class FChatCompletionsRun final : public IUnrealMcpAssistantHandle, public TSharedFromThis<FChatCompletionsRun, ESPMode::ThreadSafe>
	{
	public:
		FChatCompletionsRun(
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
			CachedSettings.ProviderId = Config.Id;
			CachedSettings.bEnableAiAssistant = Settings->bEnableAiAssistant;
			CachedSettings.OpenAIResponsesUrl = Config.BaseUrl;
			CachedSettings.OpenAIApiKey = Config.ApiKey;
			CachedSettings.OpenAIModel = Config.Model;
			CachedSettings.OpenAIReasoningEffort = Config.ReasoningEffort;
			CachedSettings.AiMaxToolRounds = Settings->AiMaxToolRounds;
			CachedSettings.AiMaxOutputTokens = Config.MaxOutputTokens > 0 ? Config.MaxOutputTokens : Settings->AiMaxOutputTokens;
			CachedSettings.AiRequestTimeoutSeconds = Settings->AiRequestTimeoutSeconds;
			CachedSettings.AiRequestActivityTimeoutSeconds = Settings->AiRequestActivityTimeoutSeconds;
			CachedSettings.AssistantSystemPrompt = Settings->AssistantSystemPrompt;
			if (!CachedSettings.bEnableAiAssistant)
			{
				Finish(TEXT("AI assistant is disabled. Enable it in Project Settings > Plugins > Unreal MCP > AI."), true);
				return;
			}
			UnrealMcp::ApplyAiHttpTimeoutOverrides(*Settings);
			BuildTools();
			BuildInitialMessages();
			SendModelRequest();
		}
		virtual void Cancel() override
		{
			TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> RequestToCancel;
			{ const FScopeLock Lock(&StateMutex); if (bCompleted) { return; } bCancellationRequested.store(true, std::memory_order_relaxed); RequestToCancel = ActiveRequest; }
			if (RequestToCancel.IsValid()) { RequestToCancel->CancelRequest(); }
			Finish(TEXT("Generation stopped."), false, true);
		}
		virtual bool Steer(const FString& Instruction) override
		{
			const FString Trimmed = Instruction.TrimStartAndEnd();
			if (Trimmed.IsEmpty()) { return false; }
			const FScopeLock Lock(&StateMutex);
			if (bCompleted || bCancellationRequested.load(std::memory_order_relaxed)) { return false; }
			PendingSteerInstructions.Add(Trimmed);
			return true;
		}
	private:
		void BuildTools()
		{
			TArray<TSharedPtr<FJsonValue>> McpTools;
			UnrealMcp::AppendRegisteredToolDefinitions(McpTools);
			TMap<FString, int32> SeenNames;
			for (const TSharedPtr<FJsonValue>& ToolValue : McpTools)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid()) { continue; }
				const TSharedPtr<FJsonObject> ToolObject = ToolValue->AsObject();
				FString OriginalName;
				FString Description;
				const TSharedPtr<FJsonObject>* InputSchema = nullptr;
				if (!ToolObject->TryGetStringField(TEXT("name"), OriginalName)
					|| !ToolObject->TryGetStringField(TEXT("description"), Description)
					|| !ToolObject->TryGetObjectField(TEXT("inputSchema"), InputSchema)
					|| !InputSchema
					|| !(*InputSchema).IsValid())
				{
					continue;
				}
				const FString FunctionName = MakeFunctionName(OriginalName, SeenNames);
				const TSharedPtr<FJsonObject> Schema = UnrealMcp::NormalizeOpenAiSchemaObject(*InputSchema);
				FString Reason;
				if (!UnrealMcp::IsOpenAiSchemaCompatibleObject(Schema, Reason))
				{
					UE_LOG(LogUnrealMcp, Display, TEXT("Skipping chat-compatible AI tool '%s': %s"), *OriginalName, *Reason);
					continue;
				}
				TSharedPtr<FJsonObject> Function = MakeShared<FJsonObject>();
				Function->SetStringField(TEXT("name"), FunctionName);
				Function->SetStringField(TEXT("description"), FString::Printf(TEXT("%s Original MCP tool name: %s."), *Description, *OriginalName));
				Function->SetObjectField(TEXT("parameters"), Schema);
				TSharedPtr<FJsonObject> ChatTool = MakeShared<FJsonObject>();
				ChatTool->SetStringField(TEXT("type"), TEXT("function"));
				ChatTool->SetObjectField(TEXT("function"), Function);
				OpenAiTools.Add(MakeShared<FJsonValueObject>(ChatTool));
				FunctionNameToToolName.Add(FunctionName, OriginalName);
			}
		}
		static FString MakeFunctionName(const FString& OriginalName, TMap<FString, int32>& SeenNames)
		{
			FString FunctionName = OriginalName;
			for (TCHAR& Character : FunctionName)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')) { Character = TEXT('_'); }
			}
			if (FunctionName.IsEmpty()) { FunctionName = TEXT("tool"); }
			if (FunctionName.Len() > 64) { FunctionName.LeftInline(64, EAllowShrinking::No); }
			const int32 DuplicateCount = SeenNames.FindRef(FunctionName);
			SeenNames.FindOrAdd(FunctionName) = DuplicateCount + 1;
			if (DuplicateCount <= 0) { return FunctionName; }
			const FString Suffix = FString::Printf(TEXT("_%d"), DuplicateCount + 1);
			return FunctionName.Left(FMath::Max(1, 64 - Suffix.Len())) + Suffix;
		}
		FString BuildInstructions() const
		{
			FString Instructions =
				TEXT("You are Unreal MCP AI running inside Unreal Editor. Help the user build, inspect, and modify the current Unreal project by using the provided function tools when helpful. ")
				TEXT("Prefer the smallest safe set of tool calls. Inspect before concluding for read-only questions, act directly for clear modification requests, and avoid destructive actions unless explicitly asked. ")
				TEXT("Prefer AI-safe wrapper tools before falling back to execute_python. Keep answers compact and focused on what changed or was found.");
			if (!CachedSettings.AssistantSystemPrompt.TrimStartAndEnd().IsEmpty())
			{
				Instructions += TEXT("\n\nAdditional instructions:\n");
				Instructions += CachedSettings.AssistantSystemPrompt.TrimStartAndEnd();
			}
			return Instructions;
		}
		void BuildInitialMessages()
		{
			Messages.Reset();
			Messages.Add(ChatMessage(TEXT("system"), BuildInstructions()));
			// TODO: chat/completions has no previous_response_id; persist and replay message history per provider in a future enhancement.
			static_cast<void>(PreviousResponseId);
			if (!ConversationContext.TrimStartAndEnd().IsEmpty()) { Messages.Add(ChatMessage(TEXT("user"), ConversationContext)); }
			Messages.Add(ChatMessage(TEXT("user"), UserPrompt));
		}
		void AppendPendingSteerMessages()
		{
			TArray<FString> Instructions;
			{ const FScopeLock Lock(&StateMutex); Instructions = PendingSteerInstructions; PendingSteerInstructions.Reset(); }
			if (!Instructions.IsEmpty())
			{
				Messages.Add(ChatMessage(TEXT("system"), FString::Printf(TEXT("User steering update for the current turn:\n- %s"), *FString::Join(Instructions, TEXT("\n- ")))));
			}
		}
		void ResetPerRequestState()
		{
			RawResponseBytes.Reset();
			PendingStreamBytes.Reset();
			PendingSseData.Reset();
			StreamFailureMessage.Reset();
			StreamToolCalls.Reset();
			AccumulatedAssistantText.Reset();
			bToolCallsFinished = false;
		}
		void SendModelRequest()
		{
			{ const FScopeLock Lock(&StateMutex); if (bCompleted || bCancellationRequested.load(std::memory_order_relaxed)) { return; } ResetPerRequestState(); }
			AppendPendingSteerMessages();
			TArray<TSharedPtr<FJsonValue>> MessageValues;
			for (const TSharedPtr<FJsonObject>& Message : Messages) { MessageValues.Add(MakeShared<FJsonValueObject>(Message)); }
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("model"), CachedSettings.OpenAIModel);
			Payload->SetArrayField(TEXT("messages"), MessageValues);
			Payload->SetArrayField(TEXT("tools"), OpenAiTools);
			Payload->SetStringField(TEXT("tool_choice"), TEXT("auto"));
			Payload->SetBoolField(TEXT("stream"), true);
			Payload->SetNumberField(TEXT("max_tokens"), CachedSettings.AiMaxOutputTokens);
			AddReasoningEffortIfSupported(Payload);
			StartHttpRequest(UnrealMcp::JsonObjectToString(Payload));
		}
		void AddReasoningEffortIfSupported(const TSharedPtr<FJsonObject>& Payload) const
		{
			const FString ReasoningEffort = CachedSettings.OpenAIReasoningEffort.TrimStartAndEnd();
			const FString ModelLower = CachedSettings.OpenAIModel.ToLower();
			if (!ReasoningEffort.IsEmpty() && (ModelLower.Contains(TEXT("o1")) || ModelLower.Contains(TEXT("o3")) || ModelLower.Contains(TEXT("gpt-5"))))
			{
				Payload->SetStringField(TEXT("reasoning_effort"), ReasoningEffort);
			}
		}
		void StartHttpRequest(const FString& PayloadString)
		{
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(CachedSettings.OpenAIResponsesUrl);
			Request->SetVerb(TEXT("POST"));
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CachedSettings.OpenAIApiKey));
			Request->SetTimeout(CachedSettings.AiRequestTimeoutSeconds);
			Request->SetActivityTimeout(CachedSettings.AiRequestActivityTimeoutSeconds);
			Request->SetContentAsString(PayloadString);
			TWeakPtr<FChatCompletionsRun, ESPMode::ThreadSafe> WeakRun = AsShared();
			FHttpRequestStreamDelegateV2 StreamDelegate;
			StreamDelegate.BindLambda([WeakRun](void* Ptr, int64& InOutLength)
			{
				if (const TSharedPtr<FChatCompletionsRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin()) { Pinned->ConsumeResponseBytes(Ptr, InOutLength); }
			});
			Request->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate);
			Request->OnProcessRequestComplete().BindLambda([WeakRun](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
			{
				if (const TSharedPtr<FChatCompletionsRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin()) { Pinned->HandleModelRequestFinished(HttpRequest, HttpResponse, bSucceeded); }
			});
			{ const FScopeLock Lock(&StateMutex); ActiveRequest = Request; }
			if (!Request->ProcessRequest()) { Finish(TEXT("Failed to start the HTTP request to the AI provider."), true); }
		}
		void ConsumeResponseBytes(const void* Ptr, int64 Length)
		{
			if (Length <= 0 || Ptr == nullptr) { return; }
			TArray<FString> Events;
			{ const FScopeLock Lock(&StateMutex); RawResponseBytes.Append(static_cast<const uint8*>(Ptr), Length); PendingStreamBytes.Append(static_cast<const uint8*>(Ptr), Length); DrainCompleteSseEvents(Events); }
			for (const FString& Event : Events) { ProcessSseEvent(Event); }
		}
		void DrainCompleteSseEvents(TArray<FString>& OutEvents)
		{
			int32 LineStart = 0;
			for (int32 Index = 0; Index < PendingStreamBytes.Num(); ++Index)
			{
				if (PendingStreamBytes[Index] != '\n') { continue; }
				int32 LineLength = Index - LineStart;
				if (LineLength > 0 && PendingStreamBytes[LineStart + LineLength - 1] == '\r') { --LineLength; }
				const FString Line = DecodeUtf8Line(LineStart, LineLength);
				if (Line.IsEmpty() && !PendingSseData.IsEmpty())
				{
					OutEvents.Add(PendingSseData);
					PendingSseData.Reset();
				}
				else if (Line.StartsWith(TEXT("data:"), ESearchCase::CaseSensitive))
				{
					FString DataLine = Line.Mid(5);
					DataLine.TrimStartInline();
					if (!PendingSseData.IsEmpty()) { PendingSseData += TEXT("\n"); }
					PendingSseData += DataLine;
				}
				LineStart = Index + 1;
			}
			if (LineStart > 0) { PendingStreamBytes.RemoveAt(0, LineStart, EAllowShrinking::No); }
		}
		FString DecodeUtf8Line(int32 LineStart, int32 LineLength) const
		{
			if (LineLength <= 0) { return FString(); }
			const FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(PendingStreamBytes.GetData() + LineStart), LineLength);
			return FString(Converter.Length(), Converter.Get());
		}
		void ProcessSseEvent(const FString& EventData)
		{
			if (EventData.Equals(TEXT("[DONE]"), ESearchCase::CaseSensitive)) { return; }
			TSharedPtr<FJsonObject> EventObject;
			const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
			if (!UnrealMcp::LoadJsonObject(EventData, EventObject) || !EventObject.IsValid() || !EventObject->TryGetArrayField(TEXT("choices"), Choices) || !Choices) { return; }
			for (const TSharedPtr<FJsonValue>& ChoiceValue : *Choices) { ProcessChoiceDelta(ChoiceValue); }
		}
		void ProcessChoiceDelta(const TSharedPtr<FJsonValue>& ChoiceValue)
		{
			if (!ChoiceValue.IsValid() || ChoiceValue->Type != EJson::Object || !ChoiceValue->AsObject().IsValid()) { return; }
			const TSharedPtr<FJsonObject> Choice = ChoiceValue->AsObject();
			FString FinishReason;
			Choice->TryGetStringField(TEXT("finish_reason"), FinishReason);
			if (FinishReason == TEXT("tool_calls")) { bToolCallsFinished = true; }
			const TSharedPtr<FJsonObject>* Delta = nullptr;
			if (!Choice->TryGetObjectField(TEXT("delta"), Delta) || !Delta || !(*Delta).IsValid()) { return; }
			FString ContentDelta;
			if ((*Delta)->TryGetStringField(TEXT("content"), ContentDelta) && !ContentDelta.IsEmpty())
			{
				AccumulatedAssistantText += ContentDelta;
				EmitTextDelta(ContentDelta);
			}
			const TArray<TSharedPtr<FJsonValue>>* ToolCalls = nullptr;
			if ((*Delta)->TryGetArrayField(TEXT("tool_calls"), ToolCalls) && ToolCalls)
			{
				for (int32 FallbackIndex = 0; FallbackIndex < ToolCalls->Num(); ++FallbackIndex) { AccumulateToolCall((*ToolCalls)[FallbackIndex], FallbackIndex); }
			}
		}
		void AccumulateToolCall(const TSharedPtr<FJsonValue>& ToolCallValue, int32 FallbackIndex)
		{
			if (!ToolCallValue.IsValid() || ToolCallValue->Type != EJson::Object || !ToolCallValue->AsObject().IsValid()) { return; }
			const TSharedPtr<FJsonObject> Object = ToolCallValue->AsObject();
			double IndexNumber = FallbackIndex;
			Object->TryGetNumberField(TEXT("index"), IndexNumber);
			FChatToolCall& ToolCall = StreamToolCalls.FindOrAdd(static_cast<int32>(IndexNumber));
			ToolCall.Index = static_cast<int32>(IndexNumber);
			FString Id;
			if (Object->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty()) { ToolCall.Id = Id; }
			const TSharedPtr<FJsonObject>* Function = nullptr;
			if (Object->TryGetObjectField(TEXT("function"), Function) && Function && (*Function).IsValid())
			{
				FString NamePart;
				if ((*Function)->TryGetStringField(TEXT("name"), NamePart) && !NamePart.IsEmpty()) { ToolCall.OpenAiFunctionName += NamePart; }
				FString ArgumentsPart;
				if ((*Function)->TryGetStringField(TEXT("arguments"), ArgumentsPart)) { ToolCall.ArgumentsJson += ArgumentsPart; }
			}
		}
		void HandleModelRequestFinished(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
		{
			FString BodyString;
			bool bShouldHandleToolCalls = false;
			{ const FScopeLock Lock(&StateMutex); if (bCompleted) { return; } if (ActiveRequest == HttpRequest) { ActiveRequest.Reset(); } BodyString = BytesToString(RawResponseBytes); bShouldHandleToolCalls = bToolCallsFinished && StreamToolCalls.Num() > 0; }
			if (bCancellationRequested.load(std::memory_order_relaxed)) { Finish(TEXT("Generation stopped."), false, true); }
			else if (!bSucceeded || !HttpResponse.IsValid()) { Finish(TEXT("The AI request failed before a valid HTTP response was returned."), true); }
			else if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300) { Finish(FString::Printf(TEXT("AI request failed. HTTP %d: %s"), HttpResponse->GetResponseCode(), *ExtractErrorMessage(BodyString)), true); }
			else if (!StreamFailureMessage.IsEmpty()) { Finish(StreamFailureMessage, true); }
			else if (bShouldHandleToolCalls) { HandleToolCalls(); }
			else { Finish(AccumulatedAssistantText, false); }
		}
		FString ExtractErrorMessage(const FString& BodyString) const
		{
			TSharedPtr<FJsonObject> ErrorObject;
			const TSharedPtr<FJsonObject>* Nested = nullptr;
			FString Message;
			if (UnrealMcp::LoadJsonObject(BodyString, ErrorObject)
				&& ErrorObject.IsValid()
				&& ErrorObject->TryGetObjectField(TEXT("error"), Nested)
				&& Nested
				&& (*Nested).IsValid()
				&& (*Nested)->TryGetStringField(TEXT("message"), Message)
				&& !Message.IsEmpty())
			{
				return Message;
			}
			return BodyString.IsEmpty() ? TEXT("Unknown error") : BodyString;
		}
		void HandleToolCalls()
		{
			if (++ToolRoundCount > CachedSettings.AiMaxToolRounds)
			{
				Finish(FString::Printf(TEXT("Stopped after reaching the configured AI tool round limit (%d)."), CachedSettings.AiMaxToolRounds), true);
				return;
			}
			TArray<int32> Indices;
			StreamToolCalls.GetKeys(Indices);
			Indices.Sort();
			TArray<TSharedPtr<FJsonValue>> AssistantToolCalls;
			TArray<TSharedPtr<FJsonObject>> ToolMessages;
			for (int32 Index : Indices)
			{
				FChatToolCall ToolCall = StreamToolCalls[Index];
				if (ToolCall.Id.IsEmpty()) { ToolCall.Id = FString::Printf(TEXT("call_%d"), Index); }
				ToolCall.UnrealToolName = FunctionNameToToolName.FindRef(ToolCall.OpenAiFunctionName);
				AssistantToolCalls.Add(MakeToolCallObject(ToolCall));
				ToolMessages.Add(RunToolCall(ToolCall));
			}
			TSharedPtr<FJsonObject> AssistantMessage = ChatMessage(TEXT("assistant"), AccumulatedAssistantText);
			AssistantMessage->SetArrayField(TEXT("tool_calls"), AssistantToolCalls);
			Messages.Add(AssistantMessage);
			for (const TSharedPtr<FJsonObject>& ToolMessage : ToolMessages) { Messages.Add(ToolMessage); }
			SendModelRequest();
		}
		TSharedPtr<FJsonValueObject> MakeToolCallObject(const FChatToolCall& ToolCall) const
		{
			TSharedPtr<FJsonObject> Function = MakeShared<FJsonObject>();
			Function->SetStringField(TEXT("name"), ToolCall.OpenAiFunctionName);
			Function->SetStringField(TEXT("arguments"), ToolCall.ArgumentsJson);
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), ToolCall.Id);
			Object->SetStringField(TEXT("type"), TEXT("function"));
			Object->SetObjectField(TEXT("function"), Function);
			return MakeShared<FJsonValueObject>(Object);
		}
		TSharedPtr<FJsonObject> RunToolCall(const FChatToolCall& ToolCall)
		{
			EmitToolStarted(ToolCall);
			FUnrealMcpExecutionResult Result;
			if (ToolCall.UnrealToolName.IsEmpty())
			{
				Result.Text = FString::Printf(TEXT("AI called unknown tool alias `%s`."), *ToolCall.OpenAiFunctionName);
				Result.bIsError = true;
			}
			else
			{
				TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
				if (!ToolCall.ArgumentsJson.TrimStartAndEnd().IsEmpty() && (!UnrealMcp::LoadJsonObject(ToolCall.ArgumentsJson, Arguments) || !Arguments.IsValid()))
				{
					Result.Text = FString::Printf(TEXT("Tool `%s` arguments were not valid JSON."), *ToolCall.UnrealToolName);
					Result.bIsError = true;
				}
				else
				{
					Result = Module->ExecuteToolFromEditorUI(ToolCall.UnrealToolName, *Arguments);
				}
			}
			EmitToolFinished(ToolCall, Result);
			TSharedPtr<FJsonObject> Message = ChatMessage(TEXT("tool"), SerializeToolResult(Result));
			Message->SetStringField(TEXT("tool_call_id"), ToolCall.Id);
			return Message;
		}
		void EmitTextDelta(const FString& Delta) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::TextDelta;
			Event.Text = Delta;
			EmitEvent(Event);
		}
		void EmitToolStarted(const FChatToolCall& ToolCall) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::ToolCallStarted;
			Event.ToolName = ToolCall.UnrealToolName.IsEmpty() ? ToolCall.OpenAiFunctionName : ToolCall.UnrealToolName;
			Event.ToolCallId = ToolCall.Id;
			Event.ToolArgumentsJson = ToolCall.ArgumentsJson;
			EmitEvent(Event);
		}
		void EmitToolFinished(const FChatToolCall& ToolCall, const FUnrealMcpExecutionResult& Result) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::ToolCallFinished;
			Event.ToolName = ToolCall.UnrealToolName.IsEmpty() ? ToolCall.OpenAiFunctionName : ToolCall.UnrealToolName;
			Event.ToolCallId = ToolCall.Id;
			Event.Text = Result.Text;
			Event.bIsError = Result.bIsError;
			EmitEvent(Event);
		}
		void EmitEvent(const FUnrealMcpAssistantEvent& Event) const
		{
			if (!OnEvent) { return; }
			const FUnrealMcpAssistantEvent EventCopy = Event;
			TFunction<void(const FUnrealMcpAssistantEvent&)> EventCallback = OnEvent;
			AsyncTask(ENamedThreads::GameThread, [EventCopy, EventCallback = MoveTemp(EventCallback)]() mutable
			{
				if (EventCallback) { EventCallback(EventCopy); }
			});
		}
		void Finish(const FString& Message, bool bIsError, bool bWasCancelled = false)
		{
			{ const FScopeLock Lock(&StateMutex); if (bCompleted) { return; } bCompleted = true; ActiveRequest.Reset(); }
			if (!OnComplete) { return; }
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
		UnrealMcp::FUnrealMcpAssistantSettingsCache CachedSettings;
		TArray<TSharedPtr<FJsonObject>> Messages;
		TArray<TSharedPtr<FJsonValue>> OpenAiTools;
		TMap<FString, FString> FunctionNameToToolName;
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
		FCriticalSection StateMutex;
		TArray<uint8> RawResponseBytes;
		TArray<uint8> PendingStreamBytes;
		FString PendingSseData;
		FString StreamFailureMessage;
		TMap<int32, FChatToolCall> StreamToolCalls;
		TArray<FString> PendingSteerInstructions;
		FString AccumulatedAssistantText;
		int32 ToolRoundCount = 0;
		bool bToolCallsFinished = false;
		bool bCompleted = false;
		std::atomic<bool> bCancellationRequested{false};
	};
}
namespace UnrealMcp
{
	EAiProviderKind FOpenAiChatCompletionsProvider::GetKind() const
	{
		return EAiProviderKind::OpenAiChatCompat;
	}
	bool FOpenAiChatCompletionsProvider::ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const
	{
		const FString ProviderId = ProviderIdForError(Config);
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
	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FOpenAiChatCompletionsProvider::StartTurn(
		const FAiProviderConfig& Config,
		const FUnrealMcpModule* Module,
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		const TSharedRef<FChatCompletionsRun, ESPMode::ThreadSafe> Run = MakeShared<FChatCompletionsRun, ESPMode::ThreadSafe>(
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
		TUniquePtr<IAssistantProvider> CreateOpenAiChatCompletionsProvider()
		{
			return MakeUnique<FOpenAiChatCompletionsProvider>();
		}
	}
}
