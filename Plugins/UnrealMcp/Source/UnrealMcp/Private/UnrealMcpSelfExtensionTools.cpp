#include "UnrealMcpSelfExtensionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
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
	FUnrealMcpExecutionResult BackupProjectState(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult BuildEditor(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ListMcpScaffolds(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult InspectMcpScaffold(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ValidateCppSnippet(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult PatchScaffoldSnippet(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult CompileErrorFixPlan(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult DiffLastMcpApply(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult CleanMcpTestArtifacts(const FJsonObject& Arguments);
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	bool ResolveMcpTestsDirectory(
		const FJsonObject& Arguments,
		FString& OutTestsDirectory,
		FString& OutScaffoldDirectory,
		FString& OutToolName,
		FString& OutFailureReason);

	bool TryExecuteSelfExtensionTool(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
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

		if (ToolName == TEXT("unreal.mcp_validate_cpp_snippet"))
		{
			OutResult = ValidateCppSnippet(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_patch_scaffold_snippet"))
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

		return false;
	}

	FUnrealMcpExecutionResult PipelineStatus(const FJsonObject& Arguments)
	{
		FString MemoryKey = TEXT("mcp.extension.pipeline");
		bool bIncludeAllMemory = false;
		bool bIncludeBuildLogTail = true;
		Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
		Arguments.TryGetBoolField(TEXT("includeAllMemory"), bIncludeAllMemory);
		Arguments.TryGetBoolField(TEXT("includeBuildLogTail"), bIncludeBuildLogTail);
		const int32 BuildLogTailLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("buildLogTailLines"), 80), 500);

		FString FailureReason;
		TSharedPtr<FJsonObject> MemoryObject;
		if (!LoadProjectMemory(MemoryObject, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TSharedPtr<FJsonObject> MatchingMemoryEntry = FindMemoryEntryByKey(MemoryObject, MemoryKey.TrimStartAndEnd());
		TArray<TSharedPtr<FJsonValue>> MemorySummaries;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (MemoryObject->TryGetArrayField(TEXT("entries"), Entries) && Entries)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				if (EntryValue.IsValid() && EntryValue->Type == EJson::Object && EntryValue->AsObject().IsValid())
				{
					MemorySummaries.Add(MakeShared<FJsonValueObject>(MakeMemoryEntrySummary(EntryValue->AsObject(), bIncludeAllMemory)));
				}
			}
		}

		TArray<FString> TestScaffolds;
		TArray<FString> ExtensionBackups;
		TArray<FString> TestRequests;
		FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), TEXT("*"), false, true, TestScaffolds);
		FindImmediateChildren(GetMcpExtensionBackupRoot(), TEXT("*"), false, true, ExtensionBackups);
		FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestRequests")), TEXT("*"), false, true, TestRequests);

		TArray<TSharedPtr<FJsonValue>> TestScaffoldValues;
		for (const FString& Path : TestScaffolds)
		{
			TestScaffoldValues.Add(MakeShared<FJsonValueString>(Path));
		}
		TArray<TSharedPtr<FJsonValue>> BackupValues;
		for (const FString& Path : ExtensionBackups)
		{
			BackupValues.Add(MakeShared<FJsonValueString>(Path));
		}
		TArray<TSharedPtr<FJsonValue>> TestRequestValues;
		for (const FString& Path : TestRequests)
		{
			TestRequestValues.Add(MakeShared<FJsonValueString>(Path));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_pipeline_status"));
		StructuredContent->SetStringField(TEXT("savedRoot"), GetUnrealMcpSavedRoot());
		StructuredContent->SetStringField(TEXT("memoryPath"), GetProjectMemoryFilePath());
		StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
		StructuredContent->SetBoolField(TEXT("memoryEntryFound"), MatchingMemoryEntry.IsValid());
		StructuredContent->SetObjectField(TEXT("selectedMemoryEntry"), MakeMemoryEntrySummary(MatchingMemoryEntry, true));
		StructuredContent->SetNumberField(TEXT("memoryEntryCount"), MemorySummaries.Num());
		StructuredContent->SetArrayField(TEXT("memoryEntries"), MemorySummaries);
		StructuredContent->SetStringField(TEXT("latestManifestPath"), GetLatestMcpExtensionManifestPath());
		StructuredContent->SetObjectField(TEXT("latestManifestFile"), MakeFileInfoObject(GetLatestMcpExtensionManifestPath()));

		TSharedPtr<FJsonObject> ManifestObject;
		if (FPaths::FileExists(GetLatestMcpExtensionManifestPath()) && LoadJsonObjectFromFile(GetLatestMcpExtensionManifestPath(), ManifestObject, FailureReason))
		{
			StructuredContent->SetObjectField(TEXT("latestManifest"), ManifestObject);
		}
		else if (!FailureReason.IsEmpty())
		{
			StructuredContent->SetStringField(TEXT("latestManifestReadWarning"), FailureReason);
		}

		FString LatestBuildLogPath;
		StructuredContent->SetStringField(TEXT("buildLogRoot"), GetMcpBuildLogRoot());
		if (FindNewestFile(GetMcpBuildLogRoot(), TEXT("*.log"), LatestBuildLogPath))
		{
			StructuredContent->SetObjectField(TEXT("latestBuildLog"), MakeFileInfoObject(LatestBuildLogPath));
			if (bIncludeBuildLogTail)
			{
				FString LogText;
				if (FFileHelper::LoadFileToString(LogText, *LatestBuildLogPath))
				{
					StructuredContent->SetStringField(TEXT("latestBuildLogTail"), TailLines(LogText, BuildLogTailLines));
				}
			}
		}

		StructuredContent->SetNumberField(TEXT("testScaffoldCount"), TestScaffolds.Num());
		StructuredContent->SetArrayField(TEXT("testScaffolds"), TestScaffoldValues);
		StructuredContent->SetNumberField(TEXT("extensionBackupCount"), ExtensionBackups.Num());
		StructuredContent->SetArrayField(TEXT("extensionBackups"), BackupValues);
		StructuredContent->SetNumberField(TEXT("testRequestCount"), TestRequests.Num());
		StructuredContent->SetArrayField(TEXT("testRequests"), TestRequestValues);
		StructuredContent->SetStringField(TEXT("recommendedNextStep"), RecommendPipelineNextStep(MatchingMemoryEntry));

		const FString Text = FString::Printf(
			TEXT("MCP pipeline status: memoryKey=%s found=%s testScaffolds=%d backups=%d."),
			*MemoryKey,
			MatchingMemoryEntry.IsValid() ? TEXT("true") : TEXT("false"),
			TestScaffolds.Num(),
			ExtensionBackups.Num());
		return MakeExecutionResult(Text, StructuredContent, false);
	}

	FUnrealMcpExecutionResult WorkbenchStatus(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		FString MemoryKey = TEXT("mcp.extension.pipeline");
		bool bIncludeBuildLogTail = false;
		Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
		Arguments.TryGetBoolField(TEXT("includeBuildLogTail"), bIncludeBuildLogTail);
		const int32 BuildLogTailLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("buildLogTailLines"), 80), 500);

		TSharedPtr<FJsonObject> PipelineArguments = MakeShared<FJsonObject>();
		PipelineArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		PipelineArguments->SetBoolField(TEXT("includeAllMemory"), false);
		PipelineArguments->SetBoolField(TEXT("includeBuildLogTail"), bIncludeBuildLogTail);
		PipelineArguments->SetNumberField(TEXT("buildLogTailLines"), BuildLogTailLines);

		const FUnrealMcpExecutionResult AuditResult = AuditMcpTools(ToolsArray);
		const FUnrealMcpExecutionResult PipelineResult = PipelineStatus(*PipelineArguments);

		double SchemaIncompatibleCount = 0.0;
		double MissingHandlerCount = 0.0;
		double MissingDocumentationCount = 0.0;
		if (AuditResult.StructuredContent.IsValid())
		{
			AuditResult.StructuredContent->TryGetNumberField(TEXT("schemaIncompatibleCount"), SchemaIncompatibleCount);
			AuditResult.StructuredContent->TryGetNumberField(TEXT("missingHandlerCount"), MissingHandlerCount);
			AuditResult.StructuredContent->TryGetNumberField(TEXT("missingDocumentationCount"), MissingDocumentationCount);
		}

		double MemoryEntryCount = 0.0;
		double TestScaffoldCount = 0.0;
		double ExtensionBackupCount = 0.0;
		if (PipelineResult.StructuredContent.IsValid())
		{
			PipelineResult.StructuredContent->TryGetNumberField(TEXT("memoryEntryCount"), MemoryEntryCount);
			PipelineResult.StructuredContent->TryGetNumberField(TEXT("testScaffoldCount"), TestScaffoldCount);
			PipelineResult.StructuredContent->TryGetNumberField(TEXT("extensionBackupCount"), ExtensionBackupCount);
		}

		TArray<FString> TestScaffolds;
		FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), TEXT("*"), false, true, TestScaffolds);

		int32 SavedTestCaseCount = 0;
		for (const FString& TestScaffold : TestScaffolds)
		{
			TArray<FString> TestFiles;
			FindImmediateChildren(FPaths::Combine(TestScaffold, TEXT("Tests")), TEXT("*.json"), true, false, TestFiles);
			SavedTestCaseCount += TestFiles.Num();
		}

		TArray<FString> VersionedTestFiles;
		const FString VersionedTestRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpTests")));
		IFileManager::Get().FindFilesRecursive(VersionedTestFiles, *VersionedTestRoot, TEXT("*.json"), true, false);
		const int32 VersionedTestCaseCount = VersionedTestFiles.Num();
		const int32 TestCaseCount = SavedTestCaseCount + VersionedTestCaseCount;

		FString LatestSupervisorLogPath;
		const bool bHasSupervisorLog = FindNewestFile(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("SupervisorLogs")), TEXT("*.log"), LatestSupervisorLogPath);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_workbench_status"));
		StructuredContent->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		StructuredContent->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
		StructuredContent->SetObjectField(TEXT("audit"), AuditResult.StructuredContent.IsValid() ? AuditResult.StructuredContent : MakeShared<FJsonObject>());
		StructuredContent->SetObjectField(TEXT("pipeline"), PipelineResult.StructuredContent.IsValid() ? PipelineResult.StructuredContent : MakeShared<FJsonObject>());
		StructuredContent->SetNumberField(TEXT("visibleToolCount"), ToolsArray.Num());
		StructuredContent->SetNumberField(TEXT("testCaseCount"), TestCaseCount);
		StructuredContent->SetNumberField(TEXT("savedTestCaseCount"), SavedTestCaseCount);
		StructuredContent->SetNumberField(TEXT("versionedTestCaseCount"), VersionedTestCaseCount);
		StructuredContent->SetStringField(TEXT("versionedTestRoot"), VersionedTestRoot);
		StructuredContent->SetBoolField(TEXT("supervisorLogFound"), bHasSupervisorLog);
		if (bHasSupervisorLog)
		{
			StructuredContent->SetObjectField(TEXT("latestSupervisorLog"), MakeFileInfoObject(LatestSupervisorLogPath));
		}
		AddToolRegistryStatus(StructuredContent);

		const bool bHealthy = SchemaIncompatibleCount <= 0.0
			&& MissingHandlerCount <= 0.0
			&& MissingDocumentationCount <= 0.0
			&& !PipelineResult.bIsError;
		StructuredContent->SetBoolField(TEXT("healthy"), bHealthy);
		StructuredContent->SetStringField(
			TEXT("recommendedNextStep"),
			bHealthy
				? TEXT("Continue with ToolRegistry modularization and add versioned test fixtures for core self-extension tools.")
				: TEXT("Run unreal.mcp_tool_audit and address schema, handler, documentation, or pipeline warnings before adding more tools."));

		const FString Text = FString::Printf(
			TEXT("MCP workbench status: visibleTools=%d schemaIncompatible=%d missingHandlers=%d memoryEntries=%d testScaffolds=%d testCases=%d healthy=%s"),
			ToolsArray.Num(),
			static_cast<int32>(SchemaIncompatibleCount),
			static_cast<int32>(MissingHandlerCount),
			static_cast<int32>(MemoryEntryCount),
			static_cast<int32>(TestScaffoldCount),
			TestCaseCount,
			bHealthy ? TEXT("true") : TEXT("false"));
		return MakeExecutionResult(Text, StructuredContent, false);
	}
}

FUnrealMcpExecutionResult FUnrealMcpModule::RunMcpToolTest(const FJsonObject& Arguments) const
{
	FString ToolName;
	FString TestRequestPath;
	FString ScaffoldDir;
	FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
	FString MemoryKey = TEXT("mcp.extension.build_test");
	bool bReadProjectMemory = true;
	bool bWriteProjectMemory = true;
	bool bExecuteTool = true;
	bool bExpectToolListed = true;
	bool bRunSuite = false;

	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
	Arguments.TryGetBoolField(TEXT("readProjectMemory"), bReadProjectMemory);
	Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
	Arguments.TryGetBoolField(TEXT("executeTool"), bExecuteTool);
	Arguments.TryGetBoolField(TEXT("expectToolListed"), bExpectToolListed);
	Arguments.TryGetBoolField(TEXT("runSuite"), bRunSuite);

	if (bRunSuite)
	{
		return RunMcpTestSuite(Arguments);
	}

	ToolName = ToolName.TrimStartAndEnd();
	TestRequestPath = TestRequestPath.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
	if (MemoryKey.IsEmpty())
	{
		MemoryKey = TEXT("mcp.extension.build_test");
	}

	TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
	if (bReadProjectMemory)
	{
		FString FailureReason;
		TSharedPtr<FJsonObject> MemoryObject;
		if (UnrealMcp::LoadProjectMemory(MemoryObject, FailureReason) && MemoryObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (MemoryObject->TryGetArrayField(TEXT("entries"), Entries) && Entries)
			{
				for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
				{
					if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
					FString ExistingKey;
					if (!EntryObject->TryGetStringField(TEXT("key"), ExistingKey) || ExistingKey != MemoryKey)
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ContentObject = nullptr;
					if (EntryObject->TryGetObjectField(TEXT("content"), ContentObject) && ContentObject && (*ContentObject).IsValid())
					{
						MemoryContent = *ContentObject;
						if (ToolName.IsEmpty())
						{
							MemoryContent->TryGetStringField(TEXT("toolName"), ToolName);
						}
						if (TestRequestPath.IsEmpty())
						{
							MemoryContent->TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
						}
						if (ScaffoldDir.IsEmpty())
						{
							MemoryContent->TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
						}
					}
					break;
				}
			}
		}
	}

	if (TestRequestPath.IsEmpty())
	{
		if (!ScaffoldDir.IsEmpty())
		{
			FString ResolvedScaffoldDir;
			FString FailureReason;
			if (!UnrealMcp::ResolveProjectPathInsideProject(ScaffoldDir, ResolvedScaffoldDir, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}
			TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
		}
		else if (!ToolName.IsEmpty())
		{
			FString ResolvedOutputRoot;
			FString FailureReason;
			if (!UnrealMcp::ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}
			TestRequestPath = FPaths::Combine(ResolvedOutputRoot, UnrealMcp::SanitizeMcpToolIdForPath(ToolName), TEXT("TestRequest.json"));
		}
	}

	FString ResolvedTestRequestPath;
	FString ResolveFailure;
	if (!UnrealMcp::ResolveProjectPathInsideProject(TestRequestPath, ResolvedTestRequestPath, ResolveFailure))
	{
		return UnrealMcp::MakeExecutionResult(ResolveFailure, nullptr, true);
	}

	FString TestRequestText;
	if (!FFileHelper::LoadFileToString(TestRequestText, *ResolvedTestRequestPath))
	{
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to read TestRequest.json at '%s'."), *ResolvedTestRequestPath), nullptr, true);
	}

	TSharedPtr<FJsonObject> TestRequestObject;
	if (!UnrealMcp::LoadJsonObject(TestRequestText, TestRequestObject) || !TestRequestObject.IsValid())
	{
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Test request '%s' is not valid JSON."), *ResolvedTestRequestPath), nullptr, true);
	}

	TSharedPtr<FJsonObject> TestCaseObject;
	TSharedPtr<FJsonObject> RequestObject = TestRequestObject;
	FString TestName = FPaths::GetBaseFilename(ResolvedTestRequestPath);
	FString TestDescription;
	FString ExpectationNote;
	bool bExpectToolCallError = false;
	bool bHasExpectToolCallError = false;
	const TSharedPtr<FJsonObject>* WrappedRequestObject = nullptr;
	if (TestRequestObject->TryGetObjectField(TEXT("request"), WrappedRequestObject) && WrappedRequestObject && (*WrappedRequestObject).IsValid())
	{
		TestCaseObject = TestRequestObject;
		RequestObject = *WrappedRequestObject;
		TestCaseObject->TryGetStringField(TEXT("name"), TestName);
		TestCaseObject->TryGetStringField(TEXT("description"), TestDescription);
		TestCaseObject->TryGetStringField(TEXT("expectationNote"), ExpectationNote);
		TestCaseObject->TryGetBoolField(TEXT("executeTool"), bExecuteTool);
		TestCaseObject->TryGetBoolField(TEXT("expectToolListed"), bExpectToolListed);
		if (TestCaseObject->TryGetBoolField(TEXT("expectToolCallError"), bExpectToolCallError)
			|| TestCaseObject->TryGetBoolField(TEXT("expectError"), bExpectToolCallError))
		{
			bHasExpectToolCallError = true;
		}
	}

	FString Method;
	RequestObject->TryGetStringField(TEXT("method"), Method);
	if (!Method.IsEmpty() && Method != TEXT("tools/call"))
	{
		return UnrealMcp::MakeExecutionResult(TEXT("TestRequest.json must use JSON-RPC method tools/call."), nullptr, true);
	}

	const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
	if (!RequestObject->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject || !(*ParamsObject).IsValid())
	{
		return UnrealMcp::MakeExecutionResult(TEXT("TestRequest.json is missing params object."), nullptr, true);
	}

	FString RequestToolName;
	(*ParamsObject)->TryGetStringField(TEXT("name"), RequestToolName);
	RequestToolName = RequestToolName.TrimStartAndEnd();
	if (RequestToolName.IsEmpty())
	{
		RequestToolName = ToolName;
	}
	if (RequestToolName.IsEmpty())
	{
		return UnrealMcp::MakeExecutionResult(TEXT("Unable to determine tool name from arguments, project memory, or TestRequest.json."), nullptr, true);
	}

	const TSharedPtr<FJsonObject>* RequestArgumentsObject = nullptr;
	const TSharedPtr<FJsonObject> EmptyArguments = UnrealMcp::MakeEmptyObject();
	const FJsonObject& RequestArguments = ((*ParamsObject)->TryGetObjectField(TEXT("arguments"), RequestArgumentsObject) && RequestArgumentsObject && (*RequestArgumentsObject).IsValid())
		? **RequestArgumentsObject
		: *EmptyArguments;

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	AppendToolDefinitions(ToolsArray);

	bool bToolListed = false;
	TSharedPtr<FJsonObject> ListedToolObject;
	for (const TSharedPtr<FJsonValue>& ToolValue : ToolsArray)
	{
		if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
		{
			continue;
		}

		FString ListedName;
		if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), ListedName) && ListedName == RequestToolName)
		{
			bToolListed = true;
			ListedToolObject = ToolValue->AsObject();
			break;
		}
	}

	bool bToolExecuted = false;
	FUnrealMcpExecutionResult ToolResult;
	bool bInjectedSkipLock = false;
	if (bExecuteTool && bToolListed)
	{
		TSharedPtr<FJsonObject> EffectiveRequestArguments = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RequestArguments.Values)
		{
			EffectiveRequestArguments->SetField(Pair.Key, Pair.Value);
		}

		const UnrealMcp::FToolPolicy RequestToolPolicy = UnrealMcp::GetToolPolicy(RequestToolName);
		if (RequestToolPolicy.bRequiresLock && !EffectiveRequestArguments->HasField(TEXT("skipLock")))
		{
			EffectiveRequestArguments->SetBoolField(TEXT("skipLock"), true);
			bInjectedSkipLock = true;
		}

		ToolResult = ExecuteTool(RequestToolName, *EffectiveRequestArguments);
		bToolExecuted = true;
	}

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_tool_test"));
	StructuredContent->SetStringField(TEXT("toolName"), RequestToolName);
	StructuredContent->SetStringField(TEXT("testRequestPath"), ResolvedTestRequestPath);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
	StructuredContent->SetStringField(TEXT("endpointMode"), TEXT("in_process_mcp_handlers"));
	StructuredContent->SetStringField(TEXT("endpointNote"), TEXT("tools/list and tools/call are exercised through the same in-editor MCP handlers. A network self-call to tools/call from inside tools/call would deadlock on the editor game thread."));
	StructuredContent->SetNumberField(TEXT("toolCount"), ToolsArray.Num());
	StructuredContent->SetBoolField(TEXT("toolListed"), bToolListed);
	StructuredContent->SetBoolField(TEXT("toolExecuted"), bToolExecuted);
	StructuredContent->SetBoolField(TEXT("injectedSkipLockForInProcessTest"), bInjectedSkipLock);
	StructuredContent->SetBoolField(TEXT("expectToolListed"), bExpectToolListed);
	if (ListedToolObject.IsValid())
	{
		StructuredContent->SetObjectField(TEXT("listedTool"), ListedToolObject);
	}
	if (bToolExecuted)
	{
		StructuredContent->SetBoolField(TEXT("toolCallIsError"), ToolResult.bIsError);
		StructuredContent->SetStringField(TEXT("toolCallText"), ToolResult.Text);
		if (ToolResult.StructuredContent.IsValid())
		{
			StructuredContent->SetObjectField(TEXT("toolCallStructuredContent"), ToolResult.StructuredContent);
		}
	}
	StructuredContent->SetStringField(TEXT("testName"), TestName);
	StructuredContent->SetStringField(TEXT("testDescription"), TestDescription);
	StructuredContent->SetStringField(TEXT("expectationNote"), ExpectationNote);
	StructuredContent->SetBoolField(TEXT("isWrappedTestCase"), TestCaseObject.IsValid());
	StructuredContent->SetBoolField(TEXT("hasExpectedToolCallError"), bHasExpectToolCallError);
	if (bHasExpectToolCallError)
	{
		StructuredContent->SetBoolField(TEXT("expectToolCallError"), bExpectToolCallError);
	}

	const bool bListedExpectationOk = !bExpectToolListed || bToolListed;
	const bool bToolCallExpectationOk = !bExecuteTool
		|| (bToolExecuted && (bHasExpectToolCallError ? ToolResult.bIsError == bExpectToolCallError : !ToolResult.bIsError));
	const bool bSucceeded = bListedExpectationOk && bToolCallExpectationOk;
	StructuredContent->SetBoolField(TEXT("listedExpectationOk"), bListedExpectationOk);
	StructuredContent->SetBoolField(TEXT("toolCallExpectationOk"), bToolCallExpectationOk);
	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);

	if (bWriteProjectMemory)
	{
		TSharedPtr<FJsonObject> UpdatedMemoryContent = MakeShared<FJsonObject>();
		UpdatedMemoryContent->SetStringField(TEXT("toolName"), RequestToolName);
		UpdatedMemoryContent->SetStringField(TEXT("testRequestPath"), ResolvedTestRequestPath);
		UpdatedMemoryContent->SetBoolField(TEXT("toolListed"), bToolListed);
		UpdatedMemoryContent->SetBoolField(TEXT("toolExecuted"), bToolExecuted);
		UpdatedMemoryContent->SetBoolField(TEXT("testSucceeded"), bSucceeded);
		UpdatedMemoryContent->SetStringField(TEXT("testName"), TestName);
		UnrealMcp::WriteBuildTestMemory(
			MemoryKey,
			bSucceeded ? TEXT("MCP tool test succeeded.") : TEXT("MCP tool test failed or tool is not loaded."),
			bSucceeded ? TEXT("tool_test_succeeded") : TEXT("tool_test_failed"),
			bSucceeded ? TEXT("Continue with tool audit or next MCP extension stage.") : TEXT("If the tool is missing, restart Unreal Editor after a successful build, then rerun unreal.mcp_run_tool_test."),
			UpdatedMemoryContent);
	}

	FString Text;
	if (!bToolListed)
	{
		Text = FString::Printf(TEXT("Tool '%s' was not found in tools/list."), *RequestToolName);
	}
	else if (!bExecuteTool)
	{
		Text = FString::Printf(TEXT("Tool '%s' is listed. Execution was skipped by request."), *RequestToolName);
	}
	else
	{
		Text = FString::Printf(TEXT("Test '%s' tool '%s' listed=%s executed=%s isError=%s expectationOk=%s."),
			*TestName,
			*RequestToolName,
			bToolListed ? TEXT("true") : TEXT("false"),
			bToolExecuted ? TEXT("true") : TEXT("false"),
			ToolResult.bIsError ? TEXT("true") : TEXT("false"),
			bToolCallExpectationOk ? TEXT("true") : TEXT("false"));
	}

	return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bSucceeded);
}

