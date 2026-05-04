#include "UnrealMcpModule.h"

#include "UnrealMcpActorTools.h"
#include "UnrealMcpBlueprintTools.h"
#include "UnrealMcpEditorTools.h"
#include "UnrealMcpMemoryTools.h"
#include "UnrealMcpScaffoldTools.h"
#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSkillTools.h"
#include "UnrealMcpToolExecutionGuard.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpWidgetTools.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult MakeExecutionResult(
		const FString& Text,
		const TSharedPtr<FJsonObject>& StructuredContent = nullptr,
		bool bIsError = false);
	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values);
	FString GetMcpExtensionLockPath();
	bool TryAcquireExtensionSessionLock(
		const FString& Owner,
		const FString& Reason,
		int32 TtlSeconds,
		bool bForce,
		FString& OutSessionId,
		TSharedPtr<FJsonObject>& OutLockObject,
		FString& OutFailureReason);
	bool ReleaseExtensionSessionLock(const FString& SessionId, bool bForce, FString& OutFailureReason);

	namespace
	{
		class FScopedSkillPromotionLock
		{
		public:
			FScopedSkillPromotionLock(const FString& ToolName, const FJsonObject& Arguments)
			{
				bool bSkipLock = false;
				bool bForceLock = false;
				double TtlSecondsDouble = 900.0;
				FString Owner = TEXT("Unreal MCP Chat");
				Arguments.TryGetBoolField(TEXT("skipLock"), bSkipLock);
				Arguments.TryGetBoolField(TEXT("forceLock"), bForceLock);
				Arguments.TryGetNumberField(TEXT("lockTtlSeconds"), TtlSecondsDouble);
				Arguments.TryGetStringField(TEXT("lockOwner"), Owner);

				if (bSkipLock)
				{
					bAcquired = true;
					bOwnsLock = false;
					return;
				}

				const int32 TtlSeconds = FMath::Clamp(static_cast<int32>(TtlSecondsDouble), 30, 86400);
				const FString Reason = FString::Printf(TEXT("Executing %s"), *ToolName);
				bAcquired = TryAcquireExtensionSessionLock(Owner, Reason, TtlSeconds, bForceLock, SessionId, LockObject, FailureReason);
				bOwnsLock = bAcquired;
			}

			~FScopedSkillPromotionLock()
			{
				if (bOwnsLock && !SessionId.IsEmpty())
				{
					FString ReleaseFailure;
					ReleaseExtensionSessionLock(SessionId, false, ReleaseFailure);
				}
			}

			bool IsAcquired() const
			{
				return bAcquired;
			}

			FString GetFailureReason() const
			{
				return FailureReason;
			}

			TSharedPtr<FJsonObject> MakeStructuredContent(const FString& Action) const
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), Action);
				StructuredContent->SetBoolField(TEXT("locked"), bAcquired);
				StructuredContent->SetStringField(TEXT("lockPath"), GetMcpExtensionLockPath());
				StructuredContent->SetStringField(TEXT("sessionId"), SessionId);
				if (LockObject.IsValid())
				{
					StructuredContent->SetObjectField(TEXT("lock"), LockObject);
				}
				return StructuredContent;
			}

		private:
			bool bAcquired = false;
			bool bOwnsLock = false;
			FString SessionId;
			FString FailureReason;
			TSharedPtr<FJsonObject> LockObject;
		};
	}
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteToolFromEditorUI(const FString& ToolName, const FJsonObject& Arguments) const
{
	return ExecuteTool(ToolName, Arguments);
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteTool(const FString& ToolName, const FJsonObject& Arguments) const
{
	const FString RegisteredHandlerName = UnrealMcp::ResolveToolHandlerName(ToolName);
	const UnrealMcp::FToolPolicy ActivityPolicy = UnrealMcp::GetToolPolicy(ToolName);
	if (ActivityPolicy.RiskLevel != UnrealMcp::EToolRiskLevel::ReadOnly)
	{
		TArray<FString> ArgumentKeys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments.Values)
		{
			ArgumentKeys.Add(Pair.Key);
		}
		ArgumentKeys.Sort();
		TSharedPtr<FJsonObject> ActivityDetails = MakeShared<FJsonObject>();
		ActivityDetails->SetStringField(TEXT("toolName"), ToolName);
		ActivityDetails->SetStringField(TEXT("handlerName"), RegisteredHandlerName);
		ActivityDetails->SetStringField(TEXT("riskLevel"), UnrealMcp::LexToString(ActivityPolicy.RiskLevel));
		ActivityDetails->SetArrayField(TEXT("argumentKeys"), UnrealMcp::MakeJsonStringArray(ArgumentKeys));
		UnrealMcp::RecordSkillActivityEvent(TEXT("mcp_tool_call"), FString::Printf(TEXT("Called MCP tool %s."), *ToolName), ActivityDetails);
	}

	FUnrealMcpExecutionResult Result = ExecuteToolInternal(RegisteredHandlerName, Arguments);
	UnrealMcp::AttachToolExecutionCheck(ToolName, Arguments, Result);
	return Result;
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteToolInternal(const FString& ToolName, const FJsonObject& Arguments) const
{
	FUnrealMcpExecutionResult EditorToolResult;
	if (UnrealMcp::TryExecuteEditorTool(ToolName, Arguments, EditorToolResult))
	{
		return EditorToolResult;
	}

	FUnrealMcpExecutionResult ActorToolResult;
	if (UnrealMcp::TryExecuteActorTool(ToolName, Arguments, ActorToolResult))
	{
		return ActorToolResult;
	}

	FUnrealMcpExecutionResult BlueprintToolResult;
	if (UnrealMcp::TryExecuteBlueprintTool(ToolName, Arguments, BlueprintToolResult))
	{
		return BlueprintToolResult;
	}

	FUnrealMcpExecutionResult WidgetToolResult;
	if (UnrealMcp::TryExecuteWidgetTool(ToolName, Arguments, WidgetToolResult))
	{
		return WidgetToolResult;
	}

	FUnrealMcpExecutionResult ScaffoldToolResult;
	if (UnrealMcp::TryExecuteScaffoldTool(ToolName, Arguments, ScaffoldToolResult))
	{
		return ScaffoldToolResult;
	}

	FUnrealMcpExecutionResult MemoryToolResult;
	if (UnrealMcp::TryExecuteMemoryTool(ToolName, Arguments, MemoryToolResult))
	{
		return MemoryToolResult;
	}

	FUnrealMcpExecutionResult SkillToolResult;
	if (UnrealMcp::TryExecuteSkillTool(
		ToolName,
		Arguments,
		[&ToolName](const FJsonObject& ToolArguments)
		{
			UnrealMcp::FScopedSkillPromotionLock ScopedLock(ToolName, ToolArguments);
			if (!ScopedLock.IsAcquired())
			{
				return UnrealMcp::MakeExecutionResult(ScopedLock.GetFailureReason(), ScopedLock.MakeStructuredContent(TEXT("mcp_extension_lock_failed")), true);
			}
			return UnrealMcp::SkillPromoteDraft(ToolArguments);
		},
		SkillToolResult))
	{
		return SkillToolResult;
	}

	TArray<TSharedPtr<FJsonValue>> ToolDefinitions;
	AppendToolDefinitions(ToolDefinitions);
	FUnrealMcpExecutionResult SelfExtensionToolResult;
	if (UnrealMcp::TryExecuteSelfExtensionTool(
		ToolName,
		Arguments,
		ToolDefinitions,
		[this](const FJsonObject& ToolArguments) { return RunMcpToolTest(ToolArguments); },
		[this](const FJsonObject& ToolArguments) { return RunMcpTestSuite(ToolArguments); },
		[this](const FJsonObject& ToolArguments) { return RunMcpExtensionPipeline(ToolArguments); },
		SelfExtensionToolResult))
	{
		return SelfExtensionToolResult;
	}

	return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unknown tool '%s'."), *ToolName), nullptr, true);
}
