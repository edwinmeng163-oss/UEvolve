#include "UnrealMcpToolOutcomeVerifiers.h"

#include "UnrealMcpModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UnrealMcp
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> WorkflowMakeStringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TSharedPtr<FJsonObject> WorkflowMakeVerifierResult(const FString& ToolName, const FString& Category)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("toolName"), ToolName);
			Object->SetStringField(TEXT("category"), Category);
			Object->SetStringField(TEXT("checkLevel"), TEXT("workflow_state"));
			Object->SetBoolField(TEXT("toolSpecificVerifierAvailable"), true);
			return Object;
		}

		void WorkflowFinishVerifier(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Evidence,
			const TArray<FString>& Failures,
			const FString& SuccessSummary,
			const FString& FailureSummary)
		{
			Object->SetBoolField(TEXT("verified"), Failures.Num() == 0);
			Object->SetArrayField(TEXT("evidence"), WorkflowMakeStringValues(Evidence));
			Object->SetArrayField(TEXT("failures"), WorkflowMakeStringValues(Failures));
			Object->SetStringField(TEXT("summary"), Failures.Num() == 0 ? SuccessSummary : FailureSummary);
		}

		FString ResolveFilesystemPath(const FString& RawPath)
		{
			const FString Trimmed = RawPath.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				return FString();
			}
			if (FPaths::IsRelative(Trimmed))
			{
				return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Trimmed));
			}
			return FPaths::ConvertRelativePathToFull(Trimmed);
		}

		bool LoadJsonFile(const FString& RawPath, TSharedPtr<FJsonObject>& OutObject)
		{
			const FString Path = ResolveFilesystemPath(RawPath);
			FString JsonText;
			if (Path.IsEmpty() || !FFileHelper::LoadFileToString(JsonText, *Path))
			{
				return false;
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
		}

		FString GetProjectMemoryPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ProjectMemory.json")));
		}

		TSharedPtr<FJsonObject> FindMemoryEntryByKey(const FString& MemoryPath, const FString& Key)
		{
			TSharedPtr<FJsonObject> MemoryObject;
			if (!LoadJsonFile(MemoryPath, MemoryObject))
			{
				return nullptr;
			}

			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!MemoryObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
			{
				return nullptr;
			}

			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
				{
					continue;
				}
				FString ExistingKey;
				EntryValue->AsObject()->TryGetStringField(TEXT("key"), ExistingKey);
				if (ExistingKey == Key)
				{
					return EntryValue->AsObject();
				}
			}
			return nullptr;
		}

		bool TryGetResultString(const FUnrealMcpExecutionResult& Result, const FString& FieldName, FString& OutValue)
		{
			return Result.StructuredContent.IsValid() && Result.StructuredContent->TryGetStringField(FieldName, OutValue);
		}

		bool TryGetResultBool(const FUnrealMcpExecutionResult& Result, const FString& FieldName, bool& OutValue)
		{
			return Result.StructuredContent.IsValid() && Result.StructuredContent->TryGetBoolField(FieldName, OutValue);
		}

		bool FileExistsField(const FUnrealMcpExecutionResult& Result, const FString& FieldName, FString& OutResolvedPath)
		{
			FString Path;
			if (!TryGetResultString(Result, FieldName, Path))
			{
				return false;
			}
			OutResolvedPath = ResolveFilesystemPath(Path);
			return FPaths::FileExists(OutResolvedPath);
		}

		bool DirectoryExistsField(const FUnrealMcpExecutionResult& Result, const FString& FieldName, FString& OutResolvedPath)
		{
			FString Path;
			if (!TryGetResultString(Result, FieldName, Path))
			{
				return false;
			}
			OutResolvedPath = ResolveFilesystemPath(Path);
			return FPaths::DirectoryExists(OutResolvedPath);
		}

		FString WorkflowCategoryForTool(const FString& ToolName)
		{
			if (ToolName.StartsWith(TEXT("unreal.project_memory_")))
			{
				return TEXT("memory");
			}
			if (ToolName.StartsWith(TEXT("unreal.skill_")))
			{
				return TEXT("skills");
			}
			if (ToolName.StartsWith(TEXT("unreal.scaffold_")))
			{
				return TEXT("scaffold");
			}
			if (ToolName.StartsWith(TEXT("unreal.mcp_")))
			{
				return TEXT("self-extension");
			}
			return FString();
		}

		void VerifyFileArrayField(
			const TSharedPtr<FJsonObject>& StructuredContent,
			const FString& FieldName,
			const FString& PathFieldName,
			TArray<FString>& Evidence,
			TArray<FString>& Failures)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!StructuredContent.IsValid() || !StructuredContent->TryGetArrayField(FieldName, Values) || !Values)
			{
				return;
			}

			int32 CheckedCount = 0;
			int32 ExistingCount = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
				{
					continue;
				}
				FString Path;
				if (!Value->AsObject()->TryGetStringField(PathFieldName, Path) && !Value->AsObject()->TryGetStringField(TEXT("path"), Path))
				{
					continue;
				}
				++CheckedCount;
				const FString ResolvedPath = ResolveFilesystemPath(Path);
				if (FPaths::FileExists(ResolvedPath) || FPaths::DirectoryExists(ResolvedPath))
				{
					++ExistingCount;
				}
			}
			if (CheckedCount > 0)
			{
				Evidence.Add(FString::Printf(TEXT("%s contains %d path entries; %d exist on disk."), *FieldName, CheckedCount, ExistingCount));
				if (ExistingCount < CheckedCount)
				{
					Failures.Add(FString::Printf(TEXT("%d %s path entries did not exist on disk."), CheckedCount - ExistingCount, *FieldName));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> BuildWorkflowToolPreflight(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TSharedPtr<FJsonObject>& GenericPreflight)
	{
		const FString Category = WorkflowCategoryForTool(ToolName);
		if (Category.IsEmpty() || !GenericPreflight.IsValid())
		{
			return nullptr;
		}

		TArray<FString> Evidence;
		TArray<FString> Failures;
		GenericPreflight->SetBoolField(TEXT("toolSpecificPreflightAvailable"), true);
		GenericPreflight->SetStringField(TEXT("category"), Category);
		GenericPreflight->SetStringField(TEXT("checkLevel"), TEXT("workflow_preflight"));

		if (Category == TEXT("memory"))
		{
			const FString MemoryPath = GetProjectMemoryPath();
			Evidence.Add(FString::Printf(TEXT("Project memory path resolved to %s."), *MemoryPath));
			FString Key;
			Arguments.TryGetStringField(TEXT("key"), Key);
			if ((ToolName == TEXT("unreal.project_memory_write")
				|| ToolName == TEXT("unreal.project_memory_edit")
				|| ToolName == TEXT("unreal.project_memory_delete"))
				&& Key.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(TEXT("key is required for this project memory write operation."));
			}
			else if (!Key.TrimStartAndEnd().IsEmpty())
			{
				Evidence.Add(FindMemoryEntryByKey(MemoryPath, Key).IsValid()
					? FString::Printf(TEXT("Project memory key '%s' exists before execution."), *Key)
					: FString::Printf(TEXT("Project memory key '%s' does not exist before execution."), *Key));
			}
		}
		else if (Category == TEXT("skills"))
		{
			FString SkillName;
			FString DraftPath;
			Arguments.TryGetStringField(TEXT("skillName"), SkillName);
			Arguments.TryGetStringField(TEXT("draftPath"), DraftPath);
			if ((ToolName == TEXT("unreal.skill_save_draft")
				|| ToolName == TEXT("unreal.skill_promote_draft"))
				&& SkillName.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(TEXT("skillName is required for this skill write operation."));
			}
			if (!DraftPath.TrimStartAndEnd().IsEmpty())
			{
				const FString ResolvedDraftPath = ResolveFilesystemPath(DraftPath);
				if (FPaths::FileExists(ResolvedDraftPath))
				{
					Evidence.Add(FString::Printf(TEXT("Skill draft path exists before execution: %s."), *ResolvedDraftPath));
				}
				else if (ToolName == TEXT("unreal.skill_promote_draft"))
				{
					Failures.Add(FString::Printf(TEXT("Skill draft path does not exist before promote: %s."), *ResolvedDraftPath));
				}
			}
			Evidence.Add(TEXT("Skill roots are project-local; promotion writes to Tools/UnrealMcpSkills only after tool validation."));
		}
		else if (Category == TEXT("scaffold"))
		{
			FString RootPath;
			FString ToolNameArg;
			Arguments.TryGetStringField(TEXT("rootPath"), RootPath);
			Arguments.TryGetStringField(TEXT("toolName"), ToolNameArg);
			if (ToolName == TEXT("unreal.scaffold_mcp_tool") && ToolNameArg.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(TEXT("toolName is required for scaffold_mcp_tool."));
			}
			Evidence.Add(RootPath.TrimStartAndEnd().IsEmpty()
				? TEXT("No rootPath supplied; scaffold tool will use its default project content root.")
				: FString::Printf(TEXT("Requested scaffold rootPath: %s."), *RootPath));
		}
		else if (Category == TEXT("self-extension"))
		{
			if (ToolName == TEXT("unreal.mcp_apply_scaffold") || ToolName == TEXT("unreal.mcp_patch_scaffold_snippet"))
			{
				FString ScaffoldDir;
				Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				if (!ScaffoldDir.TrimStartAndEnd().IsEmpty())
				{
					const FString ResolvedScaffoldDir = ResolveFilesystemPath(ScaffoldDir);
					if (FPaths::DirectoryExists(ResolvedScaffoldDir))
					{
						Evidence.Add(FString::Printf(TEXT("Scaffold directory exists before execution: %s."), *ResolvedScaffoldDir));
					}
					else
					{
						Failures.Add(FString::Printf(TEXT("Scaffold directory does not exist before execution: %s."), *ResolvedScaffoldDir));
					}
				}
				else
				{
					Evidence.Add(TEXT("No scaffoldDir supplied; tool may resolve it from toolName/outputRoot."));
				}
			}
			if (ToolName.Contains(TEXT("rollback")))
			{
				FString ManifestPath;
				Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
				Evidence.Add(ManifestPath.TrimStartAndEnd().IsEmpty()
					? TEXT("No manifestPath supplied; rollback tool will use the latest extension manifest.")
					: FString::Printf(TEXT("Requested manifestPath: %s."), *ManifestPath));
			}
		}

		GenericPreflight->SetBoolField(TEXT("ready"), Failures.Num() == 0);
		GenericPreflight->SetArrayField(TEXT("evidence"), WorkflowMakeStringValues(Evidence));
		GenericPreflight->SetArrayField(TEXT("failures"), WorkflowMakeStringValues(Failures));
		GenericPreflight->SetStringField(TEXT("summary"), Failures.Num() == 0
			? TEXT("Workflow preflight confirmed required paths, keys, or request shape before execution.")
			: TEXT("Workflow preflight found missing state; inspect failures before applying."));
		return GenericPreflight;
	}

	TSharedPtr<FJsonObject> VerifyWorkflowToolOutcome(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const FUnrealMcpExecutionResult& Result)
	{
		const FString Category = WorkflowCategoryForTool(ToolName);
		if (Category.IsEmpty())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Verifier = WorkflowMakeVerifierResult(ToolName, Category);
		Verifier->SetBoolField(TEXT("toolReturnedError"), Result.bIsError);
		Verifier->SetBoolField(TEXT("genericResultSucceeded"), !Result.bIsError);
		TArray<FString> Evidence;
		TArray<FString> Failures;

		if (Result.bIsError)
		{
			Failures.Add(TEXT("Tool returned an error; workflow success state was not verified."));
			WorkflowFinishVerifier(Verifier, Evidence, Failures, TEXT("Workflow state verified."), TEXT("Workflow verifier found mismatches; inspect failures for details."));
			return Verifier;
		}
		if (!Result.StructuredContent.IsValid())
		{
			Failures.Add(TEXT("Workflow tool did not return structured content to verify."));
			WorkflowFinishVerifier(Verifier, Evidence, Failures, TEXT("Workflow state verified."), TEXT("Workflow verifier found mismatches; inspect failures for details."));
			return Verifier;
		}

		bool bDryRun = false;
		TryGetResultBool(Result, TEXT("dryRun"), bDryRun);
		if (bDryRun)
		{
			Evidence.Add(TEXT("Dry-run result verified by structured content; no destructive state check required."));
		}

		if (Category == TEXT("memory"))
		{
			FString MemoryPath;
			FString Key;
			TryGetResultString(Result, TEXT("path"), MemoryPath);
			TryGetResultString(Result, TEXT("key"), Key);
			if (MemoryPath.TrimStartAndEnd().IsEmpty())
			{
				MemoryPath = GetProjectMemoryPath();
			}

			if (ToolName == TEXT("unreal.project_memory_delete") && !bDryRun)
			{
				if (!FindMemoryEntryByKey(MemoryPath, Key).IsValid())
				{
					Evidence.Add(FString::Printf(TEXT("Project memory key '%s' is absent after delete."), *Key));
				}
				else
				{
					Failures.Add(FString::Printf(TEXT("Project memory key '%s' still exists after delete."), *Key));
				}
			}
			else if (ToolName == TEXT("unreal.project_memory_write") || ToolName == TEXT("unreal.project_memory_edit"))
			{
				if (FindMemoryEntryByKey(MemoryPath, Key).IsValid())
				{
					Evidence.Add(FString::Printf(TEXT("Project memory key '%s' exists after write/edit."), *Key));
				}
				else if (!bDryRun)
				{
					Failures.Add(FString::Printf(TEXT("Project memory key '%s' was not found after write/edit."), *Key));
				}
			}
		}
		else if (Category == TEXT("skills"))
		{
			if (ToolName == TEXT("unreal.skill_recording_start"))
			{
				bool bRecording = false;
				TryGetResultBool(Result, TEXT("recording"), bRecording);
				bRecording ? Evidence.Add(TEXT("Skill activity recording is active after start.")) : Failures.Add(TEXT("Skill activity recording is not active after start."));
			}
			else if (ToolName == TEXT("unreal.skill_recording_stop"))
			{
				bool bRecording = true;
				TryGetResultBool(Result, TEXT("recording"), bRecording);
				!bRecording ? Evidence.Add(TEXT("Skill activity recording is stopped after stop.")) : Failures.Add(TEXT("Skill activity recording is still active after stop."));
			}
			else if (ToolName == TEXT("unreal.skill_distill_from_activity") || ToolName == TEXT("unreal.skill_save_draft"))
			{
				FString DraftPath;
				if (FileExistsField(Result, TEXT("draftPath"), DraftPath))
				{
					Evidence.Add(FString::Printf(TEXT("Skill draft exists after execution: %s."), *DraftPath));
				}
				else
				{
					bool bWriteDraft = true;
					TryGetResultBool(Result, TEXT("writeDraft"), bWriteDraft);
					if (bWriteDraft)
					{
						Failures.Add(TEXT("Expected skill draft file was not found after execution."));
					}
					else
					{
						Evidence.Add(TEXT("writeDraft=false; draft text was returned without writing a file."));
					}
				}
			}
			else if (ToolName == TEXT("unreal.skill_promote_draft"))
			{
				if (!bDryRun)
				{
					FString PromotedPath;
					if (FileExistsField(Result, TEXT("promotedPath"), PromotedPath))
					{
						Evidence.Add(FString::Printf(TEXT("Promoted skill exists after execution: %s."), *PromotedPath));
					}
					else
					{
						Failures.Add(TEXT("Promoted skill file was not found after execution."));
					}
				}
			}
			else if (ToolName == TEXT("unreal.skill_apply"))
			{
				FString SkillPath;
				if (FileExistsField(Result, TEXT("skillPath"), SkillPath))
				{
					Evidence.Add(FString::Printf(TEXT("Applied skill file exists: %s."), *SkillPath));
				}
			}
		}
		else if (Category == TEXT("scaffold"))
		{
			if (ToolName == TEXT("unreal.scaffold_mcp_tool"))
			{
				FString ToolDirectory;
				if (DirectoryExistsField(Result, TEXT("directory"), ToolDirectory))
				{
					Evidence.Add(FString::Printf(TEXT("MCP scaffold directory exists: %s."), *ToolDirectory));
				}
				else
				{
					Failures.Add(TEXT("MCP scaffold directory was not found after execution."));
				}
				VerifyFileArrayField(Result.StructuredContent, TEXT("files"), TEXT("path"), Evidence, Failures);
			}
			else
			{
				const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Directories = nullptr;
				if (Result.StructuredContent->TryGetArrayField(TEXT("assets"), Assets) && Assets)
				{
					Evidence.Add(FString::Printf(TEXT("Gameplay scaffold reported %d asset entries."), Assets->Num()));
				}
				if (Result.StructuredContent->TryGetArrayField(TEXT("directories"), Directories) && Directories)
				{
					Evidence.Add(FString::Printf(TEXT("Gameplay scaffold reported %d directories."), Directories->Num()));
				}
			}
		}
		else if (Category == TEXT("self-extension"))
		{
			if (ToolName == TEXT("unreal.mcp_apply_scaffold"))
			{
				bool bCanApply = false;
				Result.StructuredContent->TryGetBoolField(TEXT("canApply"), bCanApply);
				if (bDryRun)
				{
					bCanApply ? Evidence.Add(TEXT("Apply scaffold dry-run reports canApply=true.")) : Failures.Add(TEXT("Apply scaffold dry-run reports canApply=false."));
				}
				else
				{
					FString ManifestPath;
					if (FileExistsField(Result, TEXT("manifestPath"), ManifestPath))
					{
						Evidence.Add(FString::Printf(TEXT("Apply scaffold manifest exists: %s."), *ManifestPath));
					}
					else
					{
						Failures.Add(TEXT("Apply scaffold manifest was not found after execution."));
					}
				}
			}
			else if (ToolName.Contains(TEXT("rollback")))
			{
				bool bRolledBack = false;
				TryGetResultBool(Result, TEXT("rolledBack"), bRolledBack);
				if (bDryRun)
				{
					Evidence.Add(TEXT("Rollback dry-run verified by structured content."));
				}
				else
				{
					bRolledBack ? Evidence.Add(TEXT("Rollback reports rolledBack=true.")) : Failures.Add(TEXT("Rollback did not report rolledBack=true."));
				}
			}
			else if (ToolName == TEXT("unreal.mcp_build_editor"))
			{
				bool bSucceeded = false;
				Result.StructuredContent->TryGetBoolField(TEXT("succeeded"), bSucceeded);
				FString BuildLogPath;
				const bool bBuildLogExists = FileExistsField(Result, TEXT("buildLogPath"), BuildLogPath);
				if (bSucceeded && bBuildLogExists)
				{
					Evidence.Add(FString::Printf(TEXT("Build succeeded and build log exists: %s."), *BuildLogPath));
				}
				else
				{
					if (!bSucceeded)
					{
						Failures.Add(TEXT("Build did not report succeeded=true."));
					}
					if (!bBuildLogExists)
					{
						Failures.Add(TEXT("Build log was not found after build."));
					}
				}
			}
			else if (ToolName == TEXT("unreal.mcp_run_tool_test") || ToolName == TEXT("unreal.mcp_run_test_suite") || ToolName == TEXT("unreal.mcp_extension_pipeline"))
			{
				bool bSucceeded = false;
				if (Result.StructuredContent->TryGetBoolField(TEXT("succeeded"), bSucceeded) && bSucceeded)
				{
					Evidence.Add(TEXT("Self-extension test/pipeline reports succeeded=true."));
				}
				else
				{
					Failures.Add(TEXT("Self-extension test/pipeline did not report succeeded=true."));
				}
			}
			else
			{
				Evidence.Add(TEXT("Self-extension structured result was present; no deeper file verifier is required for this tool."));
			}
		}

		if (Evidence.Num() == 0 && Failures.Num() == 0)
		{
			Evidence.Add(TEXT("Workflow structured result was present; no deeper verifier was available for this tool."));
		}

		WorkflowFinishVerifier(
			Verifier,
			Evidence,
			Failures,
			TEXT("Workflow verifier confirmed expected file, memory, skill, scaffold, build, or test state."),
			TEXT("Workflow verifier found mismatches; inspect failures for details."));
		return Verifier;
	}
}
