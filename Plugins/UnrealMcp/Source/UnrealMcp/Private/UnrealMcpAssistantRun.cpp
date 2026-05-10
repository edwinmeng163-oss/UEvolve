#include "UnrealMcpAssistantRun.h"

#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealMcpMemoryTools.h"
#include "UnrealMcpSettings.h"

#include <atomic>

namespace UnrealMcp
{
	void ApplyAiHttpTimeoutOverrides(const UUnrealMcpSettings& Settings);
	TSharedPtr<FJsonObject> NormalizeOpenAiSchemaObject(const TSharedPtr<FJsonObject>& InputObject);
	bool IsOpenAiSchemaCompatibleObject(const TSharedPtr<FJsonObject>& InputObject, FString& OutReason);
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
	TSharedPtr<FJsonObject> MakeEmptyObject();
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject);
	FString GetJsonStringAtPath(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> PathSegments);
	FString ExtractOpenAiResponseFailureDetails(const TSharedPtr<FJsonObject>& ResponseObject);
}

class FUnrealMcpAssistantRun final
	: public IUnrealMcpAssistantHandle
	, public TSharedFromThis<FUnrealMcpAssistantRun, ESPMode::ThreadSafe>
{
public:
	FUnrealMcpAssistantRun(
		FAiProviderConfig InProviderConfig,
		const UUnrealMcpSettings& InSettings,
		const FUnrealMcpModule* InModule,
		FString InUserPrompt,
		FString InConversationContext,
		FString InPreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> InOnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> InOnComplete)
		: ProviderConfig(MoveTemp(InProviderConfig))
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
		CachedSettings.ProviderId = ProviderConfig.Id;
		CachedSettings.bEnableAiAssistant = Settings->bEnableAiAssistant;
		CachedSettings.OpenAIResponsesUrl = ProviderConfig.BaseUrl;
		CachedSettings.OpenAIApiKey = ProviderConfig.ApiKey;
		CachedSettings.OpenAIModel = ProviderConfig.Model;
		CachedSettings.OpenAIReasoningEffort = ProviderConfig.ReasoningEffort;
		CachedSettings.AiMaxToolRounds = Settings->AiMaxToolRounds;
		CachedSettings.AiMaxOutputTokens = ProviderConfig.MaxOutputTokens > 0
			? ProviderConfig.MaxOutputTokens
			: Settings->AiMaxOutputTokens;
		CachedSettings.AiRequestTimeoutSeconds = Settings->AiRequestTimeoutSeconds;
		CachedSettings.AiRequestActivityTimeoutSeconds = Settings->AiRequestActivityTimeoutSeconds;
		CachedSettings.AssistantSystemPrompt = Settings->AssistantSystemPrompt;

		if (!CachedSettings.bEnableAiAssistant)
		{
			Finish(TEXT("AI assistant is disabled. Enable it in Project Settings > Plugins > Unreal MCP > AI."), FString(), true);
			return;
		}

		if (CachedSettings.OpenAIApiKey.TrimStartAndEnd().IsEmpty())
		{
			const FString ProviderId = GetProviderIdForError();
			Finish(FString::Printf(TEXT("Provider '%s': API key is empty."), *ProviderId), FString(), true);
			return;
		}

		if (CachedSettings.OpenAIModel.TrimStartAndEnd().IsEmpty())
		{
			const FString ProviderId = GetProviderIdForError();
			Finish(FString::Printf(TEXT("Provider '%s': model is empty."), *ProviderId), FString(), true);
			return;
		}

		if (CachedSettings.OpenAIResponsesUrl.TrimStartAndEnd().IsEmpty())
		{
			const FString ProviderId = GetProviderIdForError();
			Finish(FString::Printf(TEXT("Provider '%s': Base URL is empty."), *ProviderId), FString(), true);
			return;
		}

		UnrealMcp::ApplyAiHttpTimeoutOverrides(*Settings);

		BuildOpenAiTools();
		SendModelRequest(BuildInitialInput(), PreviousResponseId);
	}

	virtual void Cancel() override
	{
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> RequestToCancel;
		FString ResponseId;
		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted)
			{
				return;
			}

			bCancellationRequested.store(true, std::memory_order_relaxed);
			RequestToCancel = ActiveRequest;
			ResponseId = ActiveResponseId;
		}

		if (RequestToCancel.IsValid())
		{
			RequestToCancel->CancelRequest();
		}

		Finish(TEXT("Generation stopped."), ResponseId, false, true);
	}

	virtual bool Steer(const FString& Instruction) override
	{
		const FString TrimmedInstruction = Instruction.TrimStartAndEnd();
		if (TrimmedInstruction.IsEmpty())
		{
			return false;
		}

		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted || bCancellationRequested.load(std::memory_order_relaxed))
			{
				return false;
			}

			PendingSteerInstructions.Add(CollapseForActiveTaskMemory(TrimmedInstruction, MaxSteerInstructionChars));
			while (PendingSteerInstructions.Num() > MaxPendingSteerInstructions)
			{
				PendingSteerInstructions.RemoveAt(0);
			}
		}

		RememberActiveTask(
			TEXT("in_progress_steered"),
			TEXT("Apply queued /steer guidance at the next model continuation or tool-result turn, then verify before more changes."),
			TEXT("chat_steer"));
		EmitStatus(FString::Printf(TEXT("Queued steer guidance for the next AI continuation: %s"), *CollapseForActiveTaskMemory(TrimmedInstruction, 180)));
		return true;
	}

