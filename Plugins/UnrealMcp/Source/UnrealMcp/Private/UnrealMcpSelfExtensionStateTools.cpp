#include "UnrealMcpSelfExtensionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FString HashTextForManifest(const FString& Text);
	FString GetMcpModuleSourcePath();
	FString GetMcpModuleHeaderPath();
	FString GetProjectReadmePath();
	FString GetPluginReadmePath();
	FString GetProjectMemoryFilePath();
	FString GetMcpExtensionBackupRoot();
	FString GetUnrealMcpSavedRoot();
	FString GetMcpBuildLogRoot();
	FString GetLatestMcpExtensionManifestPath();
	FString GetMcpExtensionLockPath();
	FString GetMcpProjectStateBackupRoot();
	FString SanitizeMcpToolIdForPath(const FString& ToolName);
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
	bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason);
	bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason);
	bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason);

		FUnrealMcpExecutionResult RollbackLastMcpExtension(const FJsonObject& Arguments)
		{
			FString ManifestPath = GetLatestMcpExtensionManifestPath();
			bool bDryRun = false;
			bool bForce = false;
			Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("force"), bForce);

			FString ResolvedManifestPath;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(ManifestPath, ResolvedManifestPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ManifestText;
			if (!FFileHelper::LoadFileToString(ManifestText, *ResolvedManifestPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read manifest '%s'."), *ResolvedManifestPath), nullptr, true);
			}

			TSharedPtr<FJsonObject> ManifestObject;
			if (!LoadJsonObject(ManifestText, ManifestObject) || !ManifestObject.IsValid())
			{
				return MakeExecutionResult(FString::Printf(TEXT("Manifest '%s' is not valid JSON."), *ResolvedManifestPath), nullptr, true);
			}

			const TArray<TSharedPtr<FJsonValue>>* ManifestFiles = nullptr;
			if (ManifestObject->TryGetArrayField(TEXT("files"), ManifestFiles) && ManifestFiles && ManifestFiles->Num() > 0)
			{
				FString ToolName;
				ManifestObject->TryGetStringField(TEXT("toolName"), ToolName);

				TArray<TSharedPtr<FJsonValue>> FileResults;
				bool bAllHashesMatch = true;
				FString FileFailureReason;
				for (const TSharedPtr<FJsonValue>& FileValue : *ManifestFiles)
				{
					TSharedPtr<FJsonObject> FileObject = FileValue.IsValid() ? FileValue->AsObject() : nullptr;
					if (!FileObject.IsValid())
					{
						continue;
					}

					FString SourcePath;
					FString BackupPath;
					FString ExpectedAfterHash;
					FileObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
					FileObject->TryGetStringField(TEXT("backupPath"), BackupPath);
					FileObject->TryGetStringField(TEXT("hashAfter"), ExpectedAfterHash);

					FString ResolvedSourcePath;
					FString ResolvedBackupPath;
					if (!ResolveProjectPathInsideProject(SourcePath, ResolvedSourcePath, FileFailureReason)
						|| !ResolveProjectPathInsideProject(BackupPath, ResolvedBackupPath, FileFailureReason))
					{
						return MakeExecutionResult(FileFailureReason, nullptr, true);
					}

					FString CurrentSourceText;
					FString BackupSourceText;
					if (!FFileHelper::LoadFileToString(CurrentSourceText, *ResolvedSourcePath))
					{
						return MakeExecutionResult(FString::Printf(TEXT("Failed to read current source '%s'."), *ResolvedSourcePath), nullptr, true);
					}
					if (!FFileHelper::LoadFileToString(BackupSourceText, *ResolvedBackupPath))
					{
						return MakeExecutionResult(FString::Printf(TEXT("Failed to read backup source '%s'."), *ResolvedBackupPath), nullptr, true);
					}

					const FString CurrentHash = HashTextForManifest(CurrentSourceText);
					const FString BackupHash = HashTextForManifest(BackupSourceText);
					const bool bHashMatches = ExpectedAfterHash.IsEmpty() || CurrentHash == ExpectedAfterHash;
					bAllHashesMatch &= bHashMatches;

					TSharedPtr<FJsonObject> FileResult = MakeShared<FJsonObject>();
					FileResult->SetStringField(TEXT("sourcePath"), ResolvedSourcePath);
					FileResult->SetStringField(TEXT("backupPath"), ResolvedBackupPath);
					FileResult->SetStringField(TEXT("currentHash"), CurrentHash);
					FileResult->SetStringField(TEXT("expectedAfterHash"), ExpectedAfterHash);
					FileResult->SetStringField(TEXT("backupHash"), BackupHash);
					FileResult->SetBoolField(TEXT("hashMatchesExpectedAfter"), bHashMatches);
					FileResults.Add(MakeShared<FJsonValueObject>(FileResult));
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_rollback_last_extension"));
				StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("manifestPath"), ResolvedManifestPath);
				StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
				StructuredContent->SetBoolField(TEXT("force"), bForce);
				StructuredContent->SetBoolField(TEXT("hashMatchesExpectedAfter"), bAllHashesMatch);
				StructuredContent->SetArrayField(TEXT("files"), FileResults);

				if (!bAllHashesMatch && !bForce)
				{
					return MakeExecutionResult(
						TEXT("Rollback refused because one or more current source hashes differ from the applied manifest. Pass force=true to override."),
						StructuredContent,
						true);
				}

				if (bDryRun)
				{
					return MakeExecutionResult(
						FString::Printf(TEXT("Dry run rollback for %s would restore %d files."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName, FileResults.Num()),
						StructuredContent,
						false);
				}

				for (const TSharedPtr<FJsonValue>& FileValue : FileResults)
				{
					TSharedPtr<FJsonObject> FileObject = FileValue.IsValid() ? FileValue->AsObject() : nullptr;
					if (!FileObject.IsValid())
					{
						continue;
					}

					FString SourcePath;
					FString BackupPath;
					FileObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
					FileObject->TryGetStringField(TEXT("backupPath"), BackupPath);
					FString BackupSourceText;
					if (!FFileHelper::LoadFileToString(BackupSourceText, *BackupPath)
						|| !FFileHelper::SaveStringToFile(BackupSourceText, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						return MakeExecutionResult(FString::Printf(TEXT("Failed to restore source file '%s'."), *SourcePath), StructuredContent, true);
					}
				}

				ManifestObject->SetStringField(TEXT("rolledBackAtUtc"), FDateTime::UtcNow().ToIso8601());
				ManifestObject->SetBoolField(TEXT("rolledBack"), true);
				FString ManifestFailure;
				SaveJsonObjectToFile(ManifestObject, ResolvedManifestPath, ManifestFailure);
				SaveJsonObjectToFile(ManifestObject, GetLatestMcpExtensionManifestPath(), ManifestFailure);

				StructuredContent->SetBoolField(TEXT("rolledBack"), true);
				return MakeExecutionResult(
					FString::Printf(TEXT("Rolled back descriptor-first MCP extension for %s."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName),
					StructuredContent,
					false);
			}

			FString ToolName;
			FString SourcePath;
			FString BackupSourcePath;
			FString ExpectedAfterHash;
			ManifestObject->TryGetStringField(TEXT("toolName"), ToolName);
			ManifestObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
			ManifestObject->TryGetStringField(TEXT("backupSourcePath"), BackupSourcePath);
			ManifestObject->TryGetStringField(TEXT("sourceHashAfter"), ExpectedAfterHash);

			if (SourcePath.IsEmpty() || BackupSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("Manifest is missing sourcePath or backupSourcePath."), nullptr, true);
			}

			FString ResolvedSourcePath;
			FString ResolvedBackupPath;
			if (!ResolveProjectPathInsideProject(SourcePath, ResolvedSourcePath, FailureReason)
				|| !ResolveProjectPathInsideProject(BackupSourcePath, ResolvedBackupPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString CurrentSourceText;
			FString BackupSourceText;
			if (!FFileHelper::LoadFileToString(CurrentSourceText, *ResolvedSourcePath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read current source '%s'."), *ResolvedSourcePath), nullptr, true);
			}
			if (!FFileHelper::LoadFileToString(BackupSourceText, *ResolvedBackupPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read backup source '%s'."), *ResolvedBackupPath), nullptr, true);
			}

			const FString CurrentHash = HashTextForManifest(CurrentSourceText);
			const FString BackupHash = HashTextForManifest(BackupSourceText);
			const bool bHashMatches = ExpectedAfterHash.IsEmpty() || CurrentHash == ExpectedAfterHash;

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_rollback_last_extension"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("manifestPath"), ResolvedManifestPath);
			StructuredContent->SetStringField(TEXT("sourcePath"), ResolvedSourcePath);
			StructuredContent->SetStringField(TEXT("backupSourcePath"), ResolvedBackupPath);
			StructuredContent->SetStringField(TEXT("currentHash"), CurrentHash);
			StructuredContent->SetStringField(TEXT("expectedAfterHash"), ExpectedAfterHash);
			StructuredContent->SetStringField(TEXT("backupHash"), BackupHash);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("force"), bForce);
			StructuredContent->SetBoolField(TEXT("hashMatchesExpectedAfter"), bHashMatches);

			if (!bHashMatches && !bForce)
			{
				return MakeExecutionResult(
					TEXT("Rollback refused because current source hash differs from the applied manifest. Pass force=true to override."),
					StructuredContent,
					true);
			}

			if (bDryRun)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Dry run rollback for %s would restore %s."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName, *ResolvedSourcePath),
					StructuredContent,
					false);
			}

			if (!FFileHelper::SaveStringToFile(BackupSourceText, *ResolvedSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to restore source file '%s'."), *ResolvedSourcePath), StructuredContent, true);
			}

			ManifestObject->SetStringField(TEXT("rolledBackAtUtc"), FDateTime::UtcNow().ToIso8601());
			ManifestObject->SetBoolField(TEXT("rolledBack"), true);
			FString ManifestFailure;
			SaveJsonObjectToFile(ManifestObject, ResolvedManifestPath, ManifestFailure);
			SaveJsonObjectToFile(ManifestObject, GetLatestMcpExtensionManifestPath(), ManifestFailure);

			StructuredContent->SetBoolField(TEXT("rolledBack"), true);
			StructuredContent->SetStringField(TEXT("restoredHash"), BackupHash);
			return MakeExecutionResult(
				FString::Printf(TEXT("Rolled back MCP extension for %s."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName),
				StructuredContent,
				false);
		}

		FString NormalizeFullPathForCompare(const FString& Path)
		{
			FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(NormalizedPath);
			FPaths::CollapseRelativeDirectories(NormalizedPath);
			return NormalizedPath;
		}

		bool IsPathInsideDirectory(const FString& Path, const FString& Directory)
		{
			const FString NormalizedPath = NormalizeFullPathForCompare(Path);
			FString NormalizedDirectory = NormalizeFullPathForCompare(Directory);
			NormalizedDirectory.RemoveFromEnd(TEXT("/"));
			const FString DirectoryPrefix = NormalizedDirectory + TEXT("/");
			return NormalizedPath.Equals(NormalizedDirectory, ESearchCase::IgnoreCase)
				|| NormalizedPath.StartsWith(DirectoryPrefix, ESearchCase::IgnoreCase);
		}

		FString FileTimeToIsoString(const FDateTime& Time)
		{
			return Time.GetTicks() > 0 ? Time.ToIso8601() : FString();
		}

		TSharedPtr<FJsonObject> MakeFileInfoObject(const FString& Path)
		{
			TSharedPtr<FJsonObject> InfoObject = MakeShared<FJsonObject>();
			InfoObject->SetStringField(TEXT("path"), Path);
			InfoObject->SetBoolField(TEXT("exists"), FPaths::FileExists(Path) || FPaths::DirectoryExists(Path));

			const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
			if (Stat.bIsValid)
			{
				InfoObject->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Stat.FileSize));
				InfoObject->SetStringField(TEXT("modifiedAt"), FileTimeToIsoString(Stat.ModificationTime));
			}
			return InfoObject;
		}

		void FindImmediateChildren(const FString& Directory, const FString& Pattern, bool bFiles, bool bDirectories, TArray<FString>& OutChildren)
		{
			TArray<FString> Names;
			IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, Pattern), bFiles, bDirectories);
			for (const FString& Name : Names)
			{
				OutChildren.Add(FPaths::Combine(Directory, Name));
			}
			OutChildren.Sort();
		}

		bool FindNewestFile(const FString& Directory, const FString& Pattern, FString& OutPath)
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Directory, *Pattern, true, false);
			if (Files.Num() == 0)
			{
				return false;
			}

			Files.Sort([](const FString& A, const FString& B)
			{
				const FFileStatData StatA = IFileManager::Get().GetStatData(*A);
				const FFileStatData StatB = IFileManager::Get().GetStatData(*B);
				return StatA.ModificationTime > StatB.ModificationTime;
			});

			OutPath = Files[0];
			return true;
		}

		FString MakePathRelativeToProject(const FString& Path)
		{
			FString RelativePath = FPaths::ConvertRelativePathToFull(Path);
			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeFilename(RelativePath);
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::MakePathRelativeTo(RelativePath, *ProjectDir);
			return RelativePath;
		}

		bool ParseIsoUtc(const FString& IsoText, FDateTime& OutDateTime)
		{
			if (IsoText.TrimStartAndEnd().IsEmpty())
			{
				return false;
			}
			return FDateTime::ParseIso8601(*IsoText, OutDateTime);
		}

		bool IsExtensionLockStale(const TSharedPtr<FJsonObject>& LockObject)
		{
			if (!LockObject.IsValid())
			{
				return true;
			}

			FString ExpiresAtUtc;
			if (!LockObject->TryGetStringField(TEXT("expiresAtUtc"), ExpiresAtUtc))
			{
				return true;
			}

			FDateTime ExpiresAt;
			if (!ParseIsoUtc(ExpiresAtUtc, ExpiresAt))
			{
				return true;
			}

			return FDateTime::UtcNow() >= ExpiresAt;
		}

		TSharedPtr<FJsonObject> MakeExtensionLockObject(
			const FString& SessionId,
			const FString& Owner,
			const FString& Reason,
			int32 TtlSeconds)
		{
			const FDateTime Now = FDateTime::UtcNow();
			const int32 SafeTtlSeconds = FMath::Clamp(TtlSeconds, 30, 86400);

			TSharedPtr<FJsonObject> LockObject = MakeShared<FJsonObject>();
			LockObject->SetStringField(TEXT("sessionId"), SessionId);
			LockObject->SetStringField(TEXT("owner"), Owner.TrimStartAndEnd().IsEmpty() ? TEXT("Unreal MCP Chat") : Owner.TrimStartAndEnd());
			LockObject->SetStringField(TEXT("reason"), Reason.TrimStartAndEnd());
			LockObject->SetStringField(TEXT("createdAtUtc"), Now.ToIso8601());
			LockObject->SetStringField(TEXT("refreshedAtUtc"), Now.ToIso8601());
			LockObject->SetStringField(TEXT("expiresAtUtc"), (Now + FTimespan::FromSeconds(SafeTtlSeconds)).ToIso8601());
			LockObject->SetNumberField(TEXT("ttlSeconds"), SafeTtlSeconds);
			return LockObject;
		}

		bool TryAcquireExtensionSessionLock(
			const FString& Owner,
			const FString& Reason,
			int32 TtlSeconds,
			bool bForce,
			FString& OutSessionId,
			TSharedPtr<FJsonObject>& OutLockObject,
			FString& OutFailureReason)
		{
			OutSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
			const FString LockPath = GetMcpExtensionLockPath();

			TSharedPtr<FJsonObject> ExistingLock;
			if (FPaths::FileExists(LockPath))
			{
				FString LoadFailure;
				if (LoadJsonObjectFromFile(LockPath, ExistingLock, LoadFailure) && ExistingLock.IsValid() && !IsExtensionLockStale(ExistingLock) && !bForce)
				{
					FString ExistingSessionId;
					FString ExistingOwner;
					FString ExpiresAtUtc;
					ExistingLock->TryGetStringField(TEXT("sessionId"), ExistingSessionId);
					ExistingLock->TryGetStringField(TEXT("owner"), ExistingOwner);
					ExistingLock->TryGetStringField(TEXT("expiresAtUtc"), ExpiresAtUtc);
					OutFailureReason = FString::Printf(
						TEXT("MCP extension session is locked by '%s' (sessionId=%s, expiresAtUtc=%s). Pass force=true only if you are sure that session is dead."),
						ExistingOwner.IsEmpty() ? TEXT("<unknown>") : *ExistingOwner,
						ExistingSessionId.IsEmpty() ? TEXT("<unknown>") : *ExistingSessionId,
						ExpiresAtUtc.IsEmpty() ? TEXT("<unknown>") : *ExpiresAtUtc);
					OutLockObject = ExistingLock;
					return false;
				}
			}

			OutLockObject = MakeExtensionLockObject(OutSessionId, Owner, Reason, TtlSeconds);
			FString SaveFailure;
			if (!SaveJsonObjectToFile(OutLockObject, LockPath, SaveFailure))
			{
				OutFailureReason = SaveFailure;
				return false;
			}
			OutLockObject->SetStringField(TEXT("lockPath"), LockPath);
			return true;
		}

		bool ReleaseExtensionSessionLock(const FString& SessionId, bool bForce, FString& OutFailureReason)
		{
			const FString LockPath = GetMcpExtensionLockPath();
			if (!FPaths::FileExists(LockPath))
			{
				return true;
			}

			TSharedPtr<FJsonObject> ExistingLock;
			if (!LoadJsonObjectFromFile(LockPath, ExistingLock, OutFailureReason))
			{
				if (bForce)
				{
					IFileManager::Get().Delete(*LockPath, false, true, true);
					return true;
				}
				return false;
			}

			FString ExistingSessionId;
			ExistingLock->TryGetStringField(TEXT("sessionId"), ExistingSessionId);
			if (!bForce && !ExistingSessionId.Equals(SessionId, ESearchCase::CaseSensitive))
			{
				OutFailureReason = FString::Printf(
					TEXT("Refusing to release lock owned by a different session. Existing=%s requested=%s."),
					*ExistingSessionId,
					*SessionId);
				return false;
			}

			if (!IFileManager::Get().Delete(*LockPath, false, true, true))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to delete lock file '%s'."), *LockPath);
				return false;
			}
			return true;
		}

		FString TailLines(const FString& Text, int32 MaxLines)
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			const int32 StartIndex = FMath::Max(0, Lines.Num() - FMath::Max(1, MaxLines));
			TArray<FString> Tail;
			for (int32 Index = StartIndex; Index < Lines.Num(); ++Index)
			{
				Tail.Add(Lines[Index]);
			}
			return FString::Join(Tail, TEXT("\n"));
		}

		void CollectExtensionManifestPaths(TArray<FString>& OutManifestPaths)
		{
			OutManifestPaths.Reset();
			IFileManager::Get().FindFilesRecursive(
				OutManifestPaths,
				*GetMcpExtensionBackupRoot(),
				TEXT("Manifest.json"),
				true,
				false);

			const FString LatestManifestPath = GetLatestMcpExtensionManifestPath();
			if (FPaths::FileExists(LatestManifestPath))
			{
				OutManifestPaths.AddUnique(LatestManifestPath);
			}

			OutManifestPaths.Sort([](const FString& A, const FString& B)
			{
				const FFileStatData StatA = IFileManager::Get().GetStatData(*A);
				const FFileStatData StatB = IFileManager::Get().GetStatData(*B);
				if (StatA.ModificationTime == StatB.ModificationTime)
				{
					return A > B;
				}
				return StatA.ModificationTime > StatB.ModificationTime;
			});
		}

		bool AddProjectStateBackupFile(
			const FString& SourcePath,
			const FString& BackupDirectory,
			bool bDryRun,
			TArray<TSharedPtr<FJsonValue>>& Files,
			FString& OutFailureReason)
		{
			const FString FullSourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
			const bool bExists = FPaths::FileExists(FullSourcePath);
			const FString RelativePath = MakePathRelativeToProject(FullSourcePath);
			const FString BackupPath = FPaths::Combine(BackupDirectory, RelativePath);

			TSharedPtr<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("sourcePath"), FullSourcePath);
			FileObject->SetStringField(TEXT("relativePath"), RelativePath);
			FileObject->SetStringField(TEXT("backupPath"), BackupPath);
			FileObject->SetBoolField(TEXT("exists"), bExists);

			if (bExists)
			{
				FString SourceText;
				if (!FFileHelper::LoadFileToString(SourceText, *FullSourcePath))
				{
					OutFailureReason = FString::Printf(TEXT("Failed to read source file '%s'."), *FullSourcePath);
					return false;
				}

				FileObject->SetStringField(TEXT("hash"), HashTextForManifest(SourceText));
				FileObject->SetNumberField(TEXT("sizeBytes"), SourceText.Len());
				if (!bDryRun)
				{
					const FString BackupPathDirectory = FPaths::GetPath(BackupPath);
					if (!IFileManager::Get().MakeDirectory(*BackupPathDirectory, true))
					{
						OutFailureReason = FString::Printf(TEXT("Failed to create backup directory '%s'."), *BackupPathDirectory);
						return false;
					}
					if (!FFileHelper::SaveStringToFile(SourceText, *BackupPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						OutFailureReason = FString::Printf(TEXT("Failed to write backup file '%s'."), *BackupPath);
						return false;
					}
				}
			}

			Files.Add(MakeShared<FJsonValueObject>(FileObject));
			return true;
		}

		FUnrealMcpExecutionResult BackupProjectState(const FJsonObject& Arguments)
		{
			FString Label = TEXT("manual");
			FString Reason;
			bool bIncludeSource = true;
			bool bIncludeReadmes = true;
			bool bIncludeProjectMemory = true;
			bool bIncludeManifests = true;
			bool bIncludeBuildLogs = false;
			bool bDryRun = false;

			Arguments.TryGetStringField(TEXT("label"), Label);
			Arguments.TryGetStringField(TEXT("reason"), Reason);
			Arguments.TryGetBoolField(TEXT("includeSource"), bIncludeSource);
			Arguments.TryGetBoolField(TEXT("includeReadmes"), bIncludeReadmes);
			Arguments.TryGetBoolField(TEXT("includeProjectMemory"), bIncludeProjectMemory);
			Arguments.TryGetBoolField(TEXT("includeManifests"), bIncludeManifests);
			Arguments.TryGetBoolField(TEXT("includeBuildLogs"), bIncludeBuildLogs);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			const FString SafeLabel = SanitizeMcpToolIdForPath(Label.TrimStartAndEnd().IsEmpty() ? TEXT("manual") : Label.TrimStartAndEnd());
			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BackupDirectory = FPaths::Combine(GetMcpProjectStateBackupRoot(), FString::Printf(TEXT("%s_%s"), *Timestamp, *SafeLabel));

			TArray<FString> SourcePaths;
			if (bIncludeSource)
			{
				SourcePaths.Add(GetMcpModuleSourcePath());
				SourcePaths.Add(GetMcpModuleHeaderPath());
			}
			if (bIncludeReadmes)
			{
				SourcePaths.Add(GetProjectReadmePath());
				SourcePaths.Add(GetPluginReadmePath());
			}
			if (bIncludeProjectMemory)
			{
				SourcePaths.Add(GetProjectMemoryFilePath());
			}
			if (bIncludeManifests)
			{
				TArray<FString> ManifestPaths;
				CollectExtensionManifestPaths(ManifestPaths);
				for (const FString& ManifestPath : ManifestPaths)
				{
					SourcePaths.AddUnique(ManifestPath);
				}
			}
			if (bIncludeBuildLogs)
			{
				TArray<FString> BuildLogPaths;
				IFileManager::Get().FindFilesRecursive(BuildLogPaths, *GetMcpBuildLogRoot(), TEXT("*.log"), true, false);
				BuildLogPaths.Sort([](const FString& A, const FString& B)
				{
					return IFileManager::Get().GetStatData(*A).ModificationTime > IFileManager::Get().GetStatData(*B).ModificationTime;
				});
				const int32 MaxBuildLogs = FMath::Min(5, BuildLogPaths.Num());
				for (int32 Index = 0; Index < MaxBuildLogs; ++Index)
				{
					SourcePaths.AddUnique(BuildLogPaths[Index]);
				}
			}

			TArray<TSharedPtr<FJsonValue>> Files;
			FString FailureReason;
			for (const FString& SourcePath : SourcePaths)
			{
				if (!AddProjectStateBackupFile(SourcePath, BackupDirectory, bDryRun, Files, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_backup_project_state"));
			StructuredContent->SetStringField(TEXT("label"), Label);
			StructuredContent->SetStringField(TEXT("reason"), Reason);
			StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetArrayField(TEXT("files"), Files);

			if (!bDryRun)
			{
				TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
				ManifestObject->SetStringField(TEXT("action"), TEXT("mcp_backup_project_state"));
				ManifestObject->SetStringField(TEXT("label"), Label);
				ManifestObject->SetStringField(TEXT("reason"), Reason);
				ManifestObject->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
				ManifestObject->SetStringField(TEXT("backupDirectory"), BackupDirectory);
				ManifestObject->SetArrayField(TEXT("files"), Files);
				FString ManifestFailure;
				const FString ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure))
				{
					return MakeExecutionResult(ManifestFailure, StructuredContent, true);
				}
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
			}

			return MakeExecutionResult(
				bDryRun
					? FString::Printf(TEXT("Dry run project state backup would capture %d files."), Files.Num())
					: FString::Printf(TEXT("Backed up project state to %s."), *BackupDirectory),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult LockExtensionSession(const FJsonObject& Arguments)
		{
			FString Mode = TEXT("status");
			FString SessionId;
			FString Owner = TEXT("Unreal MCP Chat");
			FString Reason;
			bool bForce = false;
			double TtlSecondsDouble = 900.0;

			Arguments.TryGetStringField(TEXT("mode"), Mode);
			Arguments.TryGetStringField(TEXT("sessionId"), SessionId);
			Arguments.TryGetStringField(TEXT("owner"), Owner);
			Arguments.TryGetStringField(TEXT("reason"), Reason);
			Arguments.TryGetBoolField(TEXT("force"), bForce);
			Arguments.TryGetNumberField(TEXT("ttlSeconds"), TtlSecondsDouble);
			Mode = Mode.TrimStartAndEnd().ToLower();

			const FString LockPath = GetMcpExtensionLockPath();
			TSharedPtr<FJsonObject> ExistingLock;
			FString FailureReason;
			const bool bHasExistingLock = FPaths::FileExists(LockPath) && LoadJsonObjectFromFile(LockPath, ExistingLock, FailureReason);
			const bool bExistingLockStale = !bHasExistingLock || IsExtensionLockStale(ExistingLock);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_lock_extension_session"));
			StructuredContent->SetStringField(TEXT("mode"), Mode);
			StructuredContent->SetStringField(TEXT("lockPath"), LockPath);
			StructuredContent->SetBoolField(TEXT("hasExistingLock"), bHasExistingLock);
			StructuredContent->SetBoolField(TEXT("existingLockStale"), bExistingLockStale);
			if (ExistingLock.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("existingLock"), ExistingLock);
			}

			if (Mode == TEXT("status") || Mode.IsEmpty())
			{
				StructuredContent->SetBoolField(TEXT("locked"), bHasExistingLock && !bExistingLockStale);
				return MakeExecutionResult(
					bHasExistingLock && !bExistingLockStale ? TEXT("MCP extension session is locked.") : TEXT("No active MCP extension session lock."),
					StructuredContent,
					false);
			}

			if (Mode == TEXT("acquire"))
			{
				FString NewSessionId;
				TSharedPtr<FJsonObject> NewLock;
				if (!TryAcquireExtensionSessionLock(Owner, Reason, static_cast<int32>(TtlSecondsDouble), bForce, NewSessionId, NewLock, FailureReason))
				{
					StructuredContent->SetStringField(TEXT("failureReason"), FailureReason);
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
				StructuredContent->SetBoolField(TEXT("locked"), true);
				StructuredContent->SetStringField(TEXT("sessionId"), NewSessionId);
				StructuredContent->SetObjectField(TEXT("lock"), NewLock);
				return MakeExecutionResult(FString::Printf(TEXT("Acquired MCP extension session lock %s."), *NewSessionId), StructuredContent, false);
			}

			if (Mode == TEXT("release"))
			{
				if (SessionId.TrimStartAndEnd().IsEmpty() && !bForce)
				{
					return MakeExecutionResult(TEXT("sessionId is required for release unless force=true."), StructuredContent, true);
				}
				if (!ReleaseExtensionSessionLock(SessionId.TrimStartAndEnd(), bForce, FailureReason))
				{
					StructuredContent->SetStringField(TEXT("failureReason"), FailureReason);
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
				StructuredContent->SetBoolField(TEXT("locked"), false);
				return MakeExecutionResult(TEXT("Released MCP extension session lock."), StructuredContent, false);
			}

			if (Mode == TEXT("refresh"))
			{
				if (!bHasExistingLock)
				{
					return MakeExecutionResult(TEXT("No lock exists to refresh."), StructuredContent, true);
				}

				FString ExistingSessionId;
				ExistingLock->TryGetStringField(TEXT("sessionId"), ExistingSessionId);
				if (!bForce && !ExistingSessionId.Equals(SessionId.TrimStartAndEnd(), ESearchCase::CaseSensitive))
				{
					return MakeExecutionResult(TEXT("Refusing to refresh lock owned by a different session."), StructuredContent, true);
				}

				TSharedPtr<FJsonObject> RefreshedLock = MakeExtensionLockObject(ExistingSessionId, Owner, Reason, static_cast<int32>(TtlSecondsDouble));
				FString SaveFailure;
				if (!SaveJsonObjectToFile(RefreshedLock, LockPath, SaveFailure))
				{
					return MakeExecutionResult(SaveFailure, StructuredContent, true);
				}
				StructuredContent->SetBoolField(TEXT("locked"), true);
				StructuredContent->SetObjectField(TEXT("lock"), RefreshedLock);
				return MakeExecutionResult(TEXT("Refreshed MCP extension session lock."), StructuredContent, false);
			}

			return MakeExecutionResult(TEXT("mode must be one of status, acquire, release, or refresh."), StructuredContent, true);
		}

		FUnrealMcpExecutionResult RollbackToManifest(const FJsonObject& Arguments)
		{
			FString ManifestPath;
			FString ToolName;
			FString Selector = TEXT("latest");
			bool bDryRun = false;
			bool bForce = false;
			bool bCreatePreRollbackBackup = true;
			double ManifestIndexDouble = -1.0;

			Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("selector"), Selector);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("force"), bForce);
			Arguments.TryGetBoolField(TEXT("createPreRollbackBackup"), bCreatePreRollbackBackup);
			Arguments.TryGetNumberField(TEXT("manifestIndex"), ManifestIndexDouble);

			TArray<FString> ManifestPaths;
			CollectExtensionManifestPaths(ManifestPaths);

			TArray<FString> CandidatePaths;
			if (!ManifestPath.TrimStartAndEnd().IsEmpty())
			{
				FString ResolvedManifestPath;
				FString FailureReason;
				if (!ResolveProjectPathInsideProject(ManifestPath, ResolvedManifestPath, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				CandidatePaths.Add(ResolvedManifestPath);
			}
			else
			{
				for (const FString& CandidatePath : ManifestPaths)
				{
					if (ToolName.TrimStartAndEnd().IsEmpty())
					{
						CandidatePaths.Add(CandidatePath);
						continue;
					}

					TSharedPtr<FJsonObject> CandidateObject;
					FString FailureReason;
					if (LoadJsonObjectFromFile(CandidatePath, CandidateObject, FailureReason) && CandidateObject.IsValid())
					{
						FString CandidateToolName;
						CandidateObject->TryGetStringField(TEXT("toolName"), CandidateToolName);
						if (CandidateToolName.Equals(ToolName.TrimStartAndEnd(), ESearchCase::IgnoreCase))
						{
							CandidatePaths.Add(CandidatePath);
						}
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> CandidateObjects;
			for (const FString& CandidatePath : CandidatePaths)
			{
				TSharedPtr<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
				CandidateObject->SetStringField(TEXT("manifestPath"), CandidatePath);
				CandidateObject->SetObjectField(TEXT("file"), MakeFileInfoObject(CandidatePath));
				CandidateObjects.Add(MakeShared<FJsonValueObject>(CandidateObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_rollback_to_manifest"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("selector"), Selector);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("force"), bForce);
			StructuredContent->SetArrayField(TEXT("candidates"), CandidateObjects);

			if (CandidatePaths.Num() == 0)
			{
				return MakeExecutionResult(TEXT("No matching extension manifest was found."), StructuredContent, true);
			}

			int32 SelectedIndex = FMath::RoundToInt(ManifestIndexDouble);
			if (SelectedIndex < 0)
			{
				SelectedIndex = Selector.TrimStartAndEnd().Equals(TEXT("oldest"), ESearchCase::IgnoreCase) ? CandidatePaths.Num() - 1 : 0;
			}
			if (!CandidatePaths.IsValidIndex(SelectedIndex))
			{
				return MakeExecutionResult(TEXT("manifestIndex is outside the available candidate range."), StructuredContent, true);
			}

			const FString SelectedManifestPath = CandidatePaths[SelectedIndex];
			StructuredContent->SetNumberField(TEXT("selectedIndex"), SelectedIndex);
			StructuredContent->SetStringField(TEXT("selectedManifestPath"), SelectedManifestPath);

			if (!bDryRun && bCreatePreRollbackBackup)
			{
				TSharedPtr<FJsonObject> BackupArguments = MakeShared<FJsonObject>();
				BackupArguments->SetStringField(TEXT("label"), TEXT("pre_rollback"));
				BackupArguments->SetStringField(TEXT("reason"), FString::Printf(TEXT("Pre-rollback snapshot before restoring manifest %s."), *SelectedManifestPath));
				BackupArguments->SetBoolField(TEXT("includeBuildLogs"), false);
				const FUnrealMcpExecutionResult BackupResult = BackupProjectState(*BackupArguments);
				StructuredContent->SetObjectField(TEXT("preRollbackBackup"), BackupResult.StructuredContent.IsValid() ? BackupResult.StructuredContent : MakeShared<FJsonObject>());
				if (BackupResult.bIsError)
				{
					return MakeExecutionResult(BackupResult.Text, StructuredContent, true);
				}
			}

			TSharedPtr<FJsonObject> RollbackArguments = MakeShared<FJsonObject>();
			RollbackArguments->SetStringField(TEXT("manifestPath"), SelectedManifestPath);
			RollbackArguments->SetBoolField(TEXT("dryRun"), bDryRun);
			RollbackArguments->SetBoolField(TEXT("force"), bForce);
			const FUnrealMcpExecutionResult RollbackResult = RollbackLastMcpExtension(*RollbackArguments);
			if (RollbackResult.StructuredContent.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("rollback"), RollbackResult.StructuredContent);
			}
			return MakeExecutionResult(RollbackResult.Text, StructuredContent, RollbackResult.bIsError);
		}

		TSharedPtr<FJsonObject> MakeMemoryEntrySummary(const TSharedPtr<FJsonObject>& EntryObject, bool bIncludeContent)
		{
			TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
			if (!EntryObject.IsValid())
			{
				return SummaryObject;
			}

			FString Key;
			FString Summary;
			FString Status;
			FString NextStep;
			FString UpdatedAtUtc;
			EntryObject->TryGetStringField(TEXT("key"), Key);
			EntryObject->TryGetStringField(TEXT("summary"), Summary);
			EntryObject->TryGetStringField(TEXT("status"), Status);
			EntryObject->TryGetStringField(TEXT("nextStep"), NextStep);
			EntryObject->TryGetStringField(TEXT("updatedAtUtc"), UpdatedAtUtc);
			SummaryObject->SetStringField(TEXT("key"), Key);
			SummaryObject->SetStringField(TEXT("summary"), Summary);
			SummaryObject->SetStringField(TEXT("status"), Status);
			SummaryObject->SetStringField(TEXT("nextStep"), NextStep);
			SummaryObject->SetStringField(TEXT("updatedAtUtc"), UpdatedAtUtc);

			const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
			if (EntryObject->TryGetArrayField(TEXT("tags"), Tags) && Tags)
			{
				SummaryObject->SetArrayField(TEXT("tags"), *Tags);
			}

			const TSharedPtr<FJsonObject>* Content = nullptr;
			if (bIncludeContent && EntryObject->TryGetObjectField(TEXT("content"), Content) && Content && (*Content).IsValid())
			{
				SummaryObject->SetObjectField(TEXT("content"), *Content);
			}
			return SummaryObject;
		}

		TSharedPtr<FJsonObject> FindMemoryEntryByKey(const TSharedPtr<FJsonObject>& MemoryObject, const FString& Key)
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!MemoryObject.IsValid() || !MemoryObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
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
				if (EntryValue->AsObject()->TryGetStringField(TEXT("key"), ExistingKey) && ExistingKey == Key)
				{
					return EntryValue->AsObject();
				}
			}
			return nullptr;
		}

		FString RecommendPipelineNextStep(const TSharedPtr<FJsonObject>& MemoryEntry)
		{
			if (!MemoryEntry.IsValid())
			{
				return TEXT("No matching project memory entry was found. Run unreal.mcp_extension_pipeline or write memory for the active extension.");
			}

			FString Status;
			MemoryEntry->TryGetStringField(TEXT("status"), Status);
			const FString LowerStatus = Status.ToLower();
			if (LowerStatus.Contains(TEXT("restart")))
			{
				return TEXT("Restart Unreal Editor, then run unreal.mcp_extension_pipeline with mode=resume_test or unreal.mcp_run_tool_test.");
			}
			if (LowerStatus.Contains(TEXT("build_failed")))
			{
				return TEXT("Open the latest build log, fix compile errors, then rerun unreal.mcp_build_editor.");
			}
			if (LowerStatus.Contains(TEXT("tool_test_succeeded")))
			{
				return TEXT("Run unreal.mcp_tool_audit, then optionally run unreal.mcp_clean_test_artifacts in dryRun mode.");
			}
			if (LowerStatus.Contains(TEXT("pipeline_apply_complete")))
			{
				return TEXT("Run unreal.mcp_build_editor, restart if needed, then resume the tool test.");
			}
			return TEXT("Continue with the next pipeline step shown in project memory.");
		}

}
