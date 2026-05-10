#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
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

		const bool bFunctionalHealthy = SchemaIncompatibleCount <= 0.0
			&& MissingHandlerCount <= 0.0
			&& !PipelineResult.bIsError;
		const bool bDocumentationHealthy = MissingDocumentationCount <= 0.0;
		const bool bHealthy = bFunctionalHealthy;
		StructuredContent->SetBoolField(TEXT("healthy"), bHealthy);
		StructuredContent->SetBoolField(TEXT("functionalHealthy"), bFunctionalHealthy);
		StructuredContent->SetBoolField(TEXT("documentationHealthy"), bDocumentationHealthy);
		StructuredContent->SetNumberField(TEXT("documentationWarningCount"), MissingDocumentationCount);

		FString RecommendedNextStep;
		if (!bFunctionalHealthy)
		{
			RecommendedNextStep = TEXT("Run unreal.mcp_tool_audit and address schema, handler, or pipeline errors before adding more tools.");
		}
		else if (!bDocumentationHealthy)
		{
			RecommendedNextStep = TEXT("MCP is functionally healthy. Documentation warnings are non-blocking; fix docsPath packaging when preparing a release.");
		}
		else
		{
			RecommendedNextStep = TEXT("Continue with ToolRegistry modularization and add versioned test fixtures for core self-extension tools.");
		}
		StructuredContent->SetStringField(TEXT("recommendedNextStep"), RecommendedNextStep);

		const FString Text = FString::Printf(
			TEXT("MCP workbench status: visibleTools=%d schemaIncompatible=%d missingHandlers=%d documentationWarnings=%d memoryEntries=%d testScaffolds=%d testCases=%d healthy=%s functionalHealthy=%s documentationHealthy=%s"),
			ToolsArray.Num(),
			static_cast<int32>(SchemaIncompatibleCount),
			static_cast<int32>(MissingHandlerCount),
			static_cast<int32>(MissingDocumentationCount),
			static_cast<int32>(MemoryEntryCount),
			static_cast<int32>(TestScaffoldCount),
			TestCaseCount,
			bHealthy ? TEXT("true") : TEXT("false"),
			bFunctionalHealthy ? TEXT("true") : TEXT("false"),
			bDocumentationHealthy ? TEXT("true") : TEXT("false"));
		return MakeExecutionResult(Text, StructuredContent, false);
		}

}
