#include "UnrealMcpToolExecutionGuard.h"

#include "UnrealMcpModule.h"
#include "UnrealMcpToolOutcomeVerifiers.h"
#include "UnrealMcpToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UnrealMcp
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> ExecutionGuardMakeStringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TArray<FString> ExecutionGuardGetArgumentKeys(const FJsonObject& Arguments)
		{
			TArray<FString> Keys;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments.Values)
			{
				Keys.Add(Pair.Key);
			}
			Keys.Sort();
			return Keys;
		}

		TArray<FString> InferMutationAreas(const FString& ToolName, const FToolPolicy& Policy)
		{
			TArray<FString> Areas;
			if (!Policy.bRequiresWrite && !Policy.bRequiresBuild && !Policy.bRequiresExternalProcess && !Policy.bRequiresRestart)
			{
				return Areas;
			}

			if (ToolName.Contains(TEXT("bp_")) || ToolName.Contains(TEXT("blueprint")))
			{
				Areas.Add(TEXT("Blueprint assets or Blueprint graphs"));
			}
			if (ToolName.Contains(TEXT("widget")))
			{
				Areas.Add(TEXT("Widget Blueprint hierarchy, bindings, or layout"));
			}
			if (ToolName.Contains(TEXT("actor")) || ToolName.Contains(TEXT("spawn")) || ToolName.Contains(TEXT("layout")) || ToolName.Contains(TEXT("level")))
			{
				Areas.Add(TEXT("Current editor level actors or selection state"));
			}
			if (ToolName.Contains(TEXT("project_memory")))
			{
				Areas.Add(TEXT("Saved/UnrealMcp/ProjectMemory.json"));
			}
			if (ToolName.Contains(TEXT("skill")))
			{
				Areas.Add(TEXT("Saved skill drafts, activity logs, or Tools/UnrealMcpSkills"));
			}
			if (ToolName.Contains(TEXT("mcp_")) || ToolName.Contains(TEXT("scaffold")))
			{
				Areas.Add(TEXT("MCP source patches, generated tests, manifests, or supervisor files"));
			}
			if (Policy.bRequiresBuild)
			{
				Areas.Add(TEXT("Unreal Build Tool output and build handoff state"));
			}
			if (Policy.bRequiresExternalProcess)
			{
				Areas.Add(TEXT("External process execution or generated launcher files"));
			}
			if (Policy.bRequiresRestart)
			{
				Areas.Add(TEXT("Editor restart handoff state"));
			}
			if (Areas.Num() == 0 && Policy.bRequiresWrite)
			{
				Areas.Add(TEXT("Editor/project state"));
			}
			return Areas;
		}

		TSharedPtr<FJsonObject> MakePreflightObject(const FString& ToolName, const FJsonObject& Arguments, const FToolPolicy& Policy)
		{
			TSharedPtr<FJsonObject> Preflight = MakeShared<FJsonObject>();
			Preflight->SetStringField(TEXT("toolName"), ToolName);
			Preflight->SetStringField(TEXT("riskLevel"), LexToString(Policy.RiskLevel));
			Preflight->SetObjectField(TEXT("policy"), MakeToolPolicyObject(ToolName));
			Preflight->SetArrayField(TEXT("argumentKeys"), ExecutionGuardMakeStringValues(ExecutionGuardGetArgumentKeys(Arguments)));
			Preflight->SetArrayField(TEXT("expectedMutationAreas"), ExecutionGuardMakeStringValues(InferMutationAreas(ToolName, Policy)));
			Preflight->SetStringField(TEXT("checkLevel"), TEXT("registry_metadata"));
			Preflight->SetBoolField(TEXT("evaluatedBeforeExecution"), true);
			Preflight->SetStringField(TEXT("summary"), Policy.bDryRunSupport
				? TEXT("Tool supports dryRun; prefer dryRun=true before applying write changes.")
				: TEXT("Tool execution plan inferred from explicit ToolRegistry metadata."));

			bool bDryRun = false;
			if (Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun))
			{
				Preflight->SetBoolField(TEXT("requestedDryRun"), bDryRun);
			}
			return Preflight;
		}

		TSharedPtr<FJsonObject> MakePostcheckObject(const FString& ToolName, const FUnrealMcpExecutionResult& Result, const FToolPolicy& Policy)
		{
			TSharedPtr<FJsonObject> Postcheck = MakeShared<FJsonObject>();
			Postcheck->SetStringField(TEXT("toolName"), ToolName);
			Postcheck->SetBoolField(TEXT("toolReturnedError"), Result.bIsError);
			Postcheck->SetBoolField(TEXT("genericResultSucceeded"), !Result.bIsError);
			Postcheck->SetBoolField(TEXT("toolSpecificVerifierAvailable"), false);
			Postcheck->SetStringField(TEXT("checkLevel"), TEXT("generic_result"));
			Postcheck->SetStringField(TEXT("summary"), Policy.bPostcheckSupport
				? TEXT("Generic postcheck attached. Add a tool-specific verifier for stronger outcome proof.")
				: TEXT("ToolRegistry does not require postcheck for this tool."));
			Postcheck->SetNumberField(TEXT("textLength"), Result.Text.Len());
			Postcheck->SetBoolField(TEXT("hasStructuredContent"), Result.StructuredContent.IsValid());
			return Postcheck;
		}

		bool NeedsExecutionCheck(const FToolPolicy& Policy)
		{
			return Policy.bRequiresWrite
				|| Policy.bRequiresBuild
				|| Policy.bRequiresExternalProcess
				|| Policy.bRequiresRestart
				|| Policy.bRequiresLock
				|| Policy.bPreflightSupport
				|| Policy.bPostcheckSupport;
		}
	}

	TSharedPtr<FJsonObject> BuildToolExecutionPreflight(
		const FString& RequestedToolName,
		const FJsonObject& Arguments)
	{
		const FToolPolicy Policy = GetToolPolicy(RequestedToolName);
		if (!NeedsExecutionCheck(Policy))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Preflight = MakePreflightObject(RequestedToolName, Arguments, Policy);
		if (TSharedPtr<FJsonObject> BlueprintPreflight = BuildBlueprintToolPreflight(RequestedToolName, Arguments, Preflight))
		{
			Preflight = BlueprintPreflight;
		}
		else if (TSharedPtr<FJsonObject> WidgetPreflight = BuildWidgetToolPreflight(RequestedToolName, Arguments, Preflight))
		{
			Preflight = WidgetPreflight;
		}
		else if (TSharedPtr<FJsonObject> ActorPreflight = BuildActorToolPreflight(RequestedToolName, Arguments, Preflight))
		{
			Preflight = ActorPreflight;
		}
		else if (TSharedPtr<FJsonObject> WorkflowPreflight = BuildWorkflowToolPreflight(RequestedToolName, Arguments, Preflight))
		{
			Preflight = WorkflowPreflight;
		}
		return Preflight;
	}

	void AttachToolExecutionCheck(
		const FString& RequestedToolName,
		const FJsonObject& Arguments,
		FUnrealMcpExecutionResult& Result)
	{
		AttachToolExecutionCheck(RequestedToolName, Arguments, BuildToolExecutionPreflight(RequestedToolName, Arguments), Result);
	}

	void AttachToolExecutionCheck(
		const FString& RequestedToolName,
		const FJsonObject& Arguments,
		const TSharedPtr<FJsonObject>& PreflightBeforeExecution,
		FUnrealMcpExecutionResult& Result)
	{
		const FToolPolicy Policy = GetToolPolicy(RequestedToolName);
		if (!NeedsExecutionCheck(Policy))
		{
			return;
		}

		if (!Result.StructuredContent.IsValid())
		{
			Result.StructuredContent = MakeShared<FJsonObject>();
			Result.StructuredContent->SetStringField(TEXT("message"), Result.Text);
		}

		Result.StructuredContent->SetObjectField(TEXT("policy"), MakeToolPolicyObject(RequestedToolName));
		if (PreflightBeforeExecution.IsValid())
		{
			Result.StructuredContent->SetObjectField(TEXT("preflight"), PreflightBeforeExecution);
		}
		else
		{
			TSharedPtr<FJsonObject> LatePreflight = BuildToolExecutionPreflight(RequestedToolName, Arguments);
			if (LatePreflight.IsValid())
			{
				LatePreflight->SetBoolField(TEXT("evaluatedBeforeExecution"), false);
				Result.StructuredContent->SetObjectField(TEXT("preflight"), LatePreflight);
			}
		}

		TSharedPtr<FJsonObject> Postcheck = VerifyBlueprintToolOutcome(RequestedToolName, Arguments, Result);
		if (!Postcheck.IsValid())
		{
			Postcheck = VerifyWidgetToolOutcome(RequestedToolName, Arguments, Result);
		}
		if (!Postcheck.IsValid())
		{
			Postcheck = VerifyActorToolOutcome(RequestedToolName, Arguments, Result);
		}
		if (!Postcheck.IsValid())
		{
			Postcheck = VerifyWorkflowToolOutcome(RequestedToolName, Arguments, Result);
		}
		if (!Postcheck.IsValid())
		{
			Postcheck = MakePostcheckObject(RequestedToolName, Result, Policy);
		}
		Result.StructuredContent->SetObjectField(TEXT("postcheck"), Postcheck);
	}
}
