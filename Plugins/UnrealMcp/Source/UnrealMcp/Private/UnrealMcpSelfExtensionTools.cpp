#include "UnrealMcpSelfExtensionTools.h"

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
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	TSharedPtr<FJsonObject> MakeEmptyObject();
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FUnrealMcpExecutionResult AuditMcpTools(const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason);
	bool ResolveProjectOutputDirectory(const FString& RequestedOutputRoot, FString& OutDirectory, FString& OutFailureReason);
	FString SanitizeMcpToolIdForPath(const FString& ToolName);
	FString GetUnrealMcpSavedRoot();
	FString GetProjectMemoryFilePath();
	FString GetMcpExtensionBackupRoot();
	FString GetMcpBuildLogRoot();
	FString GetLatestMcpExtensionManifestPath();
	FString GetMcpProjectStateBackupRoot();
	FString HashTextForManifest(const FString& Text);
	FString MakePathRelativeToProject(const FString& Path);
	FString FileTimeToIsoString(const FDateTime& Time);
	bool IsPathInsideDirectory(const FString& Path, const FString& Directory);
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
	bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason);
	TSharedPtr<FJsonObject> NormalizeOpenAiSchemaObject(const TSharedPtr<FJsonObject>& InputObject);
	bool IsOpenAiSchemaCompatibleObject(const TSharedPtr<FJsonObject>& InputObject, FString& OutReason);
	bool LoadProjectMemory(TSharedPtr<FJsonObject>& OutMemory, FString& OutFailureReason);
	bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason);
	void FindImmediateChildren(const FString& Directory, const FString& Pattern, bool bFiles, bool bDirectories, TArray<FString>& OutChildren);
	bool FindNewestFile(const FString& Directory, const FString& Pattern, FString& OutPath);
	TSharedPtr<FJsonObject> MakeFileInfoObject(const FString& Path);
	TSharedPtr<FJsonObject> MakeMemoryEntrySummary(const TSharedPtr<FJsonObject>& EntryObject, bool bIncludeContent);
	TSharedPtr<FJsonObject> FindMemoryEntryByKey(const TSharedPtr<FJsonObject>& MemoryObject, const FString& Key);
	FString TailLines(const FString& Text, int32 MaxLines);
	FString RecommendPipelineNextStep(const TSharedPtr<FJsonObject>& MemoryEntry);
	void WriteBuildTestMemory(
		const FString& MemoryKey,
		const FString& Summary,
		const FString& Status,
		const FString& NextStep,
		const TSharedPtr<FJsonObject>& Content);
	TSharedPtr<FJsonObject> MakePipelineStepObject(
		const FString& StepName,
		const FString& Status,
		const FString& Message,
		const FUnrealMcpExecutionResult* Result = nullptr);
	void AddAuditIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Location,
		const FString& Message);
	bool ResolveMcpScaffoldDirectory(const FJsonObject& Arguments, FString& OutDirectory, FString& OutToolName, FString& OutFailureReason);
	TSharedPtr<FJsonObject> FindToolDefinitionByName(const TArray<TSharedPtr<FJsonValue>>& ToolsArray, const FString& ToolName);
	bool ExtractRequestedSchemaFromScaffoldReadme(const FString& ScaffoldDirectory, FString& OutSchemaJson);
	FUnrealMcpExecutionResult ValidateMcpToolSchema(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult GenerateMcpTests(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ApplyMcpScaffold(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult RollbackLastMcpExtension(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult LockExtensionSession(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult BackupProjectState(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult RollbackToManifest(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult BuildEditor(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SupervisorInstall(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ListMcpScaffolds(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult InspectMcpScaffold(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ValidateCppSnippet(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult PatchScaffoldSnippet(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult CompileErrorFixPlan(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult DiffLastMcpApply(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult CleanMcpTestArtifacts(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult PreviewChangePlan(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult CaptureProjectSnapshot(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult DiffProjectSnapshot(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult VerifyTaskOutcome(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ClassifyMcpError(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult PrepareTestSandbox(const FJsonObject& Arguments);
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
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
	bool ResolveMcpTestsDirectory(
		const FJsonObject& Arguments,
		FString& OutTestsDirectory,
		FString& OutScaffoldDirectory,
		FString& OutToolName,
		FString& OutFailureReason);





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
				|| ToolName == TEXT("unreal.mcp_supervisor_install")
				|| ToolName == TEXT("unreal.mcp_generate_tests")
				|| ToolName == TEXT("unreal.mcp_build_editor")
				|| ToolName == TEXT("unreal.mcp_run_tool_test")
				|| ToolName == TEXT("unreal.mcp_run_test_suite")
				|| ToolName == TEXT("unreal.mcp_extension_pipeline");
		}

		bool IsSelfExtensionPieBlockedTool(const FString& ToolName)
		{
			return ToolName == TEXT("unreal.mcp_apply_scaffold")
				|| ToolName == TEXT("unreal.mcp_rollback_last_extension")
				|| ToolName == TEXT("unreal.mcp_rollback_to_manifest")
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

		if (ToolName == TEXT("unreal.mcp_workbench_status"))
		{
			OutResult = WorkbenchStatus(Arguments, ToolsArray);
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