private:
	struct FAssistantToolCall
	{
		FString OpenAiFunctionName;
		FString UnrealToolName;
		FString CallId;
		FString ArgumentsJson;
	};

	FString GetProviderIdForError() const
	{
		const FString TrimmedId = CachedSettings.ProviderId.TrimStartAndEnd();
		return TrimmedId.IsEmpty() ? TEXT("<unnamed>") : TrimmedId;
	}

	void EmitEvent(const FUnrealMcpAssistantEvent& Event) const
	{
		if (OnEvent)
		{
			const FUnrealMcpAssistantEvent EventCopy = Event;
			TFunction<void(const FUnrealMcpAssistantEvent&)> EventCallback = OnEvent;
			AsyncTask(ENamedThreads::GameThread, [EventCopy, EventCallback = MoveTemp(EventCallback)]() mutable
			{
				if (EventCallback)
				{
					EventCallback(EventCopy);
				}
			});
		}
	}

	void EmitStatus(const FString& Message, bool bIsError = false) const
	{
		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::Status;
		Event.Text = Message;
		Event.bIsError = bIsError;
		EmitEvent(Event);
	}

	void EmitTextDelta(const FString& Delta) const
	{
		if (Delta.IsEmpty())
		{
			return;
		}

		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::TextDelta;
		Event.Text = Delta;
		EmitEvent(Event);
	}

	void EmitToolStarted(const FAssistantToolCall& ToolCall) const
	{
		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::ToolCallStarted;
		Event.ToolName = ToolCall.UnrealToolName;
		Event.ToolCallId = ToolCall.CallId;
		Event.ToolArgumentsJson = ToolCall.ArgumentsJson;
		EmitEvent(Event);
	}

	void EmitToolFinished(const FAssistantToolCall& ToolCall, const FUnrealMcpExecutionResult& ToolResult) const
	{
		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::ToolCallFinished;
		Event.ToolName = ToolCall.UnrealToolName;
		Event.ToolCallId = ToolCall.CallId;
		Event.ToolArgumentsJson = ToolCall.ArgumentsJson;
		Event.Text = ToolResult.Text;
		Event.bIsError = ToolResult.bIsError;
		EmitEvent(Event);
	}

	static FString CollapseForActiveTaskMemory(const FString& Text, int32 MaxChars)
	{
		FString Collapsed = Text;
		Collapsed.ReplaceInline(TEXT("\r"), TEXT(" "));
		Collapsed.ReplaceInline(TEXT("\n"), TEXT(" "));
		Collapsed = Collapsed.TrimStartAndEnd();
		return Collapsed.Len() > MaxChars ? Collapsed.Left(MaxChars) + TEXT(" ...[truncated]") : Collapsed;
	}

	static FString StripUnrealLogTailFromAiFailure(const FString& Message)
	{
		FString CleanMessage = Message.TrimStartAndEnd();
		if (CleanMessage.IsEmpty())
		{
			return CleanMessage;
		}

		int32 BestLogStart = INDEX_NONE;
		auto ConsiderNeedle = [&CleanMessage, &BestLogStart](const FString& Needle)
		{
			int32 SearchStart = 0;
			while (true)
			{
				const int32 Index = CleanMessage.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
				if (Index == INDEX_NONE)
				{
					break;
				}

				const FString TailPreview = CleanMessage.Mid(Index, 420);
				if (TailPreview.Contains(TEXT("Log"), ESearchCase::CaseSensitive))
				{
					BestLogStart = BestLogStart == INDEX_NONE ? Index : FMath::Min(BestLogStart, Index);
					break;
				}

				SearchStart = Index + Needle.Len();
			}
		};

		ConsiderNeedle(TEXT("\n["));
		ConsiderNeedle(TEXT("\r\n["));
		ConsiderNeedle(TEXT("\\n["));

		if (BestLogStart == INDEX_NONE)
		{
			return CleanMessage;
		}

		return CleanMessage.Left(BestLogStart).TrimStartAndEnd()
			+ TEXT("\n\nUnreal Editor log lines were omitted from this AI transport error. Use Tool Log or unreal.tail_log if you need the full editor log.");
	}

	FString BuildAiTransportFailureMessage(const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest) const
	{
		if (!HttpRequest.IsValid())
		{
			return TEXT("The AI request failed before a valid HTTP response was returned. The HTTP request object was not available. Check network connectivity, API endpoint settings, and retry.");
		}

		const EHttpRequestStatus::Type RequestStatus = HttpRequest->GetStatus();
		const EHttpFailureReason FailureReason = HttpRequest->GetFailureReason();
		const FString RequestStatusText = EHttpRequestStatus::ToString(RequestStatus);
		const FString FailureReasonText = LexToString(FailureReason);

		if (FailureReason == EHttpFailureReason::TimedOut)
		{
			return FString::Printf(
				TEXT("AI request timed out after %.0f seconds. Increase AI Request Timeout Seconds in Project Settings > Plugins > Unreal MCP > AI if you expect long planning turns, or retry with a smaller task."),
				CachedSettings.AiRequestTimeoutSeconds);
		}

		if (FailureReason == EHttpFailureReason::Cancelled)
		{
			return TEXT("The AI request was cancelled before completion.");
		}

		if (FailureReason == EHttpFailureReason::ConnectionError)
		{
			return TEXT("The AI request failed because the connection to the AI provider could not be completed. Check network/VPN/proxy settings, then use Test AI or retry.");
		}

		if (RequestStatus == EHttpRequestStatus::Failed && FailureReasonText.Equals(TEXT("Other"), ESearchCase::IgnoreCase))
		{
			return TEXT("The AI request lost its network connection before OpenAI returned a valid response. This is usually transient network/VPN/proxy/sleep or editor-load pressure, not a Blueprint or PIE error. Retry after the editor settles, or press Test AI to verify the configured endpoint/model/key. UE HTTP status: Failed, failure reason: Other.");
		}

		if (RequestStatus == EHttpRequestStatus::Failed)
		{
			return FString::Printf(
				TEXT("The AI request failed before a valid HTTP response was returned. Request status: %s. Failure reason: %s. Check network/VPN/proxy settings, endpoint URL, and retry."),
				*RequestStatusText,
				*FailureReasonText);
		}

		return FString::Printf(
			TEXT("The AI request ended without a valid HTTP response. Request status: %s. Failure reason: %s. Check network connectivity and retry."),
			*RequestStatusText,
			*FailureReasonText);
	}

	void AddRecentToolSummary(const FAssistantToolCall& ToolCall, const FUnrealMcpExecutionResult& ToolResult)
	{
		RecentToolSummaries.Add(FString::Printf(
			TEXT("%s [%s]: %s"),
			*ToolCall.UnrealToolName,
			ToolResult.bIsError ? TEXT("error") : TEXT("ok"),
			*CollapseForActiveTaskMemory(ToolResult.Text, 360)));

		while (RecentToolSummaries.Num() > MaxActiveTaskToolSummaries)
		{
			RecentToolSummaries.RemoveAt(0);
		}
	}

	void RememberActiveTask(const FString& Status, const FString& NextStep, const FString& Trigger)
	{
		TSharedPtr<FJsonObject> ContentObject = MakeShared<FJsonObject>();
		ContentObject->SetStringField(TEXT("trigger"), Trigger);
		ContentObject->SetStringField(TEXT("userPrompt"), UserPrompt);
		ContentObject->SetStringField(TEXT("previousResponseId"), PreviousResponseId);
		ContentObject->SetStringField(TEXT("activeResponseId"), ActiveResponseId);
		ContentObject->SetNumberField(TEXT("toolRoundCount"), ToolRoundCount);
		ContentObject->SetNumberField(TEXT("maxToolRounds"), CachedSettings.AiMaxToolRounds);
		ContentObject->SetBoolField(TEXT("hadToolError"), bHadToolError);
		ContentObject->SetBoolField(TEXT("nearToolRoundLimit"), bRememberedNearToolRoundLimit);
		ContentObject->SetStringField(TEXT("assistantDraft"), CollapseForActiveTaskMemory(AccumulatedAssistantText, 1200));

		TArray<TSharedPtr<FJsonValue>> ToolSummaryValues;
		for (const FString& Summary : RecentToolSummaries)
		{
			ToolSummaryValues.Add(MakeShared<FJsonValueString>(Summary));
		}
		ContentObject->SetArrayField(TEXT("recentToolSummaries"), ToolSummaryValues);

		TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("key"), TEXT("chat.active_task"));
		Arguments->SetStringField(TEXT("summary"), CollapseForActiveTaskMemory(UserPrompt, 240));
		Arguments->SetStringField(TEXT("status"), Status);
		Arguments->SetStringField(TEXT("nextStep"), NextStep);
		Arguments->SetStringField(TEXT("contentJson"), UnrealMcp::JsonObjectToString(ContentObject));

		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(TEXT("chat")));
		Tags.Add(MakeShared<FJsonValueString>(TEXT("active_task")));
		Tags.Add(MakeShared<FJsonValueString>(TEXT("resume")));
		Arguments->SetArrayField(TEXT("tags"), Tags);

		(void)UnrealMcp::ProjectMemoryWrite(*Arguments);
	}

	void MaybeRememberTerminalActiveTask(bool bIsError, bool bWasCancelled)
	{
		if (bPausedAtToolRoundLimit)
		{
			return;
		}

		if (bWasCancelled)
		{
			RememberActiveTask(
				TEXT("cancelled"),
				TEXT("Read project memory key chat.active_task, review recentToolSummaries, then continue from the last verified step."),
				TEXT("chat_cancelled"));
			return;
		}

		if (ToolRoundCount <= 0)
		{
			return;
		}

		if (bIsError)
		{
			RememberActiveTask(
				TEXT("stopped_with_error"),
				TEXT("Read project memory key chat.active_task, classify the error if needed, then resume with a smaller verified step."),
				TEXT("chat_error"));
			return;
		}

		if (bHadToolError || ToolRoundCount >= LongTaskMemoryRoundThreshold)
		{
			RememberActiveTask(
				TEXT("completed"),
				TEXT("Use this as the latest completed checkpoint if the user asks to continue the same editor task."),
				TEXT("chat_completed_checkpoint"));
		}
	}

	void Finish(const FString& Message, const FString& ResponseId, bool bIsError, bool bWasCancelled = false)
	{
		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted)
			{
				return;
			}

			bCompleted = true;
			ActiveRequest.Reset();
		}

		MaybeRememberTerminalActiveTask(bIsError, bWasCancelled);

		if (OnComplete)
		{
			FUnrealMcpAssistantTurnResult Result;
			Result.Text = Message;
			Result.ResponseId = ResponseId;
			Result.bIsError = bIsError;
			Result.bWasCancelled = bWasCancelled;
			TFunction<void(const FUnrealMcpAssistantTurnResult&)> CompleteCallback = OnComplete;
			AsyncTask(ENamedThreads::GameThread, [Result = MoveTemp(Result), CompleteCallback = MoveTemp(CompleteCallback)]() mutable
			{
				if (CompleteCallback)
				{
					CompleteCallback(Result);
				}
			});
		}
	}

	void ResetPerRequestState()
	{
		RawResponseBytes.Reset();
		PendingStreamBytes.Reset();
		PendingSseData.Reset();
		CompletedResponseObject.Reset();
		StreamFailureMessage.Reset();
		StreamToolCalls.Reset();
		bResponseIncompleteDueToMaxOutputTokens = false;
	}

	static FString BytesToString(const TArray<uint8>& Bytes)
	{
		if (Bytes.IsEmpty())
		{
			return FString();
		}

		const FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converter.Length(), Converter.Get());
	}

	void BuildOpenAiTools()
	{
		OpenAiTools.Reset();
		FunctionNameToToolName.Reset();

		TArray<TSharedPtr<FJsonValue>> MccTools;
		Module->AppendToolDefinitions(MccTools);

		TMap<FString, int32> SeenFunctionNames;

		for (const TSharedPtr<FJsonValue>& ToolValue : MccTools)
		{
			if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject> ToolObject = ToolValue->AsObject();
			FString OriginalToolName;
			FString Description;
			const TSharedPtr<FJsonObject>* InputSchema = nullptr;
			if (!ToolObject->TryGetStringField(TEXT("name"), OriginalToolName)
				|| !ToolObject->TryGetStringField(TEXT("description"), Description)
				|| !ToolObject->TryGetObjectField(TEXT("inputSchema"), InputSchema)
				|| !InputSchema
				|| !(*InputSchema).IsValid())
			{
				continue;
			}

			FString FunctionName = OriginalToolName;
			for (TCHAR& Character : FunctionName)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
				{
					Character = TEXT('_');
				}
			}

			if (FunctionName.IsEmpty())
			{
				FunctionName = TEXT("tool");
			}

			if (FunctionName.Len() > 64)
			{
				FunctionName.LeftInline(64, EAllowShrinking::No);
			}

			const int32 DuplicateCount = SeenFunctionNames.FindRef(FunctionName);
			SeenFunctionNames.FindOrAdd(FunctionName) = DuplicateCount + 1;
			if (DuplicateCount > 0)
			{
				const FString Suffix = FString::Printf(TEXT("_%d"), DuplicateCount + 1);
				const int32 MaxBaseLength = FMath::Max(1, 64 - Suffix.Len());
				FunctionName = FunctionName.Left(MaxBaseLength) + Suffix;
			}

				TSharedPtr<FJsonObject> OpenAiTool = MakeShared<FJsonObject>();
				OpenAiTool->SetStringField(TEXT("type"), TEXT("function"));
				OpenAiTool->SetStringField(TEXT("name"), FunctionName);
				OpenAiTool->SetStringField(
					TEXT("description"),
					FString::Printf(TEXT("%s Original MCP tool name: %s."), *Description, *OriginalToolName));

				const TSharedPtr<FJsonObject> NormalizedSchema = UnrealMcp::NormalizeOpenAiSchemaObject(*InputSchema);
				FString SchemaCompatibilityReason;
				if (!UnrealMcp::IsOpenAiSchemaCompatibleObject(NormalizedSchema, SchemaCompatibilityReason))
				{
					UE_LOG(
						LogUnrealMcp,
						Display,
						TEXT("Skipping AI tool '%s' because its schema is not compatible with the OpenAI function interface: %s"),
						*OriginalToolName,
						*SchemaCompatibilityReason);
					continue;
				}

				OpenAiTool->SetObjectField(TEXT("parameters"), NormalizedSchema);

				OpenAiTools.Add(MakeShared<FJsonValueObject>(OpenAiTool));
				FunctionNameToToolName.Add(FunctionName, OriginalToolName);
			}
	}

	FString BuildAssistantInstructions() const
	{
		FString Instructions =
			TEXT("You are Unreal MCP AI running inside Unreal Editor. ")
			TEXT("Help the user build, inspect, and modify the current Unreal project by using the provided function tools when they are helpful. ")
			TEXT("Prefer the smallest safe set of tool calls. ")
			TEXT("For read-only questions, inspect first before concluding. ")
			TEXT("For modifications, act directly when the user clearly asked for a change. ")
			TEXT("Avoid destructive actions such as deleting actors unless the user explicitly asked for that result. ")
			TEXT("Prefer AI-safe wrapper tools such as spawn_actor_basic, spawn_actor_batch_basic, spawn_static_mesh_actor, batch_set_actor_scale, batch_set_actor_tags, batch_set_point_light_properties, batch_configure_static_mesh_actors, bp_* Blueprint graph editing tools, widget_* UMG editing tools, scaffold_recipe, workflow_run, scaffold_mcp_tool, and mcp_* self-extension tools before falling back to execute_python. ")
			TEXT("Self-extension capability briefing: from the first turn, assume this plugin can inspect its registered tools, scaffold new MCP tools, validate schemas, apply descriptor-first patches, build the editor target, run tool tests, roll back failed extensions, and store continuation memory. ")
			TEXT("Use mcp_workbench_status or mcp_tool_audit to discover current coverage, policies, handlers, tests, and health. ")
			TEXT("For uncertain tasks, use tool_recommend and knowledge_search before inventing new tools; if knowledge_search reports a missing index, run knowledge_index_refresh and retry the search. ")
			TEXT("When the user asks for a high-level or repeatable workflow, prefer scaffold_recipe to generate a bounded recipe and workflow_run to dry-run or execute a sequence of existing tools. ")
			TEXT("workflow_run defaults to dryRun=true; run it as a plan first, then execute with dryRun=false only when the requested changes, risks, and verification steps are clear. ")
			TEXT("For new MCP capabilities, follow the safe self-extension gate: preview_change_plan, scaffold_mcp_tool, mcp_validate_tool_schema, mcp_apply_scaffold dryRun, mcp_apply_scaffold real apply, mcp_build_editor, editor restart if needed, mcp_run_tool_test or mcp_run_test_suite, then verify_task_outcome. ")
			TEXT("If a long task pauses, fails, or approaches the tool-round limit, read or write project memory key chat.active_task and continue from the smallest verified next step. ")
			TEXT("Keep answers compact by default and avoid repeating the user's prompt. ")
			TEXT("When a task is blocked because no suitable tool exists, say so plainly and suggest the closest supported path. ")
			TEXT("After tool use, give a concise final answer focused on what you changed or found.");

		if (!CachedSettings.AssistantSystemPrompt.TrimStartAndEnd().IsEmpty())
		{
			Instructions += TEXT("\n\nAdditional instructions:\n");
			Instructions += CachedSettings.AssistantSystemPrompt.TrimStartAndEnd();
		}

		return Instructions;
	}

	TSharedPtr<FJsonValueObject> BuildInputMessage(const FString& Text) const
	{
		TSharedPtr<FJsonObject> TextObject = MakeShared<FJsonObject>();
		TextObject->SetStringField(TEXT("type"), TEXT("input_text"));
		TextObject->SetStringField(TEXT("text"), Text);

		TArray<TSharedPtr<FJsonValue>> ContentArray;
		ContentArray.Add(MakeShared<FJsonValueObject>(TextObject));

		TSharedPtr<FJsonObject> MessageObject = MakeShared<FJsonObject>();
		MessageObject->SetStringField(TEXT("role"), TEXT("user"));
		MessageObject->SetArrayField(TEXT("content"), ContentArray);

		return MakeShared<FJsonValueObject>(MessageObject);
	}

	TArray<TSharedPtr<FJsonValue>> BuildUserInput(const FString& Text) const
	{
		TArray<TSharedPtr<FJsonValue>> InputArray;
		InputArray.Add(BuildInputMessage(Text));
		return InputArray;
	}

	TArray<TSharedPtr<FJsonValue>> BuildInitialInput() const
	{
		TArray<TSharedPtr<FJsonValue>> InputArray;
		if (!ConversationContext.TrimStartAndEnd().IsEmpty())
		{
			InputArray.Add(BuildInputMessage(ConversationContext));
		}
		InputArray.Add(BuildInputMessage(UserPrompt));
		return InputArray;
	}

	TArray<FString> DrainPendingSteerInstructions()
	{
		TArray<FString> Instructions;
		{
			const FScopeLock Lock(&StateMutex);
			Instructions = PendingSteerInstructions;
			PendingSteerInstructions.Reset();
		}
		return Instructions;
	}

	bool HasPendingSteerInstructions()
	{
		const FScopeLock Lock(&StateMutex);
		return PendingSteerInstructions.Num() > 0;
	}

	bool AppendPendingSteerInput(TArray<TSharedPtr<FJsonValue>>& InputArray)
	{
		const TArray<FString> Instructions = DrainPendingSteerInstructions();
		if (Instructions.IsEmpty())
		{
			return false;
		}

		TArray<FString> Lines;
		Lines.Add(TEXT("User steering update for the current AI turn. Treat this as higher priority than the earlier plan, while preserving verified work and avoiding unnecessary rewrites."));
		for (const FString& Instruction : Instructions)
		{
			Lines.Add(FString::Printf(TEXT("- %s"), *Instruction));
		}

		InputArray.Add(BuildInputMessage(FString::Join(Lines, TEXT("\n"))));
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> BuildContinuationInput()
	{
		TArray<TSharedPtr<FJsonValue>> InputArray = BuildUserInput(
			TEXT("Continue from exactly where you left off. ")
			TEXT("Do not repeat prior completed text unless a very short bridge is needed. ")
			TEXT("Keep the answer concise and finish the response. ")
			TEXT("If more tool use is required, continue using tools."));
		AppendPendingSteerInput(InputArray);
		return InputArray;
	}

	TArray<TSharedPtr<FJsonValue>> BuildSteeredFollowupInput()
	{
		TArray<TSharedPtr<FJsonValue>> InputArray = BuildUserInput(
			TEXT("Apply the queued user steering update to the current answer. ")
			TEXT("If the previous answer already went in the wrong direction, correct course concisely. ")
			TEXT("Do not redo verified tool work unless the steering explicitly requires it."));
		AppendPendingSteerInput(InputArray);
		return InputArray;
	}

	void SendModelRequest(const TArray<TSharedPtr<FJsonValue>>& InputItems, const FString& PriorResponseId)
	{
		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted || bCancellationRequested.load(std::memory_order_relaxed))
			{
				return;
			}

			ResetPerRequestState();
			LastRequestInputItems = InputItems;
			LastRequestPriorResponseId = PriorResponseId;
		}

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("model"), CachedSettings.OpenAIModel);
		Payload->SetStringField(TEXT("instructions"), BuildAssistantInstructions());
		Payload->SetArrayField(TEXT("input"), InputItems);
		Payload->SetArrayField(TEXT("tools"), OpenAiTools);
		Payload->SetStringField(TEXT("tool_choice"), TEXT("auto"));
		Payload->SetBoolField(TEXT("parallel_tool_calls"), true);
		Payload->SetBoolField(TEXT("stream"), true);
		Payload->SetStringField(TEXT("truncation"), TEXT("auto"));
		Payload->SetNumberField(TEXT("max_output_tokens"), CachedSettings.AiMaxOutputTokens);

		const FString ReasoningEffort = CachedSettings.OpenAIReasoningEffort.TrimStartAndEnd();
		if (!ReasoningEffort.IsEmpty())
		{
			TSharedPtr<FJsonObject> ReasoningObject = MakeShared<FJsonObject>();
			ReasoningObject->SetStringField(TEXT("effort"), ReasoningEffort);
			Payload->SetObjectField(TEXT("reasoning"), ReasoningObject);
		}

		if (!PriorResponseId.TrimStartAndEnd().IsEmpty())
		{
			Payload->SetStringField(TEXT("previous_response_id"), PriorResponseId);
		}

		FString PayloadString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
		FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(CachedSettings.OpenAIResponsesUrl);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CachedSettings.OpenAIApiKey));
		Request->SetTimeout(CachedSettings.AiRequestTimeoutSeconds);
		Request->SetActivityTimeout(CachedSettings.AiRequestActivityTimeoutSeconds);
		Request->SetContentAsString(PayloadString);

		FHttpRequestStreamDelegateV2 StreamDelegate;
		TWeakPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> WeakRun = AsShared();
		StreamDelegate.BindLambda(
			[WeakRun](void* Ptr, int64& InOutLength)
			{
				if (const TSharedPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> PinnedThis = WeakRun.Pin())
				{
					PinnedThis->ConsumeResponseBytes(Ptr, InOutLength);
				}
			});
		Request->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate);

		Request->OnProcessRequestComplete().BindLambda(
			[WeakRun](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
			{
				if (const TSharedPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> PinnedThis = WeakRun.Pin())
				{
					PinnedThis->HandleModelRequestFinished(HttpRequest, HttpResponse, bSucceeded);
				}
			});

		{
			const FScopeLock Lock(&StateMutex);
			ActiveRequest = Request;
		}

		if (!Request->ProcessRequest())
		{
			Finish(TEXT("Failed to start the HTTP request to the AI provider."), FString(), true, false);
		}
	}

	static bool IsMissingPreviousResponseError(const FString& ErrorMessage)
	{
		return ErrorMessage.Contains(TEXT("previous response"), ESearchCase::IgnoreCase)
			&& ErrorMessage.Contains(TEXT("not found"), ESearchCase::IgnoreCase);
	}

	void ConsumeResponseBytes(const void* Ptr, int64 Length)
	{
		if (Length <= 0 || Ptr == nullptr)
		{
			return;
		}

		TArray<FString> CompletedEvents;
		{
			const FScopeLock Lock(&StateMutex);
			RawResponseBytes.Append(static_cast<const uint8*>(Ptr), Length);
			PendingStreamBytes.Append(static_cast<const uint8*>(Ptr), Length);

			int32 LineStart = 0;
			for (int32 Index = 0; Index < PendingStreamBytes.Num(); ++Index)
			{
				if (PendingStreamBytes[Index] != '\n')
				{
					continue;
				}

				int32 LineLength = Index - LineStart;
				if (LineLength > 0 && PendingStreamBytes[LineStart + LineLength - 1] == '\r')
				{
					--LineLength;
				}

				FString Line;
				if (LineLength > 0)
				{
					const FUTF8ToTCHAR Converter(
						reinterpret_cast<const UTF8CHAR*>(PendingStreamBytes.GetData() + LineStart),
						LineLength);
					Line = FString(Converter.Length(), Converter.Get());
				}

				if (Line.IsEmpty())
				{
					if (!PendingSseData.IsEmpty())
					{
						CompletedEvents.Add(PendingSseData);
						PendingSseData.Reset();
					}
				}
				else if (Line.StartsWith(TEXT("data:"), ESearchCase::CaseSensitive))
				{
					FString DataLine = Line.Mid(5);
					DataLine.TrimStartInline();
					if (!PendingSseData.IsEmpty())
					{
						PendingSseData += TEXT("\n");
					}
					PendingSseData += DataLine;
				}

				LineStart = Index + 1;
			}

			if (LineStart > 0)
			{
				PendingStreamBytes.RemoveAt(0, LineStart, EAllowShrinking::No);
			}
		}

		for (const FString& EventData : CompletedEvents)
		{
			if (EventData.Equals(TEXT("[DONE]"), ESearchCase::CaseSensitive))
			{
				continue;
			}

			TSharedPtr<FJsonObject> EventObject;
			if (!UnrealMcp::LoadJsonObject(EventData, EventObject) || !EventObject.IsValid())
			{
				continue;
			}

			HandleStreamEvent(EventObject);
		}
	}

	void HandleStreamEvent(const TSharedPtr<FJsonObject>& EventObject)
	{
		if (!EventObject.IsValid())
		{
			return;
		}

		FString EventType;
		EventObject->TryGetStringField(TEXT("type"), EventType);
		if (EventType.IsEmpty())
		{
			return;
		}

		if (EventType == TEXT("response.created"))
		{
			const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
			if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && (*ResponseObject).IsValid())
			{
				FString ResponseId;
				if ((*ResponseObject)->TryGetStringField(TEXT("id"), ResponseId))
				{
					const FScopeLock Lock(&StateMutex);
					ActiveResponseId = ResponseId;
				}
			}

			return;
		}

		if (EventType == TEXT("response.output_text.delta"))
		{
			FString Delta;
			if (EventObject->TryGetStringField(TEXT("delta"), Delta) && !Delta.IsEmpty())
			{
				{
					const FScopeLock Lock(&StateMutex);
					AccumulatedAssistantText += Delta;
				}
				EmitTextDelta(Delta);
			}

			return;
		}

		if (EventType == TEXT("response.function_call_arguments.done"))
		{
			FAssistantToolCall ToolCall;
			if (!EventObject->TryGetStringField(TEXT("name"), ToolCall.OpenAiFunctionName)
				|| !EventObject->TryGetStringField(TEXT("call_id"), ToolCall.CallId))
			{
				return;
			}

			const FString* ToolName = FunctionNameToToolName.Find(ToolCall.OpenAiFunctionName);
			if (!ToolName)
			{
				const FScopeLock Lock(&StateMutex);
				StreamFailureMessage = FString::Printf(TEXT("AI called unknown tool alias `%s`."), *ToolCall.OpenAiFunctionName);
				return;
			}

			ToolCall.UnrealToolName = *ToolName;
			EventObject->TryGetStringField(TEXT("arguments"), ToolCall.ArgumentsJson);

			bool bShouldEmit = false;
			{
				const FScopeLock Lock(&StateMutex);
				bShouldEmit = !StreamToolCalls.Contains(ToolCall.CallId);
				StreamToolCalls.Add(ToolCall.CallId, ToolCall);
			}

			if (bShouldEmit)
			{
				EmitToolStarted(ToolCall);
			}

			return;
		}

		if (EventType == TEXT("response.completed"))
		{
			const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
			if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && (*ResponseObject).IsValid())
			{
				const FScopeLock Lock(&StateMutex);
				CompletedResponseObject = MakeShared<FJsonObject>(**ResponseObject);
				(*ResponseObject)->TryGetStringField(TEXT("id"), ActiveResponseId);
			}

			return;
		}

		if (EventType == TEXT("error"))
		{
			FString ErrorMessage;
			if (!EventObject->TryGetStringField(TEXT("message"), ErrorMessage))
			{
				const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
				if (EventObject->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && (*ErrorObject).IsValid())
				{
					(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
				}
			}

			const FScopeLock Lock(&StateMutex);
			StreamFailureMessage = ErrorMessage.IsEmpty() ? TEXT("The AI provider returned a streaming error.") : ErrorMessage;
			return;
		}

		if (EventType == TEXT("response.failed") || EventType == TEXT("response.incomplete"))
		{
			FString ErrorMessage;
			bool bTreatAsSoftIncomplete = false;
			const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
			if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && (*ResponseObject).IsValid())
			{
				FString StatusDetails;
				(*ResponseObject)->TryGetStringField(TEXT("status"), StatusDetails);
				const FString FailureDetails = UnrealMcp::ExtractOpenAiResponseFailureDetails(*ResponseObject);
				const FString IncompleteReason = UnrealMcp::GetJsonStringAtPath(*ResponseObject, { TEXT("incomplete_details"), TEXT("reason") });
				bTreatAsSoftIncomplete =
					EventType == TEXT("response.incomplete")
					&& (IncompleteReason.Equals(TEXT("max_output_tokens"), ESearchCase::IgnoreCase)
						|| FailureDetails.Equals(TEXT("max_output_tokens"), ESearchCase::IgnoreCase));

				{
					const FScopeLock Lock(&StateMutex);
					CompletedResponseObject = MakeShared<FJsonObject>(**ResponseObject);
					(*ResponseObject)->TryGetStringField(TEXT("id"), ActiveResponseId);
					bResponseIncompleteDueToMaxOutputTokens = bTreatAsSoftIncomplete;
				}

				if (!bTreatAsSoftIncomplete)
				{
					if (!StatusDetails.IsEmpty() && !FailureDetails.IsEmpty())
					{
						ErrorMessage = FString::Printf(TEXT("The AI response ended with status `%s`: %s"), *StatusDetails, *FailureDetails);
					}
					else if (!StatusDetails.IsEmpty())
					{
						ErrorMessage = FString::Printf(TEXT("The AI response ended with status `%s`."), *StatusDetails);
					}
					else if (!FailureDetails.IsEmpty())
					{
						ErrorMessage = FailureDetails;
					}
				}
			}

			if (!bTreatAsSoftIncomplete)
			{
				const FScopeLock Lock(&StateMutex);
				StreamFailureMessage = ErrorMessage.IsEmpty() ? TEXT("The AI response ended before completion.") : ErrorMessage;
			}

			return;
		}
	}

	void HandleModelRequestFinished(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
	{
		TSharedPtr<FJsonObject> CompletedObjectCopy;
		FString StreamFailureMessageCopy;
		FString ResponseIdCopy;
		FString BodyString;

		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted)
			{
				return;
			}

			if (ActiveRequest == HttpRequest)
			{
				ActiveRequest.Reset();
			}

			CompletedObjectCopy = CompletedResponseObject;
			StreamFailureMessageCopy = StreamFailureMessage;
			ResponseIdCopy = ActiveResponseId;
			BodyString = BytesToString(RawResponseBytes);
		}

		if (CompletedObjectCopy.IsValid())
		{
			ProcessCompletedResponseObject(CompletedObjectCopy, BodyString, HttpResponse.IsValid() ? HttpResponse->GetResponseCode() : 0);
			return;
		}

		if (bCancellationRequested.load(std::memory_order_relaxed))
		{
			Finish(TEXT("Generation stopped."), ResponseIdCopy, false, true);
			return;
		}

		if (!bSucceeded || !HttpResponse.IsValid())
		{
			const FString TransportFailureMessage = BuildAiTransportFailureMessage(HttpRequest);
			const FString SanitizedStreamFailureMessage = StripUnrealLogTailFromAiFailure(StreamFailureMessageCopy);

			if (!TransportFailureMessage.IsEmpty())
			{
				Finish(TransportFailureMessage, ResponseIdCopy, true, false);
			}
			else if (!SanitizedStreamFailureMessage.IsEmpty())
			{
				Finish(SanitizedStreamFailureMessage, ResponseIdCopy, true, false);
			}
			else
			{
				Finish(TEXT("The AI request failed before a valid HTTP response was returned."), ResponseIdCopy, true, false);
			}
			return;
		}

		const int32 ResponseCode = HttpResponse->GetResponseCode();
		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			FString ErrorMessage = BodyString;
			TSharedPtr<FJsonObject> ErrorObject;
			if (UnrealMcp::LoadJsonObject(BodyString, ErrorObject) && ErrorObject.IsValid())
			{
				const TSharedPtr<FJsonObject>* NestedErrorObject = nullptr;
				if (ErrorObject->TryGetObjectField(TEXT("error"), NestedErrorObject) && NestedErrorObject && (*NestedErrorObject).IsValid())
				{
					(*NestedErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
				}
			}

			if (ErrorMessage.IsEmpty())
			{
				ErrorMessage = StreamFailureMessageCopy;
			}

			TArray<TSharedPtr<FJsonValue>> RetryInputItems;
			bool bRetryWithoutPreviousResponse = false;
			{
				const FScopeLock Lock(&StateMutex);
				bRetryWithoutPreviousResponse =
					ResponseCode == 400
					&& !bRetriedWithoutPreviousResponse
					&& !LastRequestPriorResponseId.TrimStartAndEnd().IsEmpty()
					&& IsMissingPreviousResponseError(ErrorMessage);

				if (bRetryWithoutPreviousResponse)
				{
					bRetriedWithoutPreviousResponse = true;
					RetryInputItems = LastRequestInputItems;
					ActiveResponseId.Reset();
				}
			}

			if (bRetryWithoutPreviousResponse)
			{
				EmitStatus(TEXT("The saved AI conversation response id expired. Resetting the AI conversation state and retrying once."));
				SendModelRequest(RetryInputItems, FString());
				return;
			}

			Finish(FString::Printf(TEXT("AI request failed. HTTP %d: %s"), ResponseCode, *ErrorMessage), ResponseIdCopy, true, false);
			return;
		}

		if (!StreamFailureMessageCopy.IsEmpty())
		{
			Finish(StreamFailureMessageCopy, ResponseIdCopy, true, false);
			return;
		}

		TSharedPtr<FJsonObject> ResponseObject;
		if (!BodyString.IsEmpty() && UnrealMcp::LoadJsonObject(BodyString, ResponseObject) && ResponseObject.IsValid())
		{
			ProcessCompletedResponseObject(ResponseObject, BodyString, ResponseCode);
			return;
		}

		Finish(TEXT("The AI stream ended without a completed response object."), ResponseIdCopy, true, false);
	}

	void ProcessCompletedResponseObject(const TSharedPtr<FJsonObject>& ResponseObject, const FString& FallbackBody, int32 ResponseCode)
	{
		if (!ResponseObject.IsValid())
		{
			Finish(FString::Printf(TEXT("Failed to parse the AI response body. HTTP %d"), ResponseCode), FString(), true, false);
			return;
		}

		FString ResponseId;
		ResponseObject->TryGetStringField(TEXT("id"), ResponseId);
		if (!ResponseId.IsEmpty())
		{
			const FScopeLock Lock(&StateMutex);
			ActiveResponseId = ResponseId;
		}

		FString StreamFailureMessageCopy;
		bool bResponseIncompleteDueToMaxTokensCopy = false;
		{
			const FScopeLock Lock(&StateMutex);
			StreamFailureMessageCopy = StreamFailureMessage;
			bResponseIncompleteDueToMaxTokensCopy = bResponseIncompleteDueToMaxOutputTokens;
		}

		if (!StreamFailureMessageCopy.IsEmpty())
		{
			Finish(StreamFailureMessageCopy, ResponseId, true, false);
			return;
		}

		TArray<FAssistantToolCall> ExtractedToolCalls;
		FString FinalText;
		FString ParseFailureReason;
		if (!ExtractAssistantOutput(*ResponseObject, ExtractedToolCalls, FinalText, ParseFailureReason))
		{
			Finish(ParseFailureReason, ResponseId, true, false);
			return;
		}

		TArray<FAssistantToolCall> ToolCalls;
		{
			const FScopeLock Lock(&StateMutex);
			for (const FAssistantToolCall& ToolCall : ExtractedToolCalls)
			{
				const bool bAlreadyKnown = StreamToolCalls.Contains(ToolCall.CallId);
				StreamToolCalls.Add(ToolCall.CallId, ToolCall);
				if (!bAlreadyKnown)
				{
					EmitToolStarted(ToolCall);
				}
			}

			StreamToolCalls.GenerateValueArray(ToolCalls);

			if (AccumulatedAssistantText.IsEmpty() && !FinalText.IsEmpty())
			{
				AccumulatedAssistantText = FinalText;
				EmitTextDelta(FinalText);
			}
		}

		if (ToolCalls.Num() == 0)
		{
			FString FinalMessage;
			{
				const FScopeLock Lock(&StateMutex);
				FinalMessage = AccumulatedAssistantText;
			}

			if (FinalMessage.TrimStartAndEnd().IsEmpty())
			{
				FinalMessage = FinalText;
			}

			if (FinalMessage.TrimStartAndEnd().IsEmpty())
			{
				FinalMessage = FallbackBody;
			}

			if (bResponseIncompleteDueToMaxTokensCopy)
			{
				if (AutoContinuationCount < MaxAutoContinuationCount)
				{
					++AutoContinuationCount;
					EmitStatus(
						FString::Printf(
							TEXT("AI hit the output token limit and is continuing automatically (%d/%d)."),
							AutoContinuationCount,
							MaxAutoContinuationCount));
					SendModelRequest(BuildContinuationInput(), ResponseId);
					return;
				}

				if (!FinalMessage.TrimStartAndEnd().IsEmpty())
				{
					FinalMessage += TEXT("\n\n[The response was truncated after reaching the automatic continuation limit.]");
					Finish(FinalMessage, ResponseId, false, false);
					return;
				}
			}

			if (HasPendingSteerInstructions() && SteerContinuationCount < MaxSteerContinuationCount)
			{
				++SteerContinuationCount;
				EmitStatus(TEXT("AI is applying queued steer guidance."));
				SendModelRequest(BuildSteeredFollowupInput(), ResponseId);
				return;
			}

			if (FinalMessage.TrimStartAndEnd().IsEmpty())
			{
				Finish(TEXT("The AI response completed without text or tool calls."), ResponseId, true, false);
				return;
			}

			Finish(FinalMessage, ResponseId, false, false);
			return;
		}

		++ToolRoundCount;
		if (ToolRoundCount > CachedSettings.AiMaxToolRounds)
		{
			bPausedAtToolRoundLimit = true;
			RememberActiveTask(
				TEXT("paused_at_tool_round_limit"),
				TEXT("Read project memory key chat.active_task, review recentToolSummaries, then continue with exactly one bounded next step and verify it before more exploration."),
				TEXT("tool_round_limit"));
			Finish(
				FString::Printf(
					TEXT("Paused after %d tool rounds to avoid an infinite loop. Saved resume state to project memory key chat.active_task.\n\nNext step: read chat.active_task, pick the smallest unfinished step from recentToolSummaries, execute only that step, then verify before continuing."),
					CachedSettings.AiMaxToolRounds),
				FString(),
				false,
				false);
			return;
		}
		const int32 NearLimitRound = FMath::Max(1, CachedSettings.AiMaxToolRounds - 2);
		if (!bRememberedNearToolRoundLimit && ToolRoundCount >= NearLimitRound)
		{
			bRememberedNearToolRoundLimit = true;
			RememberActiveTask(
				TEXT("in_progress_near_tool_round_limit"),
				TEXT("Finish the smallest remaining step, avoid broad exploration, and verify with read-only inspection tools before calling more write tools."),
				TEXT("near_tool_round_limit"));
		}

		TArray<TSharedPtr<FJsonValue>> ToolOutputs;
		for (const FAssistantToolCall& ToolCall : ToolCalls)
		{
			if (bCancellationRequested.load(std::memory_order_relaxed))
			{
				Finish(TEXT("Generation stopped."), ResponseId, false, true);
				return;
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			FUnrealMcpExecutionResult ToolResult;
			if (!ToolCall.ArgumentsJson.TrimStartAndEnd().IsEmpty() && !UnrealMcp::LoadJsonObject(ToolCall.ArgumentsJson, ArgumentsObject))
			{
				ToolResult = UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("AI returned invalid JSON arguments for tool `%s`."), *ToolCall.UnrealToolName),
					nullptr,
					true);
			}
			else
			{
				ToolResult = Module->ExecuteTool(ToolCall.UnrealToolName, *ArgumentsObject);
			}

			AddRecentToolSummary(ToolCall, ToolResult);
			if (ToolResult.bIsError)
			{
				bHadToolError = true;
				RememberActiveTask(
					TEXT("in_progress_tool_error"),
					TEXT("Inspect the failed tool summary, classify the error if needed, then retry with a smaller or safer tool call."),
					TEXT("tool_error"));
			}

			EmitToolFinished(ToolCall, ToolResult);

			TSharedPtr<FJsonObject> ToolOutputObject = MakeShared<FJsonObject>();
			ToolOutputObject->SetStringField(TEXT("type"), TEXT("function_call_output"));
			ToolOutputObject->SetStringField(TEXT("call_id"), ToolCall.CallId);
			ToolOutputObject->SetStringField(TEXT("output"), SerializeToolResult(ToolResult));
			ToolOutputs.Add(MakeShared<FJsonValueObject>(ToolOutputObject));
		}

		const bool bAppliedSteer = AppendPendingSteerInput(ToolOutputs);
		EmitStatus(bAppliedSteer ? TEXT("AI is incorporating the tool results and queued steer guidance.") : TEXT("AI is incorporating the tool results."));
		SendModelRequest(ToolOutputs, ResponseId);
	}

	bool ExtractAssistantOutput(
		const FJsonObject& ResponseObject,
		TArray<FAssistantToolCall>& OutToolCalls,
		FString& OutFinalText,
		FString& OutFailureReason) const
	{
		OutToolCalls.Reset();
		OutFinalText.Reset();
		OutFailureReason.Reset();

		const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
		if (!ResponseObject.TryGetArrayField(TEXT("output"), OutputArray) || !OutputArray)
		{
			OutFailureReason = TEXT("AI response did not contain an output array.");
			return false;
		}

		TArray<FString> TextParts;

		for (const TSharedPtr<FJsonValue>& OutputValue : *OutputArray)
		{
			if (!OutputValue.IsValid() || OutputValue->Type != EJson::Object || !OutputValue->AsObject().IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject> OutputObject = OutputValue->AsObject();
			FString ItemType;
			OutputObject->TryGetStringField(TEXT("type"), ItemType);

			if (ItemType == TEXT("function_call"))
			{
				FAssistantToolCall ToolCall;
				if (!OutputObject->TryGetStringField(TEXT("name"), ToolCall.OpenAiFunctionName)
					|| !OutputObject->TryGetStringField(TEXT("call_id"), ToolCall.CallId))
				{
					OutFailureReason = TEXT("AI returned a function call without a name or call_id.");
					return false;
				}

				const FString* ToolName = FunctionNameToToolName.Find(ToolCall.OpenAiFunctionName);
				if (!ToolName)
				{
					OutFailureReason = FString::Printf(TEXT("AI called unknown tool alias `%s`."), *ToolCall.OpenAiFunctionName);
					return false;
				}

				ToolCall.UnrealToolName = *ToolName;
				OutputObject->TryGetStringField(TEXT("arguments"), ToolCall.ArgumentsJson);
				OutToolCalls.Add(MoveTemp(ToolCall));
				continue;
			}

			if (ItemType == TEXT("message"))
			{
				const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
				if (!OutputObject->TryGetArrayField(TEXT("content"), ContentArray) || !ContentArray)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
				{
					if (!ContentValue.IsValid() || ContentValue->Type != EJson::Object || !ContentValue->AsObject().IsValid())
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ContentObject = ContentValue->AsObject();
					FString ContentType;
					if (ContentObject->TryGetStringField(TEXT("type"), ContentType) && ContentType == TEXT("output_text"))
					{
						FString Text;
						if (ContentObject->TryGetStringField(TEXT("text"), Text) && !Text.TrimStartAndEnd().IsEmpty())
						{
							TextParts.Add(Text.TrimStartAndEnd());
						}
					}
				}
			}
		}

		if (TextParts.Num() > 0)
		{
			OutFinalText = FString::Join(TextParts, TEXT("\n\n"));
		}

		return true;
	}

	FString SerializeToolResult(const FUnrealMcpExecutionResult& ToolResult) const
	{
		TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
		OutputObject->SetStringField(TEXT("text"), ToolResult.Text);
		OutputObject->SetBoolField(TEXT("is_error"), ToolResult.bIsError);
		if (ToolResult.StructuredContent.IsValid())
		{
			OutputObject->SetObjectField(TEXT("structured_content"), ToolResult.StructuredContent);
		}

		return UnrealMcp::JsonObjectToString(OutputObject);
	}

	FAiProviderConfig ProviderConfig;
	const UUnrealMcpSettings* Settings = nullptr;
	const FUnrealMcpModule* Module = nullptr;
	FString UserPrompt;
	FString ConversationContext;
	FString PreviousResponseId;
	TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent;
	TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete;
	UnrealMcp::FUnrealMcpAssistantSettingsCache CachedSettings;
	TArray<TSharedPtr<FJsonValue>> OpenAiTools;
	TMap<FString, FString> FunctionNameToToolName;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
	FCriticalSection StateMutex;
	TArray<TSharedPtr<FJsonValue>> LastRequestInputItems;
	FString LastRequestPriorResponseId;
	TArray<FString> PendingSteerInstructions;
	TArray<uint8> RawResponseBytes;
	TArray<uint8> PendingStreamBytes;
	FString PendingSseData;
	TSharedPtr<FJsonObject> CompletedResponseObject;
	FString StreamFailureMessage;
	TMap<FString, FAssistantToolCall> StreamToolCalls;
	FString ActiveResponseId;
	FString AccumulatedAssistantText;
	TArray<FString> RecentToolSummaries;
	int32 ToolRoundCount = 0;
	int32 AutoContinuationCount = 0;
	int32 SteerContinuationCount = 0;
	static constexpr int32 MaxAutoContinuationCount = 2;
	static constexpr int32 MaxSteerContinuationCount = 2;
	static constexpr int32 MaxActiveTaskToolSummaries = 12;
	static constexpr int32 MaxPendingSteerInstructions = 5;
	static constexpr int32 MaxSteerInstructionChars = 900;
	static constexpr int32 LongTaskMemoryRoundThreshold = 4;
	bool bResponseIncompleteDueToMaxOutputTokens = false;
	bool bCompleted = false;
	std::atomic<bool> bCancellationRequested{false};
	bool bHadToolError = false;
	bool bRememberedNearToolRoundLimit = false;
	bool bPausedAtToolRoundLimit = false;
	bool bRetriedWithoutPreviousResponse = false;
};

namespace UnrealMcp
{
	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> CreateAssistantRun(
		const FAiProviderConfig& ProviderConfig,
		const UUnrealMcpSettings& Settings,
		const FUnrealMcpModule* Module,
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete)
	{
		const TSharedRef<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> Run = MakeShared<FUnrealMcpAssistantRun, ESPMode::ThreadSafe>(
			ProviderConfig,
			Settings,
			Module,
			UserPrompt,
			ConversationContext,
			PreviousResponseId,
			MoveTemp(OnEvent),
			MoveTemp(OnComplete));
		Run->Start();
		return StaticCastSharedRef<IUnrealMcpAssistantHandle>(Run);
	}
}