FUnrealMcpExecutionResult FUnrealMcpModule::RunMcpTestSuite(const FJsonObject& Arguments) const
{
	FString ToolName;
	FString TestsDir;
	FString ScaffoldDir;
	FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
	FString MemoryKey = TEXT("mcp.extension.build_test");
	bool bReadProjectMemory = true;
	bool bWriteProjectMemory = true;
	bool bExecuteTool = true;
	bool bStopOnFailure = false;
	bool bFallbackToSingleTest = true;
	bool bIncludePassedStructuredContent = false;

	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
	Arguments.TryGetBoolField(TEXT("readProjectMemory"), bReadProjectMemory);
	Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
	Arguments.TryGetBoolField(TEXT("executeTool"), bExecuteTool);
	Arguments.TryGetBoolField(TEXT("stopOnFailure"), bStopOnFailure);
	Arguments.TryGetBoolField(TEXT("fallbackToSingleTest"), bFallbackToSingleTest);
	Arguments.TryGetBoolField(TEXT("includePassedStructuredContent"), bIncludePassedStructuredContent);

	ToolName = ToolName.TrimStartAndEnd();
	TestsDir = TestsDir.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
	if (MemoryKey.IsEmpty())
	{
		MemoryKey = TEXT("mcp.extension.build_test");
	}

	if (bReadProjectMemory)
	{
		FString FailureReason;
		TSharedPtr<FJsonObject> MemoryObject;
		if (UnrealMcp::LoadProjectMemory(MemoryObject, FailureReason) && MemoryObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (MemoryObject->TryGetArrayField(TEXT("entries"), Entries) && Entries)
			{
				for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
				{
					if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
					FString ExistingKey;
					if (!EntryObject->TryGetStringField(TEXT("key"), ExistingKey) || ExistingKey != MemoryKey)
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ContentObject = nullptr;
					if (EntryObject->TryGetObjectField(TEXT("content"), ContentObject) && ContentObject && (*ContentObject).IsValid())
					{
						if (ToolName.IsEmpty())
						{
							(*ContentObject)->TryGetStringField(TEXT("toolName"), ToolName);
						}
						if (TestsDir.IsEmpty())
						{
							(*ContentObject)->TryGetStringField(TEXT("testsDir"), TestsDir);
						}
						if (ScaffoldDir.IsEmpty())
						{
							(*ContentObject)->TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
						}
					}
					break;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
	ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
	ResolveArguments->SetStringField(TEXT("testsDir"), TestsDir);
	ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);

	FString ResolvedTestsDir;
	FString ResolvedScaffoldDir;
	FString ResolvedToolName;
	FString ResolveFailure;
	if (!UnrealMcp::ResolveMcpTestsDirectory(*ResolveArguments, ResolvedTestsDir, ResolvedScaffoldDir, ResolvedToolName, ResolveFailure))
	{
		return UnrealMcp::MakeExecutionResult(ResolveFailure, nullptr, true);
	}
	ToolName = ResolvedToolName;

	TArray<FString> TestFiles;
	if (FPaths::DirectoryExists(ResolvedTestsDir))
	{
		UnrealMcp::FindImmediateChildren(ResolvedTestsDir, TEXT("*.json"), true, false, TestFiles);
	}
	TestFiles.Sort();

	if (TestFiles.Num() == 0 && bFallbackToSingleTest)
	{
		TSharedPtr<FJsonObject> SingleArguments = MakeShared<FJsonObject>();
		SingleArguments->SetStringField(TEXT("toolName"), ToolName);
		SingleArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		SingleArguments->SetStringField(TEXT("testRequestPath"), FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json")));
		SingleArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		SingleArguments->SetBoolField(TEXT("readProjectMemory"), false);
		SingleArguments->SetBoolField(TEXT("writeProjectMemory"), false);
		SingleArguments->SetBoolField(TEXT("executeTool"), bExecuteTool);
		const FUnrealMcpExecutionResult SingleResult = RunMcpToolTest(*SingleArguments);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_test_suite"));
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
		StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		StructuredContent->SetBoolField(TEXT("fallbackToSingleTest"), true);
		StructuredContent->SetBoolField(TEXT("succeeded"), !SingleResult.bIsError);
		StructuredContent->SetNumberField(TEXT("total"), 1);
		StructuredContent->SetNumberField(TEXT("passed"), SingleResult.bIsError ? 0 : 1);
		StructuredContent->SetNumberField(TEXT("failed"), SingleResult.bIsError ? 1 : 0);
		StructuredContent->SetNumberField(TEXT("passRate"), SingleResult.bIsError ? 0.0 : 1.0);
		if (SingleResult.StructuredContent.IsValid())
		{
			StructuredContent->SetObjectField(TEXT("singleTest"), SingleResult.StructuredContent);
		}
		return UnrealMcp::MakeExecutionResult(
			SingleResult.bIsError ? TEXT("MCP test suite fallback single test failed.") : TEXT("MCP test suite fallback single test passed."),
			StructuredContent,
			SingleResult.bIsError);
	}

	if (TestFiles.Num() == 0)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_test_suite"));
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
		StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		StructuredContent->SetBoolField(TEXT("succeeded"), false);
		StructuredContent->SetNumberField(TEXT("total"), 0);
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("No JSON test cases found under '%s'."), *ResolvedTestsDir), StructuredContent, true);
	}

	TArray<TSharedPtr<FJsonValue>> TestResults;
	TArray<TSharedPtr<FJsonValue>> FailedCases;
	int32 PassedCount = 0;
	int32 FailedCount = 0;

	for (const FString& TestFile : TestFiles)
	{
		TSharedPtr<FJsonObject> TestArguments = MakeShared<FJsonObject>();
		TestArguments->SetStringField(TEXT("toolName"), ToolName);
		TestArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		TestArguments->SetStringField(TEXT("testRequestPath"), TestFile);
		TestArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		TestArguments->SetBoolField(TEXT("readProjectMemory"), false);
		TestArguments->SetBoolField(TEXT("writeProjectMemory"), false);
		TestArguments->SetBoolField(TEXT("executeTool"), bExecuteTool);

		const FUnrealMcpExecutionResult TestResult = RunMcpToolTest(*TestArguments);
		const bool bPassed = !TestResult.bIsError;
		PassedCount += bPassed ? 1 : 0;
		FailedCount += bPassed ? 0 : 1;

		TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetStringField(TEXT("path"), TestFile);
		ResultObject->SetStringField(TEXT("fileName"), FPaths::GetCleanFilename(TestFile));
		ResultObject->SetBoolField(TEXT("passed"), bPassed);
		ResultObject->SetBoolField(TEXT("isError"), TestResult.bIsError);
		ResultObject->SetStringField(TEXT("text"), TestResult.Text);
		if (TestResult.StructuredContent.IsValid())
		{
			FString TestName;
			if (TestResult.StructuredContent->TryGetStringField(TEXT("testName"), TestName))
			{
				ResultObject->SetStringField(TEXT("name"), TestName);
			}
			if (!bPassed || bIncludePassedStructuredContent)
			{
				ResultObject->SetObjectField(TEXT("structuredContent"), TestResult.StructuredContent);
			}
		}

		TestResults.Add(MakeShared<FJsonValueObject>(ResultObject));
		if (!bPassed)
		{
			FailedCases.Add(MakeShared<FJsonValueObject>(ResultObject));
			if (bStopOnFailure)
			{
				break;
			}
		}
	}

	const int32 ExecutedCount = PassedCount + FailedCount;
	const double PassRate = ExecutedCount > 0 ? static_cast<double>(PassedCount) / static_cast<double>(ExecutedCount) : 0.0;
	const bool bSucceeded = FailedCount == 0 && ExecutedCount == TestFiles.Num();

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_test_suite"));
	StructuredContent->SetStringField(TEXT("toolName"), ToolName);
	StructuredContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
	StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
	StructuredContent->SetBoolField(TEXT("executeTool"), bExecuteTool);
	StructuredContent->SetBoolField(TEXT("stopOnFailure"), bStopOnFailure);
	StructuredContent->SetNumberField(TEXT("total"), TestFiles.Num());
	StructuredContent->SetNumberField(TEXT("executed"), ExecutedCount);
	StructuredContent->SetNumberField(TEXT("passed"), PassedCount);
	StructuredContent->SetNumberField(TEXT("failed"), FailedCount);
	StructuredContent->SetNumberField(TEXT("passRate"), PassRate);
	StructuredContent->SetArrayField(TEXT("results"), TestResults);
	StructuredContent->SetArrayField(TEXT("failedCases"), FailedCases);

	if (bWriteProjectMemory)
	{
		TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
		MemoryContent->SetStringField(TEXT("toolName"), ToolName);
		MemoryContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		MemoryContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
		MemoryContent->SetBoolField(TEXT("testSuiteSucceeded"), bSucceeded);
		MemoryContent->SetNumberField(TEXT("total"), TestFiles.Num());
		MemoryContent->SetNumberField(TEXT("passed"), PassedCount);
		MemoryContent->SetNumberField(TEXT("failed"), FailedCount);
		MemoryContent->SetNumberField(TEXT("passRate"), PassRate);
		UnrealMcp::WriteBuildTestMemory(
			MemoryKey,
			bSucceeded ? TEXT("MCP test suite succeeded.") : TEXT("MCP test suite failed."),
			bSucceeded ? TEXT("test_suite_succeeded") : TEXT("test_suite_failed"),
			bSucceeded ? TEXT("Continue with tool audit or next MCP extension stage.") : TEXT("Inspect failedCases, patch snippets, rebuild, and rerun the suite."),
			MemoryContent);
	}

	return UnrealMcp::MakeExecutionResult(
		FString::Printf(TEXT("MCP test suite for %s: %d/%d passed (%.0f%%)."),
			*ToolName,
			PassedCount,
			TestFiles.Num(),
			PassRate * 100.0),
		StructuredContent,
		!bSucceeded);
}

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
	bool bApply = true;
	bool bBuild = true;
	bool bRunTest = true;
	bool bRunTestSuite = true;
	bool bGenerateTests = true;
	bool bOverwriteTests = true;
	bool bDryRunOnly = false;
	bool bApplyChatCommand = true;
	bool bCreateBackup = true;
	bool bBackupProjectState = true;
	bool bWriteProjectMemory = true;

	Arguments.TryGetStringField(TEXT("mode"), Mode);
	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("schemaJson"), SchemaJson);
	Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
	Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
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

	Mode = Mode.TrimStartAndEnd().ToLower();
	ToolName = ToolName.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	SchemaJson = SchemaJson.TrimStartAndEnd();
	TestRequestPath = TestRequestPath.TrimStartAndEnd();
	TestsDir = TestsDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
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
	bool bSucceeded = true;
	bool bRequiresRestart = false;
	bool bAppliedSourceChanges = false;
	bool bBuildSucceeded = false;

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_extension_pipeline"));
	StructuredContent->SetStringField(TEXT("mode"), Mode);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);

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
	TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
	ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
	ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
	if (!UnrealMcp::ResolveMcpScaffoldDirectory(*ResolveArguments, ResolvedScaffoldDir, ResolvedToolName, ResolveFailure))
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("resolve_scaffold"), TEXT("failed"), ResolveFailure)));
		StructuredContent->SetBoolField(TEXT("succeeded"), false);
		StructuredContent->SetBoolField(TEXT("requiresRestart"), false);
		StructuredContent->SetArrayField(TEXT("steps"), Steps);
		StructuredContent->SetArrayField(TEXT("issues"), Issues);
		return UnrealMcp::MakeExecutionResult(ResolveFailure, StructuredContent, true);
	}

	ToolName = ResolvedToolName;
	if (TestRequestPath.IsEmpty())
	{
		TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
	}
	if (TestsDir.IsEmpty())
	{
		TestsDir = FPaths::Combine(ResolvedScaffoldDir, TEXT("Tests"));
	}
	StructuredContent->SetStringField(TEXT("toolName"), ToolName);
	StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
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
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("validate_schema"),
			ValidateResult.bIsError ? TEXT("failed") : TEXT("completed"),
			ValidateResult.Text,
			&ValidateResult)));
		if (ValidateResult.bIsError)
		{
			bSucceeded = false;
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
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("generate_tests"),
			GenerateTestsResult.bIsError ? TEXT("failed") : TEXT("completed"),
			GenerateTestsResult.Text,
			&GenerateTestsResult)));
		if (GenerateTestsResult.bIsError)
		{
			bSucceeded = false;
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
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("apply_dry_run"),
			DryRunResult.bIsError ? TEXT("failed") : TEXT("completed"),
			DryRunResult.Text,
			&DryRunResult)));
		if (DryRunResult.bIsError)
		{
			bSucceeded = false;
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

	if (bSucceeded && !bDryRunOnly && bBackupProjectState && (bApply || bBuild || bRunTest))
	{
		TSharedPtr<FJsonObject> BackupArguments = MakeShared<FJsonObject>();
		BackupArguments->SetStringField(TEXT("label"), FString::Printf(TEXT("pipeline_%s"), *ToolName));
		BackupArguments->SetStringField(TEXT("reason"), FString::Printf(TEXT("Pre-pipeline snapshot before applying/building/testing MCP tool %s."), *ToolName));
		BackupArguments->SetBoolField(TEXT("includeBuildLogs"), false);
		const FUnrealMcpExecutionResult BackupResult = UnrealMcp::BackupProjectState(*BackupArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("backup_project_state"),
			BackupResult.bIsError ? TEXT("failed") : TEXT("completed"),
			BackupResult.Text,
			&BackupResult)));
		if (BackupResult.bIsError)
		{
			bSucceeded = false;
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
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("apply"),
			ApplyResult.bIsError ? TEXT("failed") : TEXT("completed"),
			ApplyResult.Text,
			&ApplyResult)));
		if (ApplyResult.bIsError)
		{
			bSucceeded = false;
		}
		else if (ApplyResult.StructuredContent.IsValid())
		{
			ApplyResult.StructuredContent->TryGetBoolField(TEXT("changed"), bAppliedSourceChanges);
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
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("build"),
			BuildResult.bIsError ? TEXT("failed") : TEXT("completed"),
			BuildResult.Text,
			&BuildResult)));
		if (BuildResult.bIsError)
		{
			bSucceeded = false;
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

	const bool bShouldDeferTestForRestart = bBuild && bBuildSucceeded && bAppliedSourceChanges && !bToolAlreadyListed;
	if (bShouldDeferTestForRestart)
	{
		bRequiresRestart = true;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("restart"),
			TEXT("required"),
			TEXT("New C++ snippets were compiled while the editor was running. Restart Unreal Editor before running the test step."))));
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TEXT("deferred"),
			bRunTestSuite ? TEXT("Run unreal.mcp_extension_pipeline with mode=resume_test after restart to execute the generated test suite.") : TEXT("Run unreal.mcp_extension_pipeline with mode=resume_test after restart, or use Tools/unreal_mcp_supervisor.py resume-test."))));
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
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TestResult.bIsError ? TEXT("failed") : TEXT("completed"),
			TestResult.Text,
			&TestResult)));
		if (TestResult.bIsError)
		{
			bSucceeded = false;
		}
	}
	else if (!bRunTest)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("test"), TEXT("skipped"), TEXT("runTest=false."))));
	}

	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
	StructuredContent->SetBoolField(TEXT("requiresRestart"), bRequiresRestart);
	StructuredContent->SetBoolField(TEXT("appliedSourceChanges"), bAppliedSourceChanges);
	StructuredContent->SetBoolField(TEXT("buildSucceeded"), bBuildSucceeded);
	StructuredContent->SetBoolField(TEXT("generateTests"), bGenerateTests);
	StructuredContent->SetBoolField(TEXT("runTestSuite"), bRunTestSuite);
	StructuredContent->SetBoolField(TEXT("backupProjectState"), bBackupProjectState);
	StructuredContent->SetStringField(TEXT("restartAdvice"), TEXT("If requiresRestart=true, close and reopen Unreal Editor, then call unreal.mcp_extension_pipeline with mode=resume_test and the same memoryKey."));
	StructuredContent->SetStringField(TEXT("supervisorCommand"), FString::Printf(TEXT("python3 Tools/unreal_mcp_supervisor.py resume-test --memory-key %s"), *MemoryKey));
	StructuredContent->SetArrayField(TEXT("steps"), Steps);
	StructuredContent->SetArrayField(TEXT("issues"), Issues);

	const FString Text = bRequiresRestart
		? TEXT("MCP extension pipeline applied and built changes. Restart Unreal Editor, then resume test.")
		: (bSucceeded ? TEXT("MCP extension pipeline completed.") : TEXT("MCP extension pipeline failed. See steps for details."));
	return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bSucceeded);
}
