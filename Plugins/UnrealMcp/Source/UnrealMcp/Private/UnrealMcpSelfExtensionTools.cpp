#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	namespace
	{
		class FScopedSelfExtensionToolLock
		{
		public:
			FScopedSelfExtensionToolLock(const FString& ToolName, const FJsonObject& Arguments)
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

			~FScopedSelfExtensionToolLock()
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

		bool IsSelfExtensionLockRequiredTool(const FString& ToolName)
		{
			return ToolName == TEXT("unreal.mcp_apply_scaffold")
				|| ToolName == TEXT("unreal.mcp_rollback_last_extension")
				|| ToolName == TEXT("unreal.mcp_backup_project_state")
				|| ToolName == TEXT("unreal.mcp_rollback_to_manifest")
				|| ToolName == TEXT("unreal.tools.import_package")
				|| ToolName == TEXT("unreal.mcp_supervisor_install")
				|| ToolName == TEXT("unreal.mcp_generate_tests")
				|| ToolName == TEXT("unreal.mcp_build_editor")
				|| ToolName == TEXT("unreal.mcp_build_game")
				|| ToolName == TEXT("unreal.mcp_build_server")
				|| ToolName == TEXT("unreal.mcp_build_client")
				|| ToolName == TEXT("unreal.mcp_build_packaged")
				|| ToolName == TEXT("unreal.mcp_run_tool_test")
				|| ToolName == TEXT("unreal.mcp_run_test_suite")
				|| ToolName == TEXT("unreal.mcp_extension_pipeline");
		}

		bool IsSelfExtensionPieBlockedTool(const FString& ToolName)
		{
			return ToolName == TEXT("unreal.mcp_apply_scaffold")
				|| ToolName == TEXT("unreal.mcp_rollback_last_extension")
				|| ToolName == TEXT("unreal.mcp_rollback_to_manifest")
				|| ToolName == TEXT("unreal.tools.import_package")
				|| ToolName == TEXT("unreal.mcp_generate_tests")
				|| ToolName == TEXT("unreal.mcp_extension_pipeline");
		}

		FUnrealMcpExecutionResult MakeSelfExtensionLockFailure(const FString& Action, const FScopedSelfExtensionToolLock& ScopedLock)
		{
			return MakeExecutionResult(
				ScopedLock.GetFailureReason(),
				ScopedLock.MakeStructuredContent(Action),
				true);
		}
	}

		bool TryExecuteSelfExtensionTool(
			const FString& ToolName,
			const FJsonObject& Arguments,
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
			const FSelfExtensionModuleToolRunner& RunToolTest,
			const FSelfExtensionModuleToolRunner& RunTestSuite,
			const FSelfExtensionModuleToolRunner& RunExtensionPipeline,
			const FSelfExtensionModuleToolRunner& RunWorkflow,
			FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.mcp_list_scaffolds"))
		{
			OutResult = ListMcpScaffolds(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_inspect_scaffold"))
		{
			OutResult = InspectMcpScaffold(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_validate_cpp_patch")
			|| ToolName == TEXT("unreal.mcp_validate_cpp_snippet"))
		{
			OutResult = ValidateCppSnippet(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_patch_scaffold_patch")
			|| ToolName == TEXT("unreal.mcp_patch_scaffold_snippet"))
		{
			OutResult = IsEditorPlaying() ? MakePieBlockedResult(ToolName) : PatchScaffoldSnippet(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_validate_tool_schema"))
		{
			OutResult = ValidateMcpToolSchema(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_tool_audit"))
		{
			OutResult = AuditMcpTools(ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.tools.export_package"))
		{
			OutResult = ExportToolPackage(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.tools.list_exportable"))
		{
			OutResult = ListExportableToolPackages(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_workbench_status"))
		{
			OutResult = WorkbenchStatus(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.knowledge_index_refresh"))
		{
			OutResult = KnowledgeIndexRefresh(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.knowledge_search"))
		{
			OutResult = KnowledgeSearch(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.tool_recommend"))
		{
			OutResult = ToolRecommend(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.tool_gap_analyze"))
		{
			OutResult = ToolGapAnalyze(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.workflow_recommend"))
		{
			OutResult = WorkflowRecommend(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.knowledge_eval_run"))
		{
			OutResult = KnowledgeEvalRun(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_compile_error_fix_plan"))
		{
			OutResult = CompileErrorFixPlan(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_pipeline_status"))
		{
			OutResult = PipelineStatus(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_diff_last_apply"))
		{
			OutResult = DiffLastMcpApply(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_clean_test_artifacts"))
		{
			OutResult = CleanMcpTestArtifacts(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.preview_change_plan"))
		{
			OutResult = PreviewChangePlan(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.capture_project_snapshot"))
		{
			OutResult = CaptureProjectSnapshot(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.diff_project_snapshot"))
		{
			OutResult = DiffProjectSnapshot(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.verify_task_outcome"))
		{
			OutResult = VerifyTaskOutcome(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_classify_error"))
		{
			OutResult = ClassifyMcpError(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_prepare_test_sandbox"))
		{
			OutResult = PrepareTestSandbox(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.workflow_run"))
		{
			OutResult = RunWorkflow(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_lock_extension_session"))
		{
			OutResult = LockExtensionSession(Arguments);
			return true;
		}

		if (IsSelfExtensionLockRequiredTool(ToolName))
		{
			if (IsSelfExtensionPieBlockedTool(ToolName) && IsEditorPlaying())
			{
				OutResult = MakePieBlockedResult(ToolName);
				return true;
			}

			FScopedSelfExtensionToolLock ScopedLock(ToolName, Arguments);
			if (!ScopedLock.IsAcquired())
			{
				OutResult = MakeSelfExtensionLockFailure(TEXT("mcp_extension_lock_failed"), ScopedLock);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_apply_scaffold"))
			{
				OutResult = ApplyMcpScaffold(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_rollback_last_extension"))
			{
				OutResult = RollbackLastMcpExtension(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_backup_project_state"))
			{
				OutResult = BackupProjectState(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_rollback_to_manifest"))
			{
				OutResult = RollbackToManifest(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.tools.import_package"))
			{
				OutResult = ImportToolPackage(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_supervisor_install"))
			{
				OutResult = SupervisorInstall(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_generate_tests"))
			{
				OutResult = GenerateMcpTests(Arguments, ToolsArray);
				return true;
			}

			if (TryExecuteSelfExtensionBuildTool(ToolName, Arguments, OutResult))
			{
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_run_tool_test"))
			{
				OutResult = RunToolTest(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_run_test_suite"))
			{
				OutResult = RunTestSuite(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_extension_pipeline"))
			{
				OutResult = RunExtensionPipeline(Arguments);
				return true;
			}

			OutResult = BuildEditor(Arguments);
			return true;
		}

		return false;
	}
}
