#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace UnrealMcp
{
		bool ShouldIncludeCleanupCandidate(const FString& Path, double MaxAgeDays, const FString& NameContains, FString& OutSkipReason)
		{
			OutSkipReason.Reset();
			if (!NameContains.TrimStartAndEnd().IsEmpty()
				&& !Path.Contains(NameContains.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				OutSkipReason = TEXT("nameContains filter did not match.");
				return false;
			}

			if (MaxAgeDays > 0.0)
			{
				const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
				if (!Stat.bIsValid || Stat.ModificationTime.GetTicks() <= 0)
				{
					OutSkipReason = TEXT("age filter requested but modification time is unavailable.");
					return false;
				}

				const double AgeDays = (FDateTime::Now() - Stat.ModificationTime).GetTotalDays();
				if (AgeDays < MaxAgeDays)
				{
					OutSkipReason = FString::Printf(TEXT("age %.2f days is newer than maxAgeDays %.2f."), AgeDays, MaxAgeDays);
					return false;
				}
			}
			return true;
		}

		void AddCleanupCandidate(
			const FString& Category,
			const FString& Path,
			const FString& Type,
			double MaxAgeDays,
			const FString& NameContains,
			TArray<TSharedPtr<FJsonValue>>& Candidates,
			TArray<TSharedPtr<FJsonValue>>& Skipped)
		{
			TSharedPtr<FJsonObject> CandidateObject = MakeFileInfoObject(Path);
			CandidateObject->SetStringField(TEXT("category"), Category);
			CandidateObject->SetStringField(TEXT("type"), Type);

			if (!IsPathInsideDirectory(Path, GetUnrealMcpSavedRoot()))
			{
				CandidateObject->SetStringField(TEXT("skipReason"), TEXT("path is outside Saved/UnrealMcp safety root."));
				Skipped.Add(MakeShared<FJsonValueObject>(CandidateObject));
				return;
			}

			FString SkipReason;
			if (!ShouldIncludeCleanupCandidate(Path, MaxAgeDays, NameContains, SkipReason))
			{
				CandidateObject->SetStringField(TEXT("skipReason"), SkipReason);
				Skipped.Add(MakeShared<FJsonValueObject>(CandidateObject));
				return;
			}

			Candidates.Add(MakeShared<FJsonValueObject>(CandidateObject));
		}

		void AddCleanupDirectoryChildren(
			const FString& Category,
			const FString& Directory,
			double MaxAgeDays,
			const FString& NameContains,
			TArray<TSharedPtr<FJsonValue>>& Candidates,
			TArray<TSharedPtr<FJsonValue>>& Skipped)
		{
			TArray<FString> Children;
			FindImmediateChildren(Directory, TEXT("*"), false, true, Children);
			for (const FString& Child : Children)
			{
				AddCleanupCandidate(Category, Child, TEXT("directory"), MaxAgeDays, NameContains, Candidates, Skipped);
			}
		}

		void AddCleanupFiles(
			const FString& Category,
			const FString& Directory,
			const FString& Pattern,
			double MaxAgeDays,
			const FString& NameContains,
			TArray<TSharedPtr<FJsonValue>>& Candidates,
			TArray<TSharedPtr<FJsonValue>>& Skipped)
		{
			TArray<FString> Files;
			FindImmediateChildren(Directory, Pattern, true, false, Files);
			for (const FString& File : Files)
			{
				AddCleanupCandidate(Category, File, TEXT("file"), MaxAgeDays, NameContains, Candidates, Skipped);
			}
		}

		FUnrealMcpExecutionResult CleanMcpTestArtifacts(const FJsonObject& Arguments)
		{
			bool bDryRun = true;
			bool bCleanTestScaffolds = true;
			bool bCleanTestRequests = false;
			bool bCleanBuildLogs = false;
			bool bCleanExtensionBackups = false;
			bool bCleanProjectMemory = false;
			double MaxAgeDays = 0.0;
			FString NameContains;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("cleanTestScaffolds"), bCleanTestScaffolds);
			Arguments.TryGetBoolField(TEXT("cleanTestRequests"), bCleanTestRequests);
			Arguments.TryGetBoolField(TEXT("cleanBuildLogs"), bCleanBuildLogs);
			Arguments.TryGetBoolField(TEXT("cleanExtensionBackups"), bCleanExtensionBackups);
			Arguments.TryGetBoolField(TEXT("cleanProjectMemory"), bCleanProjectMemory);
			Arguments.TryGetNumberField(TEXT("maxAgeDays"), MaxAgeDays);
			Arguments.TryGetStringField(TEXT("nameContains"), NameContains);
			MaxAgeDays = FMath::Max(0.0, MaxAgeDays);

			if (!bDryRun && IsEditorPlaying())
			{
				return MakePieBlockedResult(TEXT("unreal.mcp_clean_test_artifacts"));
			}

			TArray<TSharedPtr<FJsonValue>> Candidates;
			TArray<TSharedPtr<FJsonValue>> Skipped;
			if (bCleanTestScaffolds)
			{
				AddCleanupDirectoryChildren(TEXT("testScaffolds"), FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanTestRequests)
			{
				AddCleanupDirectoryChildren(TEXT("testRequests"), FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestRequests")), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanBuildLogs)
			{
				AddCleanupFiles(TEXT("buildLogs"), GetMcpBuildLogRoot(), TEXT("*.log"), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanExtensionBackups)
			{
				AddCleanupDirectoryChildren(TEXT("extensionBackups"), GetMcpExtensionBackupRoot(), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanProjectMemory)
			{
				AddCleanupCandidate(TEXT("projectMemory"), GetProjectMemoryFilePath(), TEXT("file"), MaxAgeDays, NameContains, Candidates, Skipped);
			}

			TArray<TSharedPtr<FJsonValue>> Deleted;
			TArray<TSharedPtr<FJsonValue>> DeleteErrors;
			if (!bDryRun)
			{
				for (const TSharedPtr<FJsonValue>& CandidateValue : Candidates)
				{
					if (!CandidateValue.IsValid() || CandidateValue->Type != EJson::Object || !CandidateValue->AsObject().IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> CandidateObject = CandidateValue->AsObject();
					FString Path;
					FString Type;
					CandidateObject->TryGetStringField(TEXT("path"), Path);
					CandidateObject->TryGetStringField(TEXT("type"), Type);

					bool bDeleted = false;
					if (Type == TEXT("directory"))
					{
						bDeleted = IFileManager::Get().DeleteDirectory(*Path, false, true);
					}
					else
					{
						bDeleted = IFileManager::Get().Delete(*Path, false, true, true);
					}

					TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
					ResultObject->SetStringField(TEXT("path"), Path);
					ResultObject->SetStringField(TEXT("type"), Type);
					ResultObject->SetBoolField(TEXT("deleted"), bDeleted);
					if (bDeleted)
					{
						Deleted.Add(MakeShared<FJsonValueObject>(ResultObject));
					}
					else
					{
						ResultObject->SetStringField(TEXT("error"), TEXT("delete operation returned false."));
						DeleteErrors.Add(MakeShared<FJsonValueObject>(ResultObject));
					}
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_clean_test_artifacts"));
			StructuredContent->SetStringField(TEXT("savedRoot"), GetUnrealMcpSavedRoot());
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("cleanTestScaffolds"), bCleanTestScaffolds);
			StructuredContent->SetBoolField(TEXT("cleanTestRequests"), bCleanTestRequests);
			StructuredContent->SetBoolField(TEXT("cleanBuildLogs"), bCleanBuildLogs);
			StructuredContent->SetBoolField(TEXT("cleanExtensionBackups"), bCleanExtensionBackups);
			StructuredContent->SetBoolField(TEXT("cleanProjectMemory"), bCleanProjectMemory);
			StructuredContent->SetNumberField(TEXT("maxAgeDays"), MaxAgeDays);
			StructuredContent->SetStringField(TEXT("nameContains"), NameContains);
			StructuredContent->SetNumberField(TEXT("candidateCount"), Candidates.Num());
			StructuredContent->SetNumberField(TEXT("skippedCount"), Skipped.Num());
			StructuredContent->SetNumberField(TEXT("deletedCount"), Deleted.Num());
			StructuredContent->SetNumberField(TEXT("deleteErrorCount"), DeleteErrors.Num());
			StructuredContent->SetArrayField(TEXT("candidates"), Candidates);
			StructuredContent->SetArrayField(TEXT("skipped"), Skipped);
			StructuredContent->SetArrayField(TEXT("deleted"), Deleted);
			StructuredContent->SetArrayField(TEXT("deleteErrors"), DeleteErrors);

			const FString Text = FString::Printf(
				TEXT("MCP cleanup %s: candidates=%d deleted=%d skipped=%d errors=%d."),
				bDryRun ? TEXT("dry run") : TEXT("applied"),
				Candidates.Num(),
				Deleted.Num(),
				Skipped.Num(),
				DeleteErrors.Num());
			return MakeExecutionResult(Text, StructuredContent, DeleteErrors.Num() > 0);
		}


}
