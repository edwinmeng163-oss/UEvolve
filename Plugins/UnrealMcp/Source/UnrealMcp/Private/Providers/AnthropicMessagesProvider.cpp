#include "Providers/AnthropicMessagesProvider.h"

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
	struct FAnthropicToolUse
	{
		int32 Index = 0;
		FString Id;
		FString AnthropicToolName;
		FString UnrealToolName;
		FString AccumulatedJson;
		TSharedPtr<FJsonObject> InitialInputObject;
	};

	FString ProviderIdForError(const FAiProviderConfig& Config)
	{
		const FString Id = Config.Id.TrimStartAndEnd();
		return Id.IsEmpty() ? TEXT("<unnamed>") : Id;
	}

	TSharedPtr<FJsonObject> AnthropicMessage(const FString& Role, const TArray<TSharedPtr<FJsonValue>>& ContentBlocks)
	{
		TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), Role);
		Message->SetArrayField(TEXT("content"), ContentBlocks);
		return Message;
	}

	TSharedPtr<FJsonValueObject> TextBlock(const FString& Text)
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		return MakeShared<FJsonValueObject>(Block);
	}

	TSharedPtr<FJsonObject> TextMessage(const FString& Role, const FString& Text)
	{
		TArray<TSharedPtr<FJsonValue>> Blocks;
		Blocks.Add(TextBlock(Text));
		return AnthropicMessage(Role, Blocks);
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

	class FAnthropicRun final : public IUnrealMcpAssistantHandle, public TSharedFromThis<FAnthropicRun, ESPMode::ThreadSafe>
	{
	public:
		FAnthropicRun(
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
				const FString ToolName = MakeToolName(OriginalName, SeenNames);
				const TSharedPtr<FJsonObject> Schema = UnrealMcp::NormalizeOpenAiSchemaObject(*InputSchema);
				FString Reason;
				if (!UnrealMcp::IsOpenAiSchemaCompatibleObject(Schema, Reason))
				{
					UE_LOG(LogUnrealMcp, Display, TEXT("Skipping Anthropic AI tool '%s': %s"), *OriginalName, *Reason);
					continue;
				}
				TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
				Tool->SetStringField(TEXT("name"), ToolName);
				Tool->SetStringField(TEXT("description"), FString::Printf(TEXT("%s Original MCP tool name: %s."), *Description, *OriginalName));
				Tool->SetObjectField(TEXT("input_schema"), Schema);
				AnthropicTools.Add(MakeShared<FJsonValueObject>(Tool));
				FunctionNameToToolName.Add(ToolName, OriginalName);
			}
		}

		static FString MakeToolName(const FString& OriginalName, TMap<FString, int32>& SeenNames)
		{
			FString ToolName = OriginalName;
			for (TCHAR& Character : ToolName)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')) { Character = TEXT('_'); }
			}
			if (ToolName.IsEmpty()) { ToolName = TEXT("tool"); }
			if (ToolName.Len() > 64) { ToolName.LeftInline(64, EAllowShrinking::No); }
			const int32 DuplicateCount = SeenNames.FindRef(ToolName);
			SeenNames.FindOrAdd(ToolName) = DuplicateCount + 1;
			if (DuplicateCount <= 0) { return ToolName; }
			const FString Suffix = FString::Printf(TEXT("_%d"), DuplicateCount + 1);
			return ToolName.Left(FMath::Max(1, 64 - Suffix.Len())) + Suffix;
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
			if (!AppliedSteerInstructions.IsEmpty())
			{
				Instructions += TEXT("\n\nUser steering updates for the current turn:\n- ");
				Instructions += FString::Join(AppliedSteerInstructions, TEXT("\n- "));
			}
			return Instructions;
		}

		void BuildInitialMessages()
		{
			Messages.Reset();
			// TODO: Anthropic has no previous_response_id; persist and replay message history per provider in a future enhancement.
			static_cast<void>(PreviousResponseId);
			if (!ConversationContext.TrimStartAndEnd().IsEmpty()) { Messages.Add(TextMessage(TEXT("user"), ConversationContext)); }
			Messages.Add(TextMessage(TEXT("user"), UserPrompt));
		}

		void PromotePendingSteerInstructions()
		{
			TArray<FString> Instructions;
			{ const FScopeLock Lock(&StateMutex); Instructions = PendingSteerInstructions; PendingSteerInstructions.Reset(); }
			// Anthropic keeps system instructions top-level, so steering is folded into the next request's system prompt instead of adding another user turn between tool_result messages.
			if (!Instructions.IsEmpty()) { AppliedSteerInstructions.Append(Instructions); }
		}

		void ResetPerRequestState()
		{
			RawResponseBytes.Reset();
			PendingStreamBytes.Reset();
			PendingSseData.Reset();
			StreamToolUses.Reset();
			AccumulatedAssistantText.Reset();
			bToolUseStopSeen = false;
		}

		void SendModelRequest()
		{
			{ const FScopeLock Lock(&StateMutex); if (bCompleted || bCancellationRequested.load(std::memory_order_relaxed)) { return; } ResetPerRequestState(); }
			PromotePendingSteerInstructions();

			TArray<TSharedPtr<FJsonValue>> MessageValues;
			for (const TSharedPtr<FJsonObject>& Message : Messages) { MessageValues.Add(MakeShared<FJsonValueObject>(Message)); }
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("model"), CachedSettings.OpenAIModel);
			Payload->SetStringField(TEXT("system"), BuildInstructions());
			Payload->SetArrayField(TEXT("messages"), MessageValues);
			Payload->SetArrayField(TEXT("tools"), AnthropicTools);
			Payload->SetNumberField(TEXT("max_tokens"), CachedSettings.AiMaxOutputTokens);
			Payload->SetBoolField(TEXT("stream"), true);
			TSharedPtr<FJsonObject> ToolChoice = MakeShared<FJsonObject>();
			ToolChoice->SetStringField(TEXT("type"), TEXT("auto"));
			Payload->SetObjectField(TEXT("tool_choice"), ToolChoice);
			AddThinkingIfSupported(Payload);
			StartHttpRequest(UnrealMcp::JsonObjectToString(Payload));
		}

		void AddThinkingIfSupported(const TSharedPtr<FJsonObject>& Payload) const
		{
			const FString Effort = CachedSettings.OpenAIReasoningEffort.TrimStartAndEnd().ToLower();
			const FString ModelLower = CachedSettings.OpenAIModel.ToLower();
			const bool bEffortSupported = Effort == TEXT("medium") || Effort == TEXT("high") || Effort == TEXT("xhigh");
			const bool bModelLooksThinkingCapable = ModelLower.Contains(TEXT("claude"))
				&& (ModelLower.Contains(TEXT("4")) || ModelLower.Contains(TEXT("opus")) || ModelLower.Contains(TEXT("sonnet-4")) || ModelLower.Contains(TEXT("extended")));
			if (!bEffortSupported || !bModelLooksThinkingCapable) { return; }

			TSharedPtr<FJsonObject> Thinking = MakeShared<FJsonObject>();
			Thinking->SetStringField(TEXT("type"), TEXT("enabled"));
			Thinking->SetNumberField(TEXT("budget_tokens"), FMath::Max(1, FMath::Min(CachedSettings.AiMaxOutputTokens / 2, 16384)));
			Payload->SetObjectField(TEXT("thinking"), Thinking);
		}

		void StartHttpRequest(const FString& PayloadString)
		{
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(CachedSettings.OpenAIResponsesUrl);
			Request->SetVerb(TEXT("POST"));
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
			Request->SetHeader(TEXT("x-api-key"), CachedSettings.OpenAIApiKey);
			Request->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
			Request->SetTimeout(CachedSettings.AiRequestTimeoutSeconds);
			Request->SetActivityTimeout(CachedSettings.AiRequestActivityTimeoutSeconds);
			Request->SetContentAsString(PayloadString);
			TWeakPtr<FAnthropicRun, ESPMode::ThreadSafe> WeakRun = AsShared();
			FHttpRequestStreamDelegateV2 StreamDelegate;
			StreamDelegate.BindLambda([WeakRun](void* Ptr, int64& InOutLength)
			{
				if (const TSharedPtr<FAnthropicRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin()) { Pinned->ConsumeResponseBytes(Ptr, InOutLength); }
			});
			Request->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate);
			Request->OnProcessRequestComplete().BindLambda([WeakRun](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
			{
				if (const TSharedPtr<FAnthropicRun, ESPMode::ThreadSafe> Pinned = WeakRun.Pin()) { Pinned->HandleModelRequestFinished(HttpRequest, HttpResponse, bSucceeded); }
			});
			{ const FScopeLock Lock(&StateMutex); ActiveRequest = Request; }
			if (!Request->ProcessRequest()) { Finish(TEXT("Failed to start the HTTP request to the Anthropic provider."), true); }
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
			TSharedPtr<FJsonObject> EventObject;
			if (!UnrealMcp::LoadJsonObject(EventData, EventObject) || !EventObject.IsValid()) { return; }
			FString Type;
			EventObject->TryGetStringField(TEXT("type"), Type);
			if (Type == TEXT("content_block_start")) { ProcessContentBlockStart(EventObject); }
			else if (Type == TEXT("content_block_delta")) { ProcessContentBlockDelta(EventObject); }
			else if (Type == TEXT("message_delta")) { ProcessMessageDelta(EventObject); }
			else if (Type == TEXT("error")) { Finish(FString::Printf(TEXT("Anthropic stream error: %s"), *ExtractEventError(EventObject)), true); }
		}

		void ProcessContentBlockStart(const TSharedPtr<FJsonObject>& EventObject)
		{
			const TSharedPtr<FJsonObject>* ContentBlock = nullptr;
			if (!EventObject->TryGetObjectField(TEXT("content_block"), ContentBlock) || !ContentBlock || !(*ContentBlock).IsValid()) { return; }
			FString BlockType;
			if (!(*ContentBlock)->TryGetStringField(TEXT("type"), BlockType) || BlockType != TEXT("tool_use")) { return; }
			double IndexNumber = 0.0;
			EventObject->TryGetNumberField(TEXT("index"), IndexNumber);
			FAnthropicToolUse& ToolUse = StreamToolUses.FindOrAdd(static_cast<int32>(IndexNumber));
			ToolUse.Index = static_cast<int32>(IndexNumber);
			(*ContentBlock)->TryGetStringField(TEXT("id"), ToolUse.Id);
			(*ContentBlock)->TryGetStringField(TEXT("name"), ToolUse.AnthropicToolName);
			const TSharedPtr<FJsonObject>* InputObject = nullptr;
			if ((*ContentBlock)->TryGetObjectField(TEXT("input"), InputObject) && InputObject && (*InputObject).IsValid())
			{
				ToolUse.InitialInputObject = *InputObject;
			}
		}

		void ProcessContentBlockDelta(const TSharedPtr<FJsonObject>& EventObject)
		{
			const TSharedPtr<FJsonObject>* Delta = nullptr;
			if (!EventObject->TryGetObjectField(TEXT("delta"), Delta) || !Delta || !(*Delta).IsValid()) { return; }
			FString DeltaType;
			(*Delta)->TryGetStringField(TEXT("type"), DeltaType);
			if (DeltaType == TEXT("text_delta"))
			{
				FString Text;
				if ((*Delta)->TryGetStringField(TEXT("text"), Text) && !Text.IsEmpty())
				{
					AccumulatedAssistantText += Text;
					EmitTextDelta(Text);
				}
				return;
			}
			if (DeltaType != TEXT("input_json_delta")) { return; }
			double IndexNumber = 0.0;
			EventObject->TryGetNumberField(TEXT("index"), IndexNumber);
			FAnthropicToolUse& ToolUse = StreamToolUses.FindOrAdd(static_cast<int32>(IndexNumber));
			ToolUse.Index = static_cast<int32>(IndexNumber);
			FString PartialJson;
			if ((*Delta)->TryGetStringField(TEXT("partial_json"), PartialJson))
			{
				ToolUse.AccumulatedJson += PartialJson;
			}
		}

		void ProcessMessageDelta(const TSharedPtr<FJsonObject>& EventObject)
		{
			const TSharedPtr<FJsonObject>* Delta = nullptr;
			if (!EventObject->TryGetObjectField(TEXT("delta"), Delta) || !Delta || !(*Delta).IsValid()) { return; }
			FString StopReason;
			if ((*Delta)->TryGetStringField(TEXT("stop_reason"), StopReason) && StopReason == TEXT("tool_use")) { bToolUseStopSeen = true; }
		}

		void HandleModelRequestFinished(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
		{
			FString BodyString;
			bool bShouldHandleToolCalls = false;
			{ const FScopeLock Lock(&StateMutex); if (bCompleted) { return; } if (ActiveRequest == HttpRequest) { ActiveRequest.Reset(); } BodyString = BytesToString(RawResponseBytes); bShouldHandleToolCalls = bToolUseStopSeen && StreamToolUses.Num() > 0; }
			if (bCancellationRequested.load(std::memory_order_relaxed)) { Finish(TEXT("Generation stopped."), false, true); }
			else if (!bSucceeded || !HttpResponse.IsValid()) { Finish(TEXT("The Anthropic request failed before a valid HTTP response was returned."), true); }
			else if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300) { Finish(FString::Printf(TEXT("Anthropic error %d: %s"), HttpResponse->GetResponseCode(), *ExtractErrorMessage(BodyString)), true); }
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

		FString ExtractEventError(const TSharedPtr<FJsonObject>& EventObject) const
		{
			const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
			FString Message;
			if (EventObject->TryGetObjectField(TEXT("error"), ErrorObject)
				&& ErrorObject
				&& (*ErrorObject).IsValid()
				&& (*ErrorObject)->TryGetStringField(TEXT("message"), Message)
				&& !Message.IsEmpty())
			{
				return Message;
			}
			return UnrealMcp::JsonObjectToString(EventObject);
		}

		void HandleToolCalls()
		{
			if (++ToolRoundCount > CachedSettings.AiMaxToolRounds)
			{
				Finish(FString::Printf(TEXT("Stopped after reaching the configured AI tool round limit (%d)."), CachedSettings.AiMaxToolRounds), true);
				return;
			}
			TArray<int32> Indices;
			StreamToolUses.GetKeys(Indices);
			Indices.Sort();
			TArray<TSharedPtr<FJsonValue>> AssistantContent;
			if (!AccumulatedAssistantText.TrimStartAndEnd().IsEmpty()) { AssistantContent.Add(TextBlock(AccumulatedAssistantText)); }
			TArray<TSharedPtr<FJsonValue>> ToolResultContent;
			for (int32 Index : Indices)
			{
				FAnthropicToolUse ToolUse = StreamToolUses[Index];
				if (ToolUse.Id.IsEmpty()) { ToolUse.Id = FString::Printf(TEXT("toolu_%d"), Index); }
				ToolUse.UnrealToolName = FunctionNameToToolName.FindRef(ToolUse.AnthropicToolName);
				AssistantContent.Add(MakeToolUseBlock(ToolUse));
				ToolResultContent.Add(RunToolCall(ToolUse));
			}
			Messages.Add(AnthropicMessage(TEXT("assistant"), AssistantContent));
			Messages.Add(AnthropicMessage(TEXT("user"), ToolResultContent));
			SendModelRequest();
		}

		TSharedPtr<FJsonValueObject> MakeToolUseBlock(const FAnthropicToolUse& ToolUse) const
		{
			TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
			if (!LoadToolArguments(ToolUse, InputObject) || !InputObject.IsValid()) { InputObject = MakeShared<FJsonObject>(); }
			TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
			Block->SetStringField(TEXT("type"), TEXT("tool_use"));
			Block->SetStringField(TEXT("id"), ToolUse.Id);
			Block->SetStringField(TEXT("name"), ToolUse.AnthropicToolName);
			Block->SetObjectField(TEXT("input"), InputObject);
			return MakeShared<FJsonValueObject>(Block);
		}

		TSharedPtr<FJsonValueObject> RunToolCall(const FAnthropicToolUse& ToolUse)
		{
			EmitToolStarted(ToolUse);
			FUnrealMcpExecutionResult Result;
			TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
			if (ToolUse.UnrealToolName.IsEmpty())
			{
				Result.Text = FString::Printf(TEXT("AI called unknown tool alias `%s`."), *ToolUse.AnthropicToolName);
				Result.bIsError = true;
			}
			else if (!LoadToolArguments(ToolUse, Arguments))
			{
				Result.Text = FString::Printf(TEXT("Tool `%s` arguments were not valid JSON."), *ToolUse.UnrealToolName);
				Result.bIsError = true;
			}
			else
			{
				Result = Module->ExecuteToolFromEditorUI(ToolUse.UnrealToolName, *Arguments);
			}
			EmitToolFinished(ToolUse, Result);
			TSharedPtr<FJsonObject> ResultBlock = MakeShared<FJsonObject>();
			ResultBlock->SetStringField(TEXT("type"), TEXT("tool_result"));
			ResultBlock->SetStringField(TEXT("tool_use_id"), ToolUse.Id);
			ResultBlock->SetStringField(TEXT("content"), SerializeToolResult(Result));
			ResultBlock->SetBoolField(TEXT("is_error"), Result.bIsError);
			return MakeShared<FJsonValueObject>(ResultBlock);
		}

		bool LoadToolArguments(const FAnthropicToolUse& ToolUse, TSharedPtr<FJsonObject>& OutArguments) const
		{
			const FString ArgumentsJson = GetToolArgumentsJson(ToolUse);
			if (ArgumentsJson.TrimStartAndEnd().IsEmpty())
			{
				OutArguments = MakeShared<FJsonObject>();
				return true;
			}
			return UnrealMcp::LoadJsonObject(ArgumentsJson, OutArguments) && OutArguments.IsValid();
		}

		FString GetToolArgumentsJson(const FAnthropicToolUse& ToolUse) const
		{
			if (!ToolUse.AccumulatedJson.TrimStartAndEnd().IsEmpty()) { return ToolUse.AccumulatedJson; }
			if (ToolUse.InitialInputObject.IsValid()) { return UnrealMcp::JsonObjectToString(ToolUse.InitialInputObject); }
			return TEXT("{}");
		}

		void EmitTextDelta(const FString& Delta) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::TextDelta;
			Event.Text = Delta;
			EmitEvent(Event);
		}

		void EmitToolStarted(const FAnthropicToolUse& ToolUse) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::ToolCallStarted;
			Event.ToolName = ToolUse.UnrealToolName.IsEmpty() ? ToolUse.AnthropicToolName : ToolUse.UnrealToolName;
			Event.ToolCallId = ToolUse.Id;
			Event.ToolArgumentsJson = GetToolArgumentsJson(ToolUse);
			EmitEvent(Event);
		}

		void EmitToolFinished(const FAnthropicToolUse& ToolUse, const FUnrealMcpExecutionResult& Result) const
		{
			FUnrealMcpAssistantEvent Event;
			Event.Type = EUnrealMcpAssistantEventType::ToolCallFinished;
			Event.ToolName = ToolUse.UnrealToolName.IsEmpty() ? ToolUse.AnthropicToolName : ToolUse.UnrealToolName;
			Event.ToolCallId = ToolUse.Id;
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
		TArray<TSharedPtr<FJsonValue>> AnthropicTools;
		TMap<FString, FString> FunctionNameToToolName;
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
		FCriticalSection StateMutex;
		TArray<uint8> RawResponseBytes;
		TArray<uint8> PendingStreamBytes;
		FString PendingSseData;
		TMap<int32, FAnthropicToolUse> StreamToolUses;
		TArray<FString> PendingSteerInstructions;
		TArray<FString> AppliedSteerInstructions;
		FString AccumulatedAssistantText;
		int32 ToolRoundCount = 0;
		bool bToolUseStopSeen = false;
		bool bCompleted = false;
		std::atomic<bool> bCancellationRequested{false};
	};
}

namespace UnrealMcp
{
	EAiProviderKind FAnthropicMessagesProvider::GetKind() const
	{
		return EAiProviderKind::AnthropicMessages;
	}

	bool FAnthropicMessagesProvider::ValidateConfig(const FAiProviderConfig& Config, FString& OutError) const
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

	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FAnthropicMessagesProvider::StartTurn(
		const FAiProviderConfig& Config,
		const FUnrealMcpModule* Module,
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		const TSharedRef<FAnthropicRun, ESPMode::ThreadSafe> Run = MakeShared<FAnthropicRun, ESPMode::ThreadSafe>(
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
		TUniquePtr<IAssistantProvider> CreateAnthropicMessagesProvider()
		{
			return MakeUnique<FAnthropicMessagesProvider>();
		}
	}
}
