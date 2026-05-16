#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

FUnrealMcpExecutionResult FUnrealMcpModule::RunMcpExtensionPipeline(const FJsonObject& Arguments) const
{
	FString Mode = TEXT("auto");
	FString ToolName;
	FString ScaffoldDir;
	FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
	FString SchemaJson;
	FString TestRequestPath;
	FString TestsDir;
	FString MemoryKey = TEXT("mcp.extension.pipeline");
	FString Task;
	bool bApply = true;
	bool bBuild = true;
	bool bRunTest = true;
	bool bRunTestSuite = true;
	bool bGenerateTests = true;
	bool bOverwriteTests = true;
	bool bDryRunOnly = false;
	bool bApplyChatCommand = false;
	bool bCreateBackup = true;
	bool bBackupProjectState = true;
	bool bWriteProjectMemory = true;
	bool bEnforceGate = true;
	bool bCaptureSnapshots = true;
	bool bVerifyOutcome = true;
	bool bClassifyFailures = true;

	Arguments.TryGetStringField(TEXT("mode"), Mode);
	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("schemaJson"), SchemaJson);
	Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
	Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
	Arguments.TryGetStringField(TEXT("task"), Task);
	Arguments.TryGetBoolField(TEXT("apply"), bApply);
	Arguments.TryGetBoolField(TEXT("build"), bBuild);
	Arguments.TryGetBoolField(TEXT("runTest"), bRunTest);
	Arguments.TryGetBoolField(TEXT("runTestSuite"), bRunTestSuite);
	Arguments.TryGetBoolField(TEXT("generateTests"), bGenerateTests);
	Arguments.TryGetBoolField(TEXT("overwriteTests"), bOverwriteTests);
	Arguments.TryGetBoolField(TEXT("dryRunOnly"), bDryRunOnly);
	Arguments.TryGetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
	Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);
	Arguments.TryGetBoolField(TEXT("backupProjectState"), bBackupProjectState);
	Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
	Arguments.TryGetBoolField(TEXT("enforceGate"), bEnforceGate);
	Arguments.TryGetBoolField(TEXT("captureSnapshots"), bCaptureSnapshots);
	Arguments.TryGetBoolField(TEXT("verifyOutcome"), bVerifyOutcome);
	Arguments.TryGetBoolField(TEXT("classifyFailures"), bClassifyFailures);

	Mode = Mode.TrimStartAndEnd().ToLower();
	ToolName = ToolName.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	SchemaJson = SchemaJson.TrimStartAndEnd();
	TestRequestPath = TestRequestPath.TrimStartAndEnd();
	TestsDir = TestsDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
	Task = Task.TrimStartAndEnd();
	if (Mode.IsEmpty())
	{
		Mode = TEXT("auto");
	}
	if (MemoryKey.IsEmpty())
	{
		MemoryKey = TEXT("mcp.extension.pipeline");
	}

	TArray<TSharedPtr<FJsonValue>> Steps;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> FailureAnalyses;
	bool bSucceeded = true;
	bool bRequiresRestart = false;
	bool bAppliedSourceChanges = false;
	bool bApplyRequiresBuildRestartForRuntimeVisibility = false;
	bool bApplyRegisteredUsableNow = false;
	bool bBuildSucceeded = false;
	FString BeforeSnapshotPath;
	FString AfterSnapshotPath;
	FString PipelineEvidenceText;
	TSharedPtr<FJsonObject> LastApplyRegistrationStatus;

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_extension_pipeline"));
	StructuredContent->SetStringField(TEXT("mode"), Mode);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
	StructuredContent->SetBoolField(TEXT("enforceGate"), bEnforceGate);
	StructuredContent->SetBoolField(TEXT("captureSnapshots"), bCaptureSnapshots);
	StructuredContent->SetBoolField(TEXT("verifyOutcome"), bVerifyOutcome);

	auto MakePipelineStringArray = [](const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	};

	auto AddFailureAnalysis = [&FailureAnalyses, &Steps, bClassifyFailures, this](const FString& StepName, const FUnrealMcpExecutionResult& FailedResult)
	{
		if (!bClassifyFailures || !FailedResult.bIsError)
		{
			return;
		}

		TSharedPtr<FJsonObject> Analysis = MakeShared<FJsonObject>();
		Analysis->SetStringField(TEXT("step"), StepName);
		Analysis->SetStringField(TEXT("errorText"), FailedResult.Text);

		TSharedPtr<FJsonObject> ClassifyArguments = MakeShared<FJsonObject>();
		ClassifyArguments->SetStringField(TEXT("text"), FailedResult.Text);
		const FUnrealMcpExecutionResult ClassifyResult = UnrealMcp::ClassifyMcpError(*ClassifyArguments);
		if (ClassifyResult.StructuredContent.IsValid())
		{
			Analysis->SetObjectField(TEXT("classification"), ClassifyResult.StructuredContent);
		}

		if (StepName == TEXT("build"))
		{
			TSharedPtr<FJsonObject> FixPlanArguments = MakeShared<FJsonObject>();
			FixPlanArguments->SetBoolField(TEXT("dryRun"), true);
			const FUnrealMcpExecutionResult FixPlanResult = UnrealMcp::CompileErrorFixPlan(*FixPlanArguments);
			Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
				TEXT("compile_error_fix_plan"),
				FixPlanResult.bIsError ? TEXT("failed") : TEXT("completed"),
				FixPlanResult.Text,
				&FixPlanResult)));
			if (FixPlanResult.StructuredContent.IsValid())
			{
				Analysis->SetObjectField(TEXT("fixPlan"), FixPlanResult.StructuredContent);
			}
		}

		TArray<TSharedPtr<FJsonValue>> NextSteps;
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Inspect the failed pipeline step structuredContent before patching.")));
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Run unreal.mcp_classify_error on the complete log if this summary is insufficient.")));
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("If source was applied, run unreal.mcp_rollback_to_manifest with dryRun=true before deciding whether to roll back.")));
		Analysis->SetArrayField(TEXT("nextSteps"), NextSteps);
		FailureAnalyses.Add(MakeShared<FJsonValueObject>(Analysis));
	};

	if (Mode == TEXT("resume_test") || Mode == TEXT("test") || Mode == TEXT("test_only"))
	{
		TSharedPtr<FJsonObject> TestArguments = MakeShared<FJsonObject>();
		TestArguments->SetStringField(TEXT("toolName"), ToolName);
		TestArguments->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		TestArguments->SetStringField(TEXT("testsDir"), TestsDir);
		TestArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
		TestArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
		TestArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		TestArguments->SetBoolField(TEXT("readProjectMemory"), true);
		TestArguments->SetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
		const FUnrealMcpExecutionResult TestResult = bRunTestSuite ? RunMcpTestSuite(*TestArguments) : RunMcpToolTest(*TestArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TestResult.bIsError ? TEXT("failed") : TEXT("completed"),
			TestResult.Text,
			&TestResult)));

		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetBoolField(TEXT("succeeded"), !TestResult.bIsError);
		StructuredContent->SetBoolField(TEXT("requiresRestart"), false);
		StructuredContent->SetArrayField(TEXT("steps"), Steps);
		StructuredContent->SetArrayField(TEXT("issues"), Issues);
		return UnrealMcp::MakeExecutionResult(
			TestResult.bIsError ? TEXT("MCP extension pipeline resume test failed.") : TEXT("MCP extension pipeline resume test completed."),
			StructuredContent,
			TestResult.bIsError);
	}

	FString ResolvedScaffoldDir;
	FString ResolvedToolName;
	FString ResolveFailure;
	UnrealMcp::FToolsReadResolution ScaffoldResolution;
	TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
	ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
	ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
	if (!UnrealMcp::ResolveMcpScaffoldDirectory(*ResolveArguments, ResolvedScaffoldDir, ResolvedToolName, ResolveFailure, &ScaffoldResolution))
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("resolve_scaffold"), TEXT("failed"), ResolveFailure)));
		StructuredContent->SetBoolField(TEXT("succeeded"), false);
		StructuredContent->SetBoolField(TEXT("requiresRestart"), false);
		StructuredContent->SetArrayField(TEXT("steps"), Steps);
		StructuredContent->SetArrayField(TEXT("issues"), Issues);
		return UnrealMcp::MakeExecutionResult(ResolveFailure, StructuredContent, true);
	}

	ToolName = ResolvedToolName;
	if (Task.IsEmpty())
	{
		Task = FString::Printf(TEXT("Extend Unreal MCP with tool %s."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName);
	}
	if (TestRequestPath.IsEmpty())
	{
		TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
	}
	if (TestsDir.IsEmpty())
	{
		TestsDir = FPaths::Combine(ResolvedScaffoldDir, TEXT("Tests"));
	}
	StructuredContent->SetStringField(TEXT("toolName"), ToolName);
	StructuredContent->SetStringField(TEXT("task"), Task);
	StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
	StructuredContent->SetBoolField(TEXT("scaffoldFound"), ScaffoldResolution.bFound);
	StructuredContent->SetStringField(TEXT("scaffoldSourceKind"), UnrealMcp::LexToString(ScaffoldResolution.SourceKind));
	StructuredContent->SetArrayField(TEXT("scaffoldCandidates"), UnrealMcp::MakeToolsReadCandidateValues(ScaffoldResolution));
	if (!ScaffoldResolution.Warning.IsEmpty())
	{
		StructuredContent->SetStringField(TEXT("scaffoldResolutionWarning"), ScaffoldResolution.Warning);
	}
	StructuredContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
	StructuredContent->SetStringField(TEXT("testsDir"), TestsDir);
	Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
		TEXT("resolve_scaffold"),
		TEXT("completed"),
		FString::Printf(TEXT("Resolved scaffold for %s."), *ToolName))));

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	AppendToolDefinitions(ToolsArray);
	const bool bToolAlreadyListed = UnrealMcp::FindToolDefinitionByName(ToolsArray, ToolName).IsValid();
	StructuredContent->SetBoolField(TEXT("toolAlreadyListed"), bToolAlreadyListed);

	if (bEnforceGate)
	{
		TSharedPtr<FJsonObject> PreviewArguments = MakeShared<FJsonObject>();
		PreviewArguments->SetStringField(TEXT("task"), Task);
		const FUnrealMcpExecutionResult PreviewResult = UnrealMcp::PreviewChangePlan(*PreviewArguments, ToolsArray);
		PipelineEvidenceText += TEXT("\n[preview_change_plan] ") + PreviewResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("preview_change_plan"),
			PreviewResult.bIsError ? TEXT("failed") : TEXT("completed"),
			PreviewResult.Text,
			&PreviewResult)));
		if (PreviewResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("preview_change_plan"), PreviewResult);
		}
	}
	else
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("preview_change_plan"),
			TEXT("skipped"),
			TEXT("enforceGate=false."))));
	}

	if (SchemaJson.IsEmpty())
	{
		UnrealMcp::ExtractRequestedSchemaFromScaffoldReadme(ResolvedScaffoldDir, SchemaJson);
	}

	if (!SchemaJson.IsEmpty() || bToolAlreadyListed)
	{
		TSharedPtr<FJsonObject> ValidateArguments = MakeShared<FJsonObject>();
		if (!SchemaJson.IsEmpty())
		{
			ValidateArguments->SetStringField(TEXT("schemaJson"), SchemaJson);
		}
		else
		{
			ValidateArguments->SetStringField(TEXT("toolName"), ToolName);
		}
		ValidateArguments->SetBoolField(TEXT("returnNormalizedSchema"), true);
		const FUnrealMcpExecutionResult ValidateResult = UnrealMcp::ValidateMcpToolSchema(*ValidateArguments, ToolsArray);
		PipelineEvidenceText += TEXT("\n[validate_schema] ") + ValidateResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("validate_schema"),
			ValidateResult.bIsError ? TEXT("failed") : TEXT("completed"),
			ValidateResult.Text,
			&ValidateResult)));
		if (ValidateResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("validate_schema"), ValidateResult);
		}
	}
	else
	{
		UnrealMcp::AddAuditIssue(
			Issues,
			TEXT("warning"),
			TEXT("schemaJson"),
			TEXT("No schemaJson provided and the tool is not loaded yet; skipped requested schema validation."));
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("validate_schema"),
			TEXT("skipped"),
			TEXT("No schemaJson found; skipped schema validation."))));
	}

	if (bSucceeded && bGenerateTests)
	{
		TSharedPtr<FJsonObject> GenerateArguments = MakeShared<FJsonObject>();
		GenerateArguments->SetStringField(TEXT("toolName"), ToolName);
		GenerateArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		GenerateArguments->SetStringField(TEXT("testsDir"), TestsDir);
		GenerateArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
		GenerateArguments->SetStringField(TEXT("schemaJson"), SchemaJson);
		GenerateArguments->SetBoolField(TEXT("overwrite"), bOverwriteTests);
		GenerateArguments->SetBoolField(TEXT("dryRun"), bDryRunOnly);
		const FUnrealMcpExecutionResult GenerateTestsResult = UnrealMcp::GenerateMcpTests(*GenerateArguments, ToolsArray);
		PipelineEvidenceText += TEXT("\n[generate_tests] ") + GenerateTestsResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("generate_tests"),
			GenerateTestsResult.bIsError ? TEXT("failed") : TEXT("completed"),
			GenerateTestsResult.Text,
			&GenerateTestsResult)));
		if (GenerateTestsResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("generate_tests"), GenerateTestsResult);
		}
	}
	else if (!bGenerateTests)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("generate_tests"), TEXT("skipped"), TEXT("generateTests=false."))));
	}

	if (bSucceeded)
	{
		TSharedPtr<FJsonObject> DryRunArguments = MakeShared<FJsonObject>();
		DryRunArguments->SetStringField(TEXT("toolName"), ToolName);
		DryRunArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		DryRunArguments->SetBoolField(TEXT("dryRun"), true);
		DryRunArguments->SetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
		DryRunArguments->SetBoolField(TEXT("createBackup"), bCreateBackup);
		const FUnrealMcpExecutionResult DryRunResult = UnrealMcp::ApplyMcpScaffold(*DryRunArguments);
		PipelineEvidenceText += TEXT("\n[apply_dry_run] ") + DryRunResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("apply_dry_run"),
			DryRunResult.bIsError ? TEXT("failed") : TEXT("completed"),
			DryRunResult.Text,
			&DryRunResult)));
		if (DryRunResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("apply_dry_run"), DryRunResult);
		}
		else if (DryRunResult.StructuredContent.IsValid())
		{
			const TSharedPtr<FJsonObject>* RegistrationStatusObject = nullptr;
			if (DryRunResult.StructuredContent->TryGetObjectField(TEXT("registrationStatus"), RegistrationStatusObject)
				&& RegistrationStatusObject
				&& (*RegistrationStatusObject).IsValid())
			{
				StructuredContent->SetObjectField(TEXT("dryRunRegistrationStatus"), *RegistrationStatusObject);
			}
		}
	}

	if (bSucceeded && bDryRunOnly)
	{
		bApply = false;
		bBuild = false;
		bRunTest = false;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("dry_run_only"),
			TEXT("completed"),
			TEXT("dryRunOnly=true; skipped apply/build/test."))));
	}

	if (bSucceeded && !bDryRunOnly && bCaptureSnapshots && (bApply || bBuild || bRunTest))
	{
		TSharedPtr<FJsonObject> SnapshotArguments = MakeShared<FJsonObject>();
		SnapshotArguments->SetStringField(TEXT("snapshotName"), FString::Printf(TEXT("pipeline_%s_before"), *ToolName));
		SnapshotArguments->SetStringField(TEXT("assetPath"), TEXT("/Game"));
		SnapshotArguments->SetBoolField(TEXT("includeActors"), true);
		SnapshotArguments->SetBoolField(TEXT("includeAssets"), true);
		SnapshotArguments->SetBoolField(TEXT("includeBlueprints"), true);
		SnapshotArguments->SetBoolField(TEXT("includeWidgets"), true);
		SnapshotArguments->SetBoolField(TEXT("includeMemory"), true);
		SnapshotArguments->SetBoolField(TEXT("includeSkills"), true);
		const FUnrealMcpExecutionResult SnapshotResult = UnrealMcp::CaptureProjectSnapshot(*SnapshotArguments);
		PipelineEvidenceText += TEXT("\n[capture_before_snapshot] ") + SnapshotResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("capture_before_snapshot"),
			SnapshotResult.bIsError ? TEXT("failed") : TEXT("completed"),
			SnapshotResult.Text,
			&SnapshotResult)));
		if (SnapshotResult.StructuredContent.IsValid())
		{
			SnapshotResult.StructuredContent->TryGetStringField(TEXT("snapshotPath"), BeforeSnapshotPath);
			StructuredContent->SetStringField(TEXT("beforeSnapshotPath"), BeforeSnapshotPath);
		}
		if (SnapshotResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("capture_before_snapshot"), SnapshotResult);
		}
	}
	else if (!bCaptureSnapshots)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("capture_before_snapshot"),
			TEXT("skipped"),
			TEXT("captureSnapshots=false."))));
	}

	if (bSucceeded && !bDryRunOnly && bBackupProjectState && (bApply || bBuild || bRunTest))
	{
		TSharedPtr<FJsonObject> BackupArguments = MakeShared<FJsonObject>();
		BackupArguments->SetStringField(TEXT("label"), FString::Printf(TEXT("pipeline_%s"), *ToolName));
		BackupArguments->SetStringField(TEXT("reason"), FString::Printf(TEXT("Pre-pipeline snapshot before applying/building/testing MCP tool %s."), *ToolName));
		BackupArguments->SetBoolField(TEXT("includeBuildLogs"), false);
		const FUnrealMcpExecutionResult BackupResult = UnrealMcp::BackupProjectState(*BackupArguments);
		PipelineEvidenceText += TEXT("\n[backup_project_state] ") + BackupResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("backup_project_state"),
			BackupResult.bIsError ? TEXT("failed") : TEXT("completed"),
			BackupResult.Text,
			&BackupResult)));
		if (BackupResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("backup_project_state"), BackupResult);
		}
	}
	else if (bDryRunOnly || !bBackupProjectState)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("backup_project_state"),
			TEXT("skipped"),
			bDryRunOnly ? TEXT("dryRunOnly=true.") : TEXT("backupProjectState=false."))));
	}

	if (bSucceeded && bApply)
	{
		TSharedPtr<FJsonObject> ApplyArguments = MakeShared<FJsonObject>();
		ApplyArguments->SetStringField(TEXT("toolName"), ToolName);
		ApplyArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		ApplyArguments->SetBoolField(TEXT("dryRun"), false);
		ApplyArguments->SetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
		ApplyArguments->SetBoolField(TEXT("createBackup"), bCreateBackup);
		const FUnrealMcpExecutionResult ApplyResult = UnrealMcp::ApplyMcpScaffold(*ApplyArguments);
		PipelineEvidenceText += TEXT("\n[apply] ") + ApplyResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("apply"),
			ApplyResult.bIsError ? TEXT("failed") : TEXT("completed"),
			ApplyResult.Text,
			&ApplyResult)));
		if (ApplyResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("apply"), ApplyResult);
		}
		else if (ApplyResult.StructuredContent.IsValid())
		{
			ApplyResult.StructuredContent->TryGetBoolField(TEXT("changed"), bAppliedSourceChanges);
			const TSharedPtr<FJsonObject>* RegistrationStatusObject = nullptr;
			if (ApplyResult.StructuredContent->TryGetObjectField(TEXT("registrationStatus"), RegistrationStatusObject)
				&& RegistrationStatusObject
				&& (*RegistrationStatusObject).IsValid())
			{
				LastApplyRegistrationStatus = *RegistrationStatusObject;
				(*RegistrationStatusObject)->TryGetBoolField(TEXT("requiresBuildRestartForRuntimeVisibility"), bApplyRequiresBuildRestartForRuntimeVisibility);
				(*RegistrationStatusObject)->TryGetBoolField(TEXT("registeredUsableNow"), bApplyRegisteredUsableNow);
				StructuredContent->SetObjectField(TEXT("registrationStatus"), LastApplyRegistrationStatus);
			}
		}
	}
	else if (!bApply)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("apply"), TEXT("skipped"), TEXT("apply=false."))));
	}

	if (bSucceeded && bWriteProjectMemory)
	{
		TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
		MemoryContent->SetStringField(TEXT("toolName"), ToolName);
		MemoryContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		MemoryContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		MemoryContent->SetStringField(TEXT("testsDir"), TestsDir);
		MemoryContent->SetStringField(TEXT("pipelineMode"), Mode);
		MemoryContent->SetBoolField(TEXT("appliedSourceChanges"), bAppliedSourceChanges);
		MemoryContent->SetBoolField(TEXT("buildRequested"), bBuild);
		MemoryContent->SetBoolField(TEXT("runTestRequested"), bRunTest);
		MemoryContent->SetBoolField(TEXT("runTestSuite"), bRunTestSuite);
		UnrealMcp::WriteBuildTestMemory(
			MemoryKey,
			TEXT("MCP extension pipeline applied scaffold; build/test handoff pending."),
			TEXT("pipeline_apply_complete"),
			bBuild ? TEXT("Run build, restart Unreal Editor if needed, then resume mcp_extension_pipeline with mode=resume_test.") : TEXT("Run mcp_extension_pipeline with mode=resume_test when ready."),
			MemoryContent);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("write_memory"),
			TEXT("completed"),
			FString::Printf(TEXT("Wrote project memory key '%s'."), *MemoryKey))));
	}

	if (bSucceeded && bBuild)
	{
		TSharedPtr<FJsonObject> BuildArguments = MakeShared<FJsonObject>();
		BuildArguments->SetStringField(TEXT("toolName"), ToolName);
		BuildArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		BuildArguments->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		BuildArguments->SetStringField(TEXT("testsDir"), TestsDir);
		BuildArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		BuildArguments->SetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
		const FUnrealMcpExecutionResult BuildResult = UnrealMcp::BuildEditor(*BuildArguments);
		PipelineEvidenceText += TEXT("\n[build] ") + BuildResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("build"),
			BuildResult.bIsError ? TEXT("failed") : TEXT("completed"),
			BuildResult.Text,
			&BuildResult)));
		if (BuildResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("build"), BuildResult);
		}
		else if (BuildResult.StructuredContent.IsValid())
		{
			BuildResult.StructuredContent->TryGetBoolField(TEXT("succeeded"), bBuildSucceeded);
		}
	}
	else if (!bBuild)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("build"), TEXT("skipped"), TEXT("build=false."))));
	}

	const bool bShouldDeferTestForRestart =
		bBuild
		&& bBuildSucceeded
		&& !bToolAlreadyListed
		&& !bApplyRegisteredUsableNow
		&& (bAppliedSourceChanges || bApplyRequiresBuildRestartForRuntimeVisibility);
	if (bShouldDeferTestForRestart)
	{
		bRequiresRestart = true;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("restart"),
			TEXT("required"),
			TEXT("New descriptor-first C++ patches were compiled while the editor was running. Restart Unreal Editor before running the test step."))));
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TEXT("deferred"),
			bRunTestSuite ? TEXT("Run unreal.mcp_extension_pipeline with mode=resume_test after restart to execute the generated test suite.") : TEXT("Run unreal.mcp_extension_pipeline with mode=resume_test after restart, or use Tools/unreal_mcp_supervisor.py resume-test."))));
	}
	else if (bSucceeded && bApplyRequiresBuildRestartForRuntimeVisibility && !bBuild && !bApplyRegisteredUsableNow && bRunTest)
	{
		bRequiresRestart = true;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("runtime_visibility"),
			TEXT("blocked"),
			TEXT("Applied scaffold changes require an editor build/restart before the new tool can appear in tools/list; skipped test because build=false."))));
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TEXT("deferred"),
			TEXT("Enable build=true, rebuild/restart the editor, then resume testing."))));
	}
	else if (bSucceeded && bRunTest)
	{
		TSharedPtr<FJsonObject> TestArguments = MakeShared<FJsonObject>();
		TestArguments->SetStringField(TEXT("toolName"), ToolName);
		TestArguments->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		TestArguments->SetStringField(TEXT("testsDir"), TestsDir);
		TestArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		TestArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		TestArguments->SetBoolField(TEXT("readProjectMemory"), false);
		TestArguments->SetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
		const FUnrealMcpExecutionResult TestResult = bRunTestSuite ? RunMcpTestSuite(*TestArguments) : RunMcpToolTest(*TestArguments);
		PipelineEvidenceText += FString::Printf(TEXT("\n[%s] %s"), bRunTestSuite ? TEXT("test_suite") : TEXT("test"), *TestResult.Text);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TestResult.bIsError ? TEXT("failed") : TEXT("completed"),
			TestResult.Text,
			&TestResult)));
		if (TestResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(bRunTestSuite ? TEXT("test_suite") : TEXT("test"), TestResult);
		}
	}
	else if (!bRunTest)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("test"), TEXT("skipped"), TEXT("runTest=false."))));
	}

	if (bSucceeded && !bRequiresRestart && bCaptureSnapshots && bVerifyOutcome)
	{
		TSharedPtr<FJsonObject> SnapshotArguments = MakeShared<FJsonObject>();
		SnapshotArguments->SetStringField(TEXT("snapshotName"), FString::Printf(TEXT("pipeline_%s_after"), *ToolName));
		SnapshotArguments->SetStringField(TEXT("assetPath"), TEXT("/Game"));
		SnapshotArguments->SetBoolField(TEXT("includeActors"), true);
		SnapshotArguments->SetBoolField(TEXT("includeAssets"), true);
		SnapshotArguments->SetBoolField(TEXT("includeBlueprints"), true);
		SnapshotArguments->SetBoolField(TEXT("includeWidgets"), true);
		SnapshotArguments->SetBoolField(TEXT("includeMemory"), true);
		SnapshotArguments->SetBoolField(TEXT("includeSkills"), true);
		const FUnrealMcpExecutionResult SnapshotResult = UnrealMcp::CaptureProjectSnapshot(*SnapshotArguments);
		PipelineEvidenceText += TEXT("\n[capture_after_snapshot] ") + SnapshotResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("capture_after_snapshot"),
			SnapshotResult.bIsError ? TEXT("failed") : TEXT("completed"),
			SnapshotResult.Text,
			&SnapshotResult)));
		if (SnapshotResult.StructuredContent.IsValid())
		{
			SnapshotResult.StructuredContent->TryGetStringField(TEXT("snapshotPath"), AfterSnapshotPath);
			StructuredContent->SetStringField(TEXT("afterSnapshotPath"), AfterSnapshotPath);
		}
		if (SnapshotResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("capture_after_snapshot"), SnapshotResult);
		}
	}

	if (bSucceeded && !bRequiresRestart && bCaptureSnapshots && bVerifyOutcome && !BeforeSnapshotPath.IsEmpty() && !AfterSnapshotPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> DiffArguments = MakeShared<FJsonObject>();
		DiffArguments->SetStringField(TEXT("beforeSnapshotPath"), BeforeSnapshotPath);
		DiffArguments->SetStringField(TEXT("afterSnapshotPath"), AfterSnapshotPath);
		const FUnrealMcpExecutionResult DiffResult = UnrealMcp::DiffProjectSnapshot(*DiffArguments);
		PipelineEvidenceText += TEXT("\n[diff_project_snapshot] ") + DiffResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("diff_project_snapshot"),
			DiffResult.bIsError ? TEXT("failed") : TEXT("completed"),
			DiffResult.Text,
			&DiffResult)));
		if (DiffResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("diff_project_snapshot"), DiffResult);
		}
	}

	if (bSucceeded && !bRequiresRestart && bVerifyOutcome)
	{
		TSharedPtr<FJsonObject> VerifyArguments = MakeShared<FJsonObject>();
		VerifyArguments->SetStringField(TEXT("task"), Task);
		VerifyArguments->SetStringField(TEXT("beforeSnapshotPath"), BeforeSnapshotPath);
		VerifyArguments->SetStringField(TEXT("afterSnapshotPath"), AfterSnapshotPath);
		VerifyArguments->SetStringField(TEXT("evidenceText"), PipelineEvidenceText);
		if (!ToolName.IsEmpty())
		{
			TArray<FString> ExpectedTools;
			ExpectedTools.Add(ToolName);
			VerifyArguments->SetArrayField(TEXT("expectedTools"), MakePipelineStringArray(ExpectedTools));
		}
		const FUnrealMcpExecutionResult VerifyResult = UnrealMcp::VerifyTaskOutcome(*VerifyArguments, ToolsArray);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("verify_task_outcome"),
			VerifyResult.bIsError ? TEXT("failed") : TEXT("completed"),
			VerifyResult.Text,
			&VerifyResult)));
		if (VerifyResult.bIsError)
		{
			bSucceeded = false;
			AddFailureAnalysis(TEXT("verify_task_outcome"), VerifyResult);
		}
	}
	else if (bRequiresRestart && bVerifyOutcome)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("verify_task_outcome"),
			TEXT("deferred"),
			TEXT("Verification is deferred until the post-restart resume_test run can see the newly loaded tool definitions."))));
	}
	else if (!bVerifyOutcome)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("verify_task_outcome"),
			TEXT("skipped"),
			TEXT("verifyOutcome=false."))));
	}

	if (!bSucceeded && bAppliedSourceChanges)
	{
		TSharedPtr<FJsonObject> RollbackArguments = MakeShared<FJsonObject>();
		RollbackArguments->SetBoolField(TEXT("dryRun"), false);
		RollbackArguments->SetBoolField(TEXT("createPreRollbackBackup"), false);
		const FUnrealMcpExecutionResult RollbackResult = UnrealMcp::RollbackToManifest(*RollbackArguments);
		PipelineEvidenceText += TEXT("\n[rollback] ") + RollbackResult.Text;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("rollback"),
			RollbackResult.bIsError ? TEXT("failed") : TEXT("completed"),
			RollbackResult.Text,
			&RollbackResult)));
		StructuredContent->SetBoolField(TEXT("rollbackAttemptedAfterFailure"), true);
		StructuredContent->SetBoolField(TEXT("rollbackSucceededAfterFailure"), !RollbackResult.bIsError);
		if (RollbackResult.StructuredContent.IsValid())
		{
			StructuredContent->SetObjectField(TEXT("rollbackAfterFailure"), RollbackResult.StructuredContent);
		}
		if (RollbackResult.bIsError)
		{
			AddFailureAnalysis(TEXT("rollback"), RollbackResult);
		}
	}

	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
	StructuredContent->SetBoolField(TEXT("requiresRestart"), bRequiresRestart);
	StructuredContent->SetBoolField(TEXT("appliedSourceChanges"), bAppliedSourceChanges);
	StructuredContent->SetBoolField(TEXT("applyRequiresBuildRestartForRuntimeVisibility"), bApplyRequiresBuildRestartForRuntimeVisibility);
	StructuredContent->SetBoolField(TEXT("applyRegisteredUsableNow"), bApplyRegisteredUsableNow);
	StructuredContent->SetBoolField(TEXT("buildSucceeded"), bBuildSucceeded);
	StructuredContent->SetBoolField(TEXT("generateTests"), bGenerateTests);
	StructuredContent->SetBoolField(TEXT("runTestSuite"), bRunTestSuite);
	StructuredContent->SetBoolField(TEXT("backupProjectState"), bBackupProjectState);
	StructuredContent->SetStringField(TEXT("restartAdvice"), TEXT("If requiresRestart=true, close and reopen Unreal Editor, then call unreal.mcp_extension_pipeline with mode=resume_test and the same memoryKey."));
	StructuredContent->SetStringField(TEXT("supervisorCommand"), FString::Printf(TEXT("python3 Tools/unreal_mcp_supervisor.py resume-test --memory-key %s"), *MemoryKey));
	StructuredContent->SetArrayField(TEXT("steps"), Steps);
	StructuredContent->SetArrayField(TEXT("issues"), Issues);
	StructuredContent->SetArrayField(TEXT("failureAnalyses"), FailureAnalyses);

	const FString Text = bRequiresRestart
		? TEXT("MCP extension pipeline applied and built changes. Restart Unreal Editor, then resume test.")
		: (bSucceeded ? TEXT("MCP extension pipeline completed.") : TEXT("MCP extension pipeline failed. See steps for details."));
	return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bSucceeded);
}
