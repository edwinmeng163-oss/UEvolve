#include "UnrealMcpModule.h"

#include "UnrealMcpPythonToolBridge.h"
#include "UnrealMcpActorTools.h"
#include "UnrealMcpBlueprintTools.h"
#include "UnrealMcpEditorTools.h"
#include "UnrealMcpMemoryTools.h"
#include "UnrealMcpScaffoldTools.h"
#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSkillTools.h"
#include "UnrealMcpToolExecutionGuard.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpWidgetTools.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult MakeExecutionResult(
		const FString& Text,
		const TSharedPtr<FJsonObject>& StructuredContent = nullptr,
		bool bIsError = false);
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

	TSharedPtr<FJsonObject> PreflightBeforeExecution = UnrealMcp::BuildToolExecutionPreflight(ToolName, Arguments);
	FUnrealMcpExecutionResult Result = ExecuteToolInternal(RegisteredHandlerName, Arguments);
	UnrealMcp::AttachToolExecutionCheck(ToolName, Arguments, PreflightBeforeExecution, Result);
	return Result;
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteToolInternal(const FString& ToolName, const FJsonObject& Arguments) const
{
	const UnrealMcp::FToolHandlerRegistryEntry* HandlerEntry = UnrealMcp::FindToolHandlerRegistryEntry(ToolName);
	if (!HandlerEntry)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("handler_dispatch_failed"));
		StructuredContent->SetStringField(TEXT("handlerName"), ToolName);
		StructuredContent->SetStringField(TEXT("reason"), TEXT("handler_not_registered"));
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Handler '%s' is not registered in the explicit MCP handler registry."), *ToolName),
			StructuredContent,
			true);
	}

	if (HandlerEntry->ImplementationTrack == UnrealMcp::EToolImplementationTrack::Python)
	{
		return UnrealMcp::UnrealMcpPythonToolBridge::ExecutePythonRegisteredTool(*HandlerEntry, Arguments);
	}

	FUnrealMcpExecutionResult CategoryResult;
	const FString& Category = HandlerEntry->Category;
	if (Category == TEXT("editor"))
	{
		if (UnrealMcp::TryExecuteEditorTool(ToolName, Arguments, CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("actors"))
	{
		if (UnrealMcp::TryExecuteActorTool(ToolName, Arguments, CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("blueprint"))
	{
		if (UnrealMcp::TryExecuteBlueprintTool(ToolName, Arguments, CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("widget"))
	{
		if (UnrealMcp::TryExecuteWidgetTool(ToolName, Arguments, CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("scaffold"))
	{
		if (UnrealMcp::TryExecuteScaffoldTool(ToolName, Arguments, CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("memory"))
	{
		if (UnrealMcp::TryExecuteMemoryTool(ToolName, Arguments, CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("skills"))
	{
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
			CategoryResult))
		{
			return CategoryResult;
		}
	}
	else if (Category == TEXT("self-extension"))
	{
		TArray<TSharedPtr<FJsonValue>> ToolDefinitions;
		AppendToolDefinitions(ToolDefinitions);
		if (UnrealMcp::TryExecuteSelfExtensionTool(
				ToolName,
				Arguments,
				ToolDefinitions,
				[this](const FJsonObject& ToolArguments) { return RunMcpToolTest(ToolArguments); },
				[this](const FJsonObject& ToolArguments) { return RunMcpTestSuite(ToolArguments); },
				[this](const FJsonObject& ToolArguments) { return RunMcpExtensionPipeline(ToolArguments); },
				[this](const FJsonObject& ToolArguments) { return RunWorkflow(ToolArguments); },
				CategoryResult))
		{
			return CategoryResult;
		}
	}

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("handler_dispatch_failed"));
	StructuredContent->SetStringField(TEXT("handlerName"), ToolName);
	StructuredContent->SetStringField(TEXT("category"), Category);
	StructuredContent->SetStringField(TEXT("sourceFile"), HandlerEntry->SourceFile);
	StructuredContent->SetStringField(TEXT("reason"), TEXT("category_dispatcher_did_not_handle_registered_handler"));
	return UnrealMcp::MakeExecutionResult(
		FString::Printf(TEXT("Handler '%s' is registered under category '%s', but that category dispatcher did not handle it."), *ToolName, *Category),
		StructuredContent,
		true);
}
