#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpActivityLog.h"
#include "UnrealMcpSharedPathResolver.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistrar.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	bool LoadScaffoldSnippet(
		const FString& ScaffoldDirectory,
		const FString& FileName,
		bool bRequired,
		FString& OutSnippet,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutFailureReason);
	TSharedPtr<FJsonObject> MakeTextDiffObject(const FString& BeforeText, const FString& AfterText, int32 MaxPreviewLines);

		enum class EMcpScaffoldInsertionStatus
		{
			WillInsert,
			Inserted,
			SkippedAlreadyIntegrated,
			SkippedOptionalMissing,
			Conflict,
			MissingAnchor
		};

		const TCHAR* LexToString(EMcpScaffoldInsertionStatus Status)
		{
			switch (Status)
			{
			case EMcpScaffoldInsertionStatus::WillInsert:
				return TEXT("will_insert");
			case EMcpScaffoldInsertionStatus::Inserted:
				return TEXT("inserted");
			case EMcpScaffoldInsertionStatus::SkippedAlreadyIntegrated:
				return TEXT("skipped_already_integrated");
			case EMcpScaffoldInsertionStatus::SkippedOptionalMissing:
				return TEXT("skipped_optional_missing");
			case EMcpScaffoldInsertionStatus::Conflict:
				return TEXT("conflict");
			case EMcpScaffoldInsertionStatus::MissingAnchor:
				return TEXT("missing_anchor");
			default:
				return TEXT("unknown");
			}
		}

		static constexpr int32 GUnrealMcpExtensionManifestSchemaVersion = 2;

		const TCHAR* GetUnrealMcpExtensionManifestSchemaName()
		{
			return TEXT("UnrealMcpExtensionManifest.v2");
		}

		void WriteManifestActivityEvent(
			const FString& EventKind,
			const FString& ToolName,
			const FString& ManifestSessionId,
			const FString& ManifestPath,
			const FString& BackupDirectory,
			bool bIncludeBackupDirectory,
			int32 ChangesCount,
			int32 FilesCount,
			bool bPostcheckOk)
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			Payload->SetStringField(TEXT("toolName"), ToolName);
			Payload->SetStringField(TEXT("manifestPath"), ManifestPath);
			if (bIncludeBackupDirectory)
			{
				Payload->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			}
			Payload->SetNumberField(TEXT("changesCount"), ChangesCount);
			Payload->SetNumberField(TEXT("filesCount"), FilesCount);
			Payload->SetBoolField(TEXT("postcheckOk"), bPostcheckOk);

			TSharedPtr<FJsonObject> Correlation = MakeShared<FJsonObject>();
			Correlation->SetStringField(TEXT("manifestId"), ManifestSessionId);

			UnrealMcp::FActivityLogEvent Event;
			Event.EventKind = EventKind;
			Event.Summary = FString::Printf(TEXT("%s for %s: %s."), *EventKind, *ToolName, bPostcheckOk ? TEXT("completed") : TEXT("failed")).Left(2000);
			Event.Payload = Payload;
			Event.Correlation = Correlation;
			Event.LegacyEventType = FString();
			UnrealMcp::WriteActivityEvent(Event);
		}

		TSharedPtr<FJsonObject> MakeInsertionChangeObject(
			const FString& Section,
			EMcpScaffoldInsertionStatus Status,
			const FString& Message,
			int32 Offset,
			const FString& Preview)
		{
			TSharedPtr<FJsonObject> ChangeObject = MakeShared<FJsonObject>();
			ChangeObject->SetStringField(TEXT("section"), Section);
			ChangeObject->SetStringField(TEXT("status"), LexToString(Status));
			ChangeObject->SetStringField(TEXT("message"), Message);
			ChangeObject->SetNumberField(TEXT("offset"), Offset);
			ChangeObject->SetStringField(TEXT("preview"), Preview);
			return ChangeObject;
		}

		int32 CountScaffoldChangesByStatus(const TArray<TSharedPtr<FJsonValue>>& Changes, const FString& Status)
		{
			int32 Count = 0;
			for (const TSharedPtr<FJsonValue>& ChangeValue : Changes)
			{
				TSharedPtr<FJsonObject> ChangeObject;
				if (ChangeValue.IsValid())
				{
					ChangeObject = ChangeValue->AsObject();
				}
				if (!ChangeObject.IsValid())
				{
					continue;
				}

				FString ChangeStatus;
				if (ChangeObject->TryGetStringField(TEXT("status"), ChangeStatus) && ChangeStatus == Status)
				{
					++Count;
				}
			}
			return Count;
		}

		TSharedPtr<FJsonObject> MakeScaffoldConflictPolicyObject()
		{
			TSharedPtr<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
			PolicyObject->SetBoolField(TEXT("exactPatchIsIdempotent"), true);
			PolicyObject->SetBoolField(TEXT("conflictNeedleBlocksApply"), true);
			PolicyObject->SetBoolField(TEXT("missingAnchorBlocksApply"), true);
			PolicyObject->SetBoolField(TEXT("unsafePatchBlocksApplyByDefault"), true);
			PolicyObject->SetStringField(TEXT("conflictDetector"), TEXT("PlanOrApplyPatchInsertion"));
			return PolicyObject;
		}

		void AddScaffoldNextStep(
			TArray<TSharedPtr<FJsonValue>>& NextSteps,
			const FString& Step,
			const FString& Tool,
			const FString& Reason)
		{
			TSharedPtr<FJsonObject> StepObject = MakeShared<FJsonObject>();
			StepObject->SetStringField(TEXT("step"), Step);
			if (!Tool.IsEmpty())
			{
				StepObject->SetStringField(TEXT("tool"), Tool);
			}
			if (!Reason.IsEmpty())
			{
				StepObject->SetStringField(TEXT("reason"), Reason);
			}
			NextSteps.Add(MakeShared<FJsonValueObject>(StepObject));
		}

		FString MakeMcpGeneratedFunctionSuffixForApply(const FString& ToolName)
		{
			FString Suffix = SanitizeMcpToolIdForPath(ToolName);
			Suffix.RemoveFromStart(TEXT("unreal_"));
			TArray<FString> Parts;
			Suffix.ParseIntoArray(Parts, TEXT("_"), true);

			FString Result;
			for (const FString& Part : Parts)
			{
				if (Part.IsEmpty())
				{
					continue;
				}

				FString CleanPart = Part.ToLower();
				CleanPart[0] = FChar::ToUpper(CleanPart[0]);
				Result += CleanPart;
			}
			return Result.IsEmpty() ? TEXT("GeneratedTool") : Result;
		}

		FString IndentRegistryPatchObject(const FString& PatchText)
		{
			TArray<FString> Lines;
			PatchText.TrimStartAndEnd().ParseIntoArrayLines(Lines, false);

			FString Result;
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				Result += TEXT("    ");
				Result += Lines[Index];
				if (Index + 1 < Lines.Num())
				{
					Result += LINE_TERMINATOR;
				}
			}
			return Result;
		}

		bool AppendRegistryPatchText(
			const FString& BeforeText,
			const FString& PatchText,
			bool bHasExistingTools,
			FString& OutAfterText,
			FString& OutFailureReason)
		{
			const FString ToolsArrayEndAnchor = TEXT("\n  ]");
			const int32 InsertOffset = BeforeText.Find(ToolsArrayEndAnchor, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (InsertOffset == INDEX_NONE)
			{
				OutFailureReason = TEXT("Could not locate ToolRegistry tools array closing anchor.");
				return false;
			}

			const FString IndentedPatch = IndentRegistryPatchObject(PatchText);
			const FString InsertionText = FString::Printf(
				TEXT("%s%s"),
				bHasExistingTools ? TEXT(",\n") : TEXT("\n"),
				*IndentedPatch);

			OutAfterText = BeforeText;
			OutAfterText.InsertAt(InsertOffset, InsertionText);
			return true;
		}

		FString GetActiveExtensionSessionIdForManifest()
		{
			TSharedPtr<FJsonObject> LockObject;
			FString FailureReason;
			if (!LoadJsonObjectFromFile(GetMcpExtensionLockPath(), LockObject, FailureReason) || !LockObject.IsValid())
			{
				return FString();
			}

			FString SessionId;
			LockObject->TryGetStringField(TEXT("sessionId"), SessionId);
			return SessionId;
		}

		bool MakeApplySourcePath(const FString& RelativePrivateSourceFile, FString& OutSourcePath, FString& OutFailureReason)
		{
			const FString RelativeSourceFile = RelativePrivateSourceFile.TrimStartAndEnd();
			const FToolsReadResolution PluginSourceRoot = ResolvePluginSourceRoot();
			FString PluginSourceDirectory = PluginSourceRoot.Path;
			if (!PluginSourceRoot.bFound)
			{
				OutFailureReason = PluginSourceRoot.Warning.IsEmpty()
					? TEXT("UnrealMcp plugin source directory could not be resolved (IPluginManager not initialized).")
					: PluginSourceRoot.Warning;
				return false;
			}
			FPaths::NormalizeDirectoryName(PluginSourceDirectory);
			FPaths::CollapseRelativeDirectories(PluginSourceDirectory);
			FString PrivateSourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				PluginSourceDirectory,
				TEXT("UnrealMcp/Private")));
			FPaths::NormalizeDirectoryName(PrivateSourceDirectory);
			FPaths::CollapseRelativeDirectories(PrivateSourceDirectory);

			FString ResolvedSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				PrivateSourceDirectory,
				RelativeSourceFile));
			FPaths::NormalizeFilename(ResolvedSourcePath);
			FPaths::CollapseRelativeDirectories(ResolvedSourcePath);

			const FString PrivateSourceDirectoryPrefix = PrivateSourceDirectory.EndsWith(TEXT("/"))
				? PrivateSourceDirectory
				: PrivateSourceDirectory + TEXT("/");
			if (!ResolvedSourcePath.Equals(PrivateSourceDirectory, ESearchCase::IgnoreCase)
				&& !ResolvedSourcePath.StartsWith(PrivateSourceDirectoryPrefix, ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(
					TEXT("Source file path '%s' resolves outside allowed plugin source directory 'Plugins/UnrealMcp/Source'."),
					*RelativeSourceFile);
				return false;
			}

			OutSourcePath = ResolvedSourcePath;
			return true;
		}

		FString GetToolRegistryMirrorPath()
		{
			const FToolsReadResolution PluginBaseDir = ResolvePluginBaseDir();
			return PluginBaseDir.Path.IsEmpty()
				? FString()
				: FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginBaseDir.Path, TEXT("Resources/ToolRegistry/tools.json")));
		}

		FString GetCategorySourceFileForApply(const FString& Category)
		{
			if (Category == TEXT("actors"))
			{
				return TEXT("UnrealMcpActorTools.cpp");
			}
			if (Category == TEXT("blueprint"))
			{
				return TEXT("UnrealMcpBlueprintTools.cpp");
			}
			if (Category == TEXT("editor"))
			{
				return TEXT("UnrealMcpEditorTools.cpp");
			}
			if (Category == TEXT("memory"))
			{
				return TEXT("UnrealMcpMemoryTools.cpp");
			}
			if (Category == TEXT("scaffold"))
			{
				return TEXT("UnrealMcpScaffoldTools.cpp");
			}
			if (Category == TEXT("skills"))
			{
				return TEXT("UnrealMcpSkillTools.cpp");
			}
			if (Category == TEXT("widget"))
			{
				return TEXT("UnrealMcpWidgetTools.cpp");
			}
			return TEXT("UnrealMcpSelfExtensionTools.cpp");
		}

		FString GetCategoryTryExecuteForApply(const FString& Category)
		{
			if (Category == TEXT("actors"))
			{
				return TEXT("TryExecuteActorTool");
			}
			if (Category == TEXT("blueprint"))
			{
				return TEXT("TryExecuteBlueprintTool");
			}
			if (Category == TEXT("editor"))
			{
				return TEXT("TryExecuteEditorTool");
			}
			if (Category == TEXT("memory"))
			{
				return TEXT("TryExecuteMemoryTool");
			}
			if (Category == TEXT("scaffold"))
			{
				return TEXT("TryExecuteScaffoldTool");
			}
			if (Category == TEXT("skills"))
			{
				return TEXT("TryExecuteSkillTool");
			}
			if (Category == TEXT("widget"))
			{
				return TEXT("TryExecuteWidgetTool");
			}
			return TEXT("TryExecuteSelfExtensionTool");
		}

		bool TryMakeApplyRelativePathForDomain(
			const FString& Path,
			const FString& DomainRoot,
			const FString& DomainPrefix,
			FString& OutRelativePath)
		{
			if (DomainRoot.IsEmpty())
			{
				return false;
			}

			FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
			FString NormalizedRoot = FPaths::ConvertRelativePathToFull(DomainRoot);
			FPaths::NormalizeFilename(NormalizedPath);
			FPaths::NormalizeDirectoryName(NormalizedRoot);
			FPaths::CollapseRelativeDirectories(NormalizedPath);
			FPaths::CollapseRelativeDirectories(NormalizedRoot);

			const FString RootPrefix = NormalizedRoot.EndsWith(TEXT("/")) ? NormalizedRoot : NormalizedRoot + TEXT("/");
			if (!NormalizedPath.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
				&& !NormalizedPath.StartsWith(RootPrefix, ESearchCase::IgnoreCase))
			{
				return false;
			}

			FString RelativePath = NormalizedPath;
			FPaths::MakePathRelativeTo(RelativePath, *NormalizedRoot);
			OutRelativePath = DomainPrefix.IsEmpty()
				? RelativePath
				: FPaths::Combine(DomainPrefix, RelativePath);
			FPaths::NormalizeFilename(OutRelativePath);
			return true;
		}

		FString MakeApplyRelativePath(const FString& Path)
		{
			FString RelativePath;

			const FToolsReadResolution PluginBaseDir = ResolvePluginBaseDir();
			if (TryMakeApplyRelativePathForDomain(Path, PluginBaseDir.Path, TEXT("Plugins/UnrealMcp"), RelativePath))
			{
				return RelativePath;
			}

			const FToolsReadResolution ToolsRoot = ResolveToolsReadSubpath(FString(), TArray<FString>());
			for (const FString& Candidate : ToolsRoot.Candidates)
			{
				if (TryMakeApplyRelativePathForDomain(Path, Candidate, TEXT("Tools"), RelativePath))
				{
					return RelativePath;
				}
			}

			if (TryMakeApplyRelativePathForDomain(Path, FPaths::ProjectDir(), FString(), RelativePath))
			{
				return RelativePath;
			}

			FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(AbsolutePath);
			FPaths::CollapseRelativeDirectories(AbsolutePath);
			return AbsolutePath;
		}

		struct FMcpScaffoldRequiredInclude
		{
			FString TargetFile;
			TArray<FString> Includes;
		};

		struct FMcpScaffoldBuildRequirements
		{
			TArray<FMcpScaffoldRequiredInclude> RequiredIncludes;
			TArray<FString> RequiredModules;
			TArray<FString> DependsOn;
		};

		void AddUniqueTrimmedString(TArray<FString>& Values, const FString& RawValue)
		{
			const FString Value = RawValue.TrimStartAndEnd();
			if (!Value.IsEmpty() && !Values.Contains(Value))
			{
				Values.Add(Value);
			}
		}

		void ParseJsonStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<FString>& OutValues)
		{
			if (!Object.IsValid())
			{
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(FieldName, Values) || !Values)
			{
				return;
			}

			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					continue;
				}
				AddUniqueTrimmedString(OutValues, Value->AsString());
			}
		}

		void ParseScaffoldBuildRequirementsMetadata(
			const TSharedPtr<FJsonObject>& MetadataObject,
			FMcpScaffoldBuildRequirements& OutRequirements)
		{
			if (!MetadataObject.IsValid())
			{
				return;
			}

			ParseJsonStringArrayField(MetadataObject, TEXT("dependsOn"), OutRequirements.DependsOn);

			const TSharedPtr<FJsonObject>* BuildRequirementsObject = nullptr;
			if (!MetadataObject->TryGetObjectField(TEXT("buildRequirements"), BuildRequirementsObject)
				|| !BuildRequirementsObject
				|| !(*BuildRequirementsObject).IsValid())
			{
				return;
			}

			ParseJsonStringArrayField(*BuildRequirementsObject, TEXT("requiredModules"), OutRequirements.RequiredModules);

			const TArray<TSharedPtr<FJsonValue>>* IncludeEntries = nullptr;
			if (!(*BuildRequirementsObject)->TryGetArrayField(TEXT("requiredIncludes"), IncludeEntries) || !IncludeEntries)
			{
				return;
			}

			for (const TSharedPtr<FJsonValue>& EntryValue : *IncludeEntries)
			{
				if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
				FMcpScaffoldRequiredInclude Entry;
				EntryObject->TryGetStringField(TEXT("file"), Entry.TargetFile);
				Entry.TargetFile = Entry.TargetFile.TrimStartAndEnd();
				ParseJsonStringArrayField(EntryObject, TEXT("includes"), Entry.Includes);
				if (!Entry.TargetFile.IsEmpty() && Entry.Includes.Num() > 0)
				{
					OutRequirements.RequiredIncludes.Add(Entry);
				}
			}
		}

		FString NormalizeBuildRequirementInclude(const FString& RawInclude)
		{
			FString Include = RawInclude.TrimStartAndEnd();
			if (Include.StartsWith(TEXT("#include"), ESearchCase::CaseSensitive))
			{
				Include = Include.RightChop(8).TrimStartAndEnd();
			}
			if ((Include.StartsWith(TEXT("\""), ESearchCase::CaseSensitive) && Include.EndsWith(TEXT("\""), ESearchCase::CaseSensitive))
				|| (Include.StartsWith(TEXT("<"), ESearchCase::CaseSensitive) && Include.EndsWith(TEXT(">"), ESearchCase::CaseSensitive)))
			{
				Include = Include.Mid(1, Include.Len() - 2).TrimStartAndEnd();
			}
			return Include;
		}

		FString ExtractIncludeTargetFromLine(const FString& Line)
		{
			const FString TrimmedLine = Line.TrimStartAndEnd();
			if (!TrimmedLine.StartsWith(TEXT("#include"), ESearchCase::CaseSensitive))
			{
				return FString();
			}
			return NormalizeBuildRequirementInclude(TrimmedLine.RightChop(8));
		}

		bool SourceContainsInclude(const FString& SourceText, const FString& Include)
		{
			TArray<FString> Lines;
			SourceText.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				if (ExtractIncludeTargetFromLine(Line).Equals(Include, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		bool FindPrimaryIncludeBlockInsertOffset(const FString& SourceText, int32& OutInsertOffset)
		{
			const int32 TextLength = SourceText.Len();
			int32 LineStart = 0;
			bool bStartedIncludeBlock = false;
			int32 LastIncludeLineEnd = INDEX_NONE;

			while (LineStart < TextLength)
			{
				const int32 NewlineOffset = SourceText.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LineStart);
				const int32 LineEnd = NewlineOffset == INDEX_NONE ? TextLength : NewlineOffset;
				const int32 NextLineStart = NewlineOffset == INDEX_NONE ? TextLength : NewlineOffset + 1;
				const FString Line = SourceText.Mid(LineStart, LineEnd - LineStart);
				const FString TrimmedLine = Line.TrimStartAndEnd();

				if (TrimmedLine.IsEmpty())
				{
					LineStart = NextLineStart;
					continue;
				}

				if (!bStartedIncludeBlock
					&& (TrimmedLine.StartsWith(TEXT("//"), ESearchCase::CaseSensitive)
						|| TrimmedLine.StartsWith(TEXT("/*"), ESearchCase::CaseSensitive)
						|| TrimmedLine.StartsWith(TEXT("*"), ESearchCase::CaseSensitive)))
				{
					LineStart = NextLineStart;
					continue;
				}

				if (TrimmedLine.StartsWith(TEXT("#include"), ESearchCase::CaseSensitive))
				{
					bStartedIncludeBlock = true;
					LastIncludeLineEnd = NextLineStart;
					LineStart = NextLineStart;
					continue;
				}

				break;
			}

			if (LastIncludeLineEnd == INDEX_NONE)
			{
				return false;
			}

			OutInsertOffset = LastIncludeLineEnd;
			return true;
		}

		bool IsPathUnderDirectory(const FString& FilePath, const FString& Directory)
		{
			FString NormalizedFilePath = FPaths::ConvertRelativePathToFull(FilePath);
			FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(Directory);
			FPaths::NormalizeFilename(NormalizedFilePath);
			FPaths::NormalizeDirectoryName(NormalizedDirectory);
			FPaths::CollapseRelativeDirectories(NormalizedFilePath);
			FPaths::CollapseRelativeDirectories(NormalizedDirectory);
			const FString DirectoryPrefix = NormalizedDirectory.EndsWith(TEXT("/")) ? NormalizedDirectory : NormalizedDirectory + TEXT("/");
			return NormalizedFilePath.StartsWith(DirectoryPrefix, ESearchCase::IgnoreCase);
		}

		bool ResolveBuildRequirementTargetFile(const FString& RequestedFile, FString& OutSourcePath, FString& OutFailureReason)
		{
			FString RelativeFile = RequestedFile.TrimStartAndEnd();
			RelativeFile.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			if (RelativeFile.IsEmpty())
			{
				OutFailureReason = TEXT("buildRequirements.requiredIncludes entry has an empty file field.");
				return false;
			}

			const FToolsReadResolution PluginSourceRoot = ResolvePluginSourceRoot();
			const FString PluginSourceDirectory = PluginSourceRoot.Path;
			if (!PluginSourceRoot.bFound)
			{
				OutFailureReason = PluginSourceRoot.Warning.IsEmpty()
					? TEXT("UnrealMcp plugin source directory could not be resolved (IPluginManager not initialized).")
					: PluginSourceRoot.Warning;
				return false;
			}

			const FString PluginModuleSourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginSourceDirectory, TEXT("UnrealMcp")));
			const FString PrivateSourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginModuleSourceDirectory, TEXT("Private")));
			const FString PublicSourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginModuleSourceDirectory, TEXT("Public")));
			const FString ProjectRelativeSourcePrefix = TEXT("Plugins/UnrealMcp/Source/");
			const FString ProjectRelativePrivatePrefix = TEXT("Plugins/UnrealMcp/Source/UnrealMcp/Private/");
			const FString ProjectRelativePublicPrefix = TEXT("Plugins/UnrealMcp/Source/UnrealMcp/Public/");

			FString ResolvedPath;
			if (FPaths::IsRelative(RelativeFile))
			{
				if (RelativeFile.StartsWith(ProjectRelativePrivatePrefix, ESearchCase::IgnoreCase)
					|| RelativeFile.StartsWith(ProjectRelativePublicPrefix, ESearchCase::IgnoreCase))
				{
					ResolvedPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						PluginSourceDirectory,
						RelativeFile.RightChop(ProjectRelativeSourcePrefix.Len())));
				}
				else if (RelativeFile.StartsWith(TEXT("Private/"), ESearchCase::IgnoreCase)
					|| RelativeFile.StartsWith(TEXT("Public/"), ESearchCase::IgnoreCase))
				{
					ResolvedPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginModuleSourceDirectory, RelativeFile));
				}
				else
				{
					ResolvedPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(PrivateSourceDirectory, RelativeFile));
				}
			}
			else
			{
				ResolvedPath = FPaths::ConvertRelativePathToFull(RelativeFile);
			}

			FPaths::NormalizeFilename(ResolvedPath);
			FPaths::CollapseRelativeDirectories(ResolvedPath);
			if (!IsPathUnderDirectory(ResolvedPath, PrivateSourceDirectory) && !IsPathUnderDirectory(ResolvedPath, PublicSourceDirectory))
			{
				OutFailureReason = FString::Printf(
					TEXT("buildRequirements.requiredIncludes file '%s' resolves outside Plugins/UnrealMcp/Source/UnrealMcp/Private or Public."),
					*RequestedFile);
				return false;
			}

			OutSourcePath = ResolvedPath;
			return true;
		}

		FString GetMcpBuildCsPath()
		{
			const FToolsReadResolution PluginSourceDirectory = ResolvePluginSourceRoot();
			return !PluginSourceDirectory.bFound
				? FString()
				: FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginSourceDirectory.Path, TEXT("UnrealMcp/UnrealMcp.Build.cs")));
		}

		bool IsSafeUnrealModuleName(const FString& ModuleName)
		{
			if (ModuleName.IsEmpty())
			{
				return false;
			}
			for (int32 Index = 0; Index < ModuleName.Len(); ++Index)
			{
				if (!FChar::IsAlnum(ModuleName[Index]) && ModuleName[Index] != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeBuildRequirementPlanObject(
			const FString& Kind,
			const FString& Name,
			const FString& SourcePath,
			const FString& Status,
			int32 Offset)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("kind"), Kind);
			Object->SetStringField(TEXT("name"), Name);
			Object->SetStringField(TEXT("sourcePath"), SourcePath);
			Object->SetStringField(TEXT("relativePath"), MakeApplyRelativePath(SourcePath));
			Object->SetStringField(TEXT("status"), Status);
			Object->SetNumberField(TEXT("offset"), Offset);
			return Object;
		}

		bool PlanBuildRequirementIncludes(
			FString& SourceText,
			const FString& SourcePath,
			const FString& RequestedFile,
			const TArray<FString>& RawIncludes,
			bool bDryRun,
			TArray<TSharedPtr<FJsonValue>>& IncludesPlanned,
			TArray<TSharedPtr<FJsonValue>>& Changes,
			bool& bOutChanged)
		{
			TArray<FString> SeenIncludes;
			TArray<FString> MissingIncludes;
			for (const FString& RawInclude : RawIncludes)
			{
				const FString Include = NormalizeBuildRequirementInclude(RawInclude);
				if (Include.IsEmpty() || SeenIncludes.Contains(Include))
				{
					continue;
				}
				SeenIncludes.Add(Include);

				if (SourceContainsInclude(SourceText, Include))
				{
					TSharedPtr<FJsonObject> PlannedObject = MakeBuildRequirementPlanObject(
						TEXT("include"),
						Include,
						SourcePath,
						TEXT("already_present"),
						INDEX_NONE);
					PlannedObject->SetStringField(TEXT("file"), RequestedFile);
					PlannedObject->SetStringField(TEXT("line"), FString::Printf(TEXT("#include \"%s\""), *Include));
					IncludesPlanned.Add(MakeShared<FJsonValueObject>(PlannedObject));
					continue;
				}

				MissingIncludes.Add(Include);
			}

			if (MissingIncludes.Num() == 0)
			{
				return true;
			}

			int32 InsertOffset = INDEX_NONE;
			if (!FindPrimaryIncludeBlockInsertOffset(SourceText, InsertOffset))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("BuildRequirementsIncludes"),
					EMcpScaffoldInsertionStatus::MissingAnchor,
					FString::Printf(TEXT("Could not find primary #include block in required include target '%s'."), *RequestedFile),
					INDEX_NONE,
					FString::Join(MissingIncludes, TEXT(", ")))));
				return false;
			}

			FString InsertionText;
			for (const FString& Include : MissingIncludes)
			{
				InsertionText += FString::Printf(TEXT("#include \"%s\"%s"), *Include, LINE_TERMINATOR);

				TSharedPtr<FJsonObject> PlannedObject = MakeBuildRequirementPlanObject(
					TEXT("include"),
					Include,
					SourcePath,
					bDryRun ? TEXT("will_insert") : TEXT("inserted"),
					InsertOffset);
				PlannedObject->SetStringField(TEXT("file"), RequestedFile);
				PlannedObject->SetStringField(TEXT("line"), FString::Printf(TEXT("#include \"%s\""), *Include));
				IncludesPlanned.Add(MakeShared<FJsonValueObject>(PlannedObject));
			}

			Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
				TEXT("BuildRequirementsIncludes"),
				bDryRun ? EMcpScaffoldInsertionStatus::WillInsert : EMcpScaffoldInsertionStatus::Inserted,
				bDryRun ? TEXT("Would insert required #include lines from ScaffoldMetadata buildRequirements.") : TEXT("Inserted required #include lines from ScaffoldMetadata buildRequirements."),
				InsertOffset,
				InsertionText.Left(800))));

			SourceText.InsertAt(InsertOffset, InsertionText);
			bOutChanged = true;
			return true;
		}

		bool FindPrivateDependencyModuleBlock(const FString& BuildCsText, int32& OutBlockStart, int32& OutInsertOffset)
		{
			const FString Anchor = TEXT("PrivateDependencyModuleNames.AddRange(new[]");
			const int32 AnchorOffset = BuildCsText.Find(Anchor, ESearchCase::CaseSensitive);
			if (AnchorOffset == INDEX_NONE)
			{
				return false;
			}

			const int32 OpenBraceOffset = BuildCsText.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, AnchorOffset + Anchor.Len());
			if (OpenBraceOffset == INDEX_NONE)
			{
				return false;
			}

			const int32 CloseOffset = BuildCsText.Find(TEXT("\n\t\t});"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenBraceOffset);
			if (CloseOffset == INDEX_NONE)
			{
				return false;
			}

			OutBlockStart = OpenBraceOffset;
			OutInsertOffset = CloseOffset;
			return true;
		}

		bool PrivateDependencyBlockContainsModule(const FString& BuildCsText, int32 BlockStart, int32 InsertOffset, const FString& ModuleName)
		{
			if (BlockStart == INDEX_NONE || InsertOffset == INDEX_NONE || InsertOffset <= BlockStart)
			{
				return false;
			}
			const FString BlockText = BuildCsText.Mid(BlockStart, InsertOffset - BlockStart);
			return BlockText.Contains(FString::Printf(TEXT("\"%s\""), *ModuleName), ESearchCase::CaseSensitive);
		}

		bool PlanBuildRequirementModules(
			FString& BuildCsText,
			const FString& BuildCsPath,
			const TArray<FString>& RequiredModules,
			bool bDryRun,
			TArray<TSharedPtr<FJsonValue>>& ModulesPlanned,
			TArray<TSharedPtr<FJsonValue>>& Changes,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			bool& bOutChanged)
		{
			if (RequiredModules.Num() == 0)
			{
				return true;
			}

			int32 BlockStart = INDEX_NONE;
			int32 InsertOffset = INDEX_NONE;
			if (!FindPrivateDependencyModuleBlock(BuildCsText, BlockStart, InsertOffset))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("BuildRequirementsModules"),
					EMcpScaffoldInsertionStatus::MissingAnchor,
					TEXT("Could not find UnrealMcp.Build.cs PrivateDependencyModuleNames AddRange block."),
					INDEX_NONE,
					FString())));
				return false;
			}

			TArray<FString> SeenModules;
			TArray<FString> MissingModules;
			for (const FString& RawModule : RequiredModules)
			{
				const FString ModuleName = RawModule.TrimStartAndEnd();
				if (SeenModules.Contains(ModuleName))
				{
					continue;
				}
				SeenModules.Add(ModuleName);

				if (!IsSafeUnrealModuleName(ModuleName))
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("severity"), TEXT("error"));
					Issue->SetStringField(TEXT("code"), TEXT("invalid_build_requirement_module"));
					Issue->SetStringField(TEXT("file"), TEXT("ScaffoldMetadata.json"));
					Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Invalid required module name '%s'. Module names may contain only letters, digits, and underscore."), *RawModule));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
					return false;
				}

				if (PrivateDependencyBlockContainsModule(BuildCsText, BlockStart, InsertOffset, ModuleName))
				{
					TSharedPtr<FJsonObject> PlannedObject = MakeBuildRequirementPlanObject(
						TEXT("module"),
						ModuleName,
						BuildCsPath,
						TEXT("already_present"),
						INDEX_NONE);
					PlannedObject->SetStringField(TEXT("module"), ModuleName);
					ModulesPlanned.Add(MakeShared<FJsonValueObject>(PlannedObject));
					continue;
				}

				MissingModules.Add(ModuleName);
			}

			if (MissingModules.Num() == 0)
			{
				return true;
			}

			const FString PrefixBeforeInsert = BuildCsText.Left(InsertOffset).TrimStartAndEnd();
			const bool bNeedsLeadingComma = !PrefixBeforeInsert.EndsWith(TEXT("{"), ESearchCase::CaseSensitive)
				&& !PrefixBeforeInsert.EndsWith(TEXT(","), ESearchCase::CaseSensitive);
			FString InsertionText;
			for (int32 ModuleIndex = 0; ModuleIndex < MissingModules.Num(); ++ModuleIndex)
			{
				if (ModuleIndex > 0 || bNeedsLeadingComma)
				{
					InsertionText += TEXT(",");
				}
				InsertionText += FString::Printf(TEXT("%s\t\t\t\"%s\""), LINE_TERMINATOR, *MissingModules[ModuleIndex]);

				TSharedPtr<FJsonObject> PlannedObject = MakeBuildRequirementPlanObject(
					TEXT("module"),
					MissingModules[ModuleIndex],
					BuildCsPath,
					bDryRun ? TEXT("will_insert") : TEXT("inserted"),
					InsertOffset);
				PlannedObject->SetStringField(TEXT("module"), MissingModules[ModuleIndex]);
				ModulesPlanned.Add(MakeShared<FJsonValueObject>(PlannedObject));
			}

			Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
				TEXT("BuildRequirementsModules"),
				bDryRun ? EMcpScaffoldInsertionStatus::WillInsert : EMcpScaffoldInsertionStatus::Inserted,
				bDryRun ? TEXT("Would insert required Unreal module dependencies from ScaffoldMetadata buildRequirements.") : TEXT("Inserted required Unreal module dependencies from ScaffoldMetadata buildRequirements."),
				InsertOffset,
				InsertionText.Left(800))));

			BuildCsText.InsertAt(InsertOffset, InsertionText);
			bOutChanged = true;
			return true;
		}

		bool IsCppIdentifierStart(TCHAR Character)
		{
			return FChar::IsAlpha(Character) || Character == TEXT('_');
		}

		bool IsCppIdentifierChar(TCHAR Character)
		{
			return FChar::IsAlnum(Character) || Character == TEXT('_');
		}

		bool TrySkipCppLineOrBlockComment(const FString& SourceText, int32& Offset)
		{
			const int32 TextLength = SourceText.Len();
			if (Offset + 1 >= TextLength || SourceText[Offset] != TEXT('/'))
			{
				return false;
			}

			if (SourceText[Offset + 1] == TEXT('/'))
			{
				Offset += 2;
				while (Offset < TextLength && SourceText[Offset] != TEXT('\n'))
				{
					++Offset;
				}
				return true;
			}

			if (SourceText[Offset + 1] == TEXT('*'))
			{
				Offset += 2;
				while (Offset + 1 < TextLength)
				{
					if (SourceText[Offset] == TEXT('*') && SourceText[Offset + 1] == TEXT('/'))
					{
						Offset += 2;
						return true;
					}
					++Offset;
				}
				Offset = TextLength;
				return true;
			}

			return false;
		}

		bool TrySkipCppRawStringLiteral(const FString& SourceText, int32& Offset)
		{
			const int32 TextLength = SourceText.Len();
			if (Offset + 2 >= TextLength || SourceText[Offset] != TEXT('R') || SourceText[Offset + 1] != TEXT('"'))
			{
				return false;
			}

			const int32 DelimiterStart = Offset + 2;
			int32 OpenParenOffset = DelimiterStart;
			while (OpenParenOffset < TextLength
				&& SourceText[OpenParenOffset] != TEXT('(')
				&& SourceText[OpenParenOffset] != TEXT('\n')
				&& SourceText[OpenParenOffset] != TEXT('\r'))
			{
				++OpenParenOffset;
			}
			if (OpenParenOffset >= TextLength || SourceText[OpenParenOffset] != TEXT('('))
			{
				return false;
			}

			const FString Delimiter = SourceText.Mid(DelimiterStart, OpenParenOffset - DelimiterStart);
			const FString ClosingNeedle = FString::Printf(TEXT(")%s\""), *Delimiter);
			const int32 ClosingOffset = SourceText.Find(ClosingNeedle, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenParenOffset + 1);
			Offset = ClosingOffset == INDEX_NONE ? TextLength : ClosingOffset + ClosingNeedle.Len();
			return true;
		}

		bool TrySkipCppStringOrCharacterLiteral(const FString& SourceText, int32& Offset)
		{
			if (TrySkipCppRawStringLiteral(SourceText, Offset))
			{
				return true;
			}

			const int32 TextLength = SourceText.Len();
			if (Offset >= TextLength || (SourceText[Offset] != TEXT('"') && SourceText[Offset] != TEXT('\'')))
			{
				return false;
			}

			const TCHAR QuoteCharacter = SourceText[Offset];
			++Offset;
			while (Offset < TextLength)
			{
				if (SourceText[Offset] == TEXT('\\'))
				{
					Offset += 2;
					continue;
				}
				if (SourceText[Offset] == QuoteCharacter)
				{
					++Offset;
					return true;
				}
				++Offset;
			}

			return true;
		}

		void SkipCppTrivia(const FString& SourceText, int32& Offset)
		{
			const int32 TextLength = SourceText.Len();
			while (Offset < TextLength)
			{
				if (FChar::IsWhitespace(SourceText[Offset]))
				{
					++Offset;
					continue;
				}
				if (TrySkipCppLineOrBlockComment(SourceText, Offset))
				{
					continue;
				}
				break;
			}
		}

		FString ReadPreviousCppIdentifier(const FString& SourceText, int32 BeforeOffset)
		{
			int32 EndOffset = BeforeOffset - 1;
			while (EndOffset >= 0 && FChar::IsWhitespace(SourceText[EndOffset]))
			{
				--EndOffset;
			}

			if (EndOffset < 0 || !IsCppIdentifierChar(SourceText[EndOffset]))
			{
				return FString();
			}

			int32 StartOffset = EndOffset;
			while (StartOffset > 0 && IsCppIdentifierChar(SourceText[StartOffset - 1]))
			{
				--StartOffset;
			}
			return SourceText.Mid(StartOffset, EndOffset - StartOffset + 1);
		}

		int32 FindMatchingCppDelimiter(const FString& SourceText, int32 OpenOffset, TCHAR OpenCharacter, TCHAR CloseCharacter)
		{
			const int32 TextLength = SourceText.Len();
			int32 Offset = OpenOffset;
			int32 Depth = 0;
			while (Offset < TextLength)
			{
				if (TrySkipCppLineOrBlockComment(SourceText, Offset) || TrySkipCppStringOrCharacterLiteral(SourceText, Offset))
				{
					continue;
				}

				if (SourceText[Offset] == OpenCharacter)
				{
					++Depth;
				}
				else if (SourceText[Offset] == CloseCharacter)
				{
					--Depth;
					if (Depth == 0)
					{
						return Offset;
					}
				}
				++Offset;
			}

			return INDEX_NONE;
		}

		bool IsCppBodyEmpty(const FString& SourceText, int32 BodyStartOffset, int32 BodyEndOffset)
		{
			int32 Offset = BodyStartOffset + 1;
			while (Offset < BodyEndOffset)
			{
				if (FChar::IsWhitespace(SourceText[Offset]))
				{
					++Offset;
					continue;
				}
				if (TrySkipCppLineOrBlockComment(SourceText, Offset))
				{
					continue;
				}
				return false;
			}
			return true;
		}

		bool TryFindFirstTopLevelIfAnchorInBody(
			const FString& SourceText,
			int32 BodyStartOffset,
			int32 BodyEndOffset,
			FString& OutAnchor)
		{
			int32 Offset = BodyStartOffset + 1;
			int32 NestedBraceDepth = 0;
			while (Offset < BodyEndOffset)
			{
				if (TrySkipCppLineOrBlockComment(SourceText, Offset) || TrySkipCppStringOrCharacterLiteral(SourceText, Offset))
				{
					continue;
				}

				if (SourceText[Offset] == TEXT('{'))
				{
					++NestedBraceDepth;
					++Offset;
					continue;
				}
				if (SourceText[Offset] == TEXT('}'))
				{
					NestedBraceDepth = FMath::Max(0, NestedBraceDepth - 1);
					++Offset;
					continue;
				}

				if (NestedBraceDepth == 0 && IsCppIdentifierStart(SourceText[Offset]))
				{
					const int32 TokenStartOffset = Offset;
					while (Offset < BodyEndOffset && IsCppIdentifierChar(SourceText[Offset]))
					{
						++Offset;
					}
					if (SourceText.Mid(TokenStartOffset, Offset - TokenStartOffset) != TEXT("if"))
					{
						continue;
					}

					int32 AfterIfOffset = Offset;
					SkipCppTrivia(SourceText, AfterIfOffset);
					if (AfterIfOffset >= BodyEndOffset || SourceText[AfterIfOffset] != TEXT('('))
					{
						continue;
					}

					int32 LineStartOffset = TokenStartOffset;
					while (LineStartOffset > BodyStartOffset + 1
						&& SourceText[LineStartOffset - 1] != TEXT('\n')
						&& SourceText[LineStartOffset - 1] != TEXT('\r'))
					{
						--LineStartOffset;
					}

					bool bOnlyIndentationBeforeIf = true;
					for (int32 LineOffset = LineStartOffset; LineOffset < TokenStartOffset; ++LineOffset)
					{
						if (!FChar::IsWhitespace(SourceText[LineOffset]))
						{
							bOnlyIndentationBeforeIf = false;
							break;
						}
					}
					if (!bOnlyIndentationBeforeIf)
					{
						continue;
					}

					int32 AnchorStartOffset = LineStartOffset;
					if (AnchorStartOffset > 0 && SourceText[AnchorStartOffset - 1] == TEXT('\n'))
					{
						--AnchorStartOffset;
						if (AnchorStartOffset > 0 && SourceText[AnchorStartOffset - 1] == TEXT('\r'))
						{
							--AnchorStartOffset;
						}
					}
					else if (AnchorStartOffset > 0 && SourceText[AnchorStartOffset - 1] == TEXT('\r'))
					{
						--AnchorStartOffset;
					}

					OutAnchor = SourceText.Mid(AnchorStartOffset, BodyEndOffset - AnchorStartOffset + 1);
					return true;
				}

				++Offset;
			}

			return false;
		}

		bool FindTryExecuteFirstIfAnchor(
			const FString& SourceText,
			const FString& TryExecuteName,
			FString& OutAnchor,
			FString& OutDispatcherBodyScope,
			FString& OutFailureReason)
		{
			OutAnchor.Reset();
			OutDispatcherBodyScope.Reset();
			OutFailureReason.Reset();

			const FString CleanTryExecuteName = TryExecuteName.TrimStartAndEnd();
			if (CleanTryExecuteName.IsEmpty())
			{
				OutFailureReason = TEXT("TryExecute dispatcher name is empty.");
				return false;
			}

			const int32 TextLength = SourceText.Len();
			int32 Offset = 0;
			while (Offset < TextLength)
			{
				if (TrySkipCppLineOrBlockComment(SourceText, Offset) || TrySkipCppStringOrCharacterLiteral(SourceText, Offset))
				{
					continue;
				}

				if (!IsCppIdentifierStart(SourceText[Offset]))
				{
					++Offset;
					continue;
				}

				const int32 TokenStartOffset = Offset;
				while (Offset < TextLength && IsCppIdentifierChar(SourceText[Offset]))
				{
					++Offset;
				}

				if (SourceText.Mid(TokenStartOffset, Offset - TokenStartOffset) != CleanTryExecuteName)
				{
					continue;
				}

				if (ReadPreviousCppIdentifier(SourceText, TokenStartOffset) != TEXT("bool"))
				{
					continue;
				}

				int32 AfterNameOffset = Offset;
				SkipCppTrivia(SourceText, AfterNameOffset);
				if (AfterNameOffset >= TextLength || SourceText[AfterNameOffset] != TEXT('('))
				{
					continue;
				}

				const int32 SignatureEndOffset = FindMatchingCppDelimiter(SourceText, AfterNameOffset, TEXT('('), TEXT(')'));
				if (SignatureEndOffset == INDEX_NONE)
				{
					continue;
				}

				int32 AfterSignatureOffset = SignatureEndOffset + 1;
				SkipCppTrivia(SourceText, AfterSignatureOffset);
				if (AfterSignatureOffset < TextLength && SourceText[AfterSignatureOffset] == TEXT(';'))
				{
					continue;
				}
				if (AfterSignatureOffset >= TextLength || SourceText[AfterSignatureOffset] != TEXT('{'))
				{
					continue;
				}

				const int32 BodyEndOffset = FindMatchingCppDelimiter(SourceText, AfterSignatureOffset, TEXT('{'), TEXT('}'));
				if (BodyEndOffset == INDEX_NONE)
				{
					OutFailureReason = FString::Printf(TEXT("Definition for %s has no matching closing brace."), *CleanTryExecuteName);
					return false;
				}

				if (IsCppBodyEmpty(SourceText, AfterSignatureOffset, BodyEndOffset))
				{
					OutFailureReason = FString::Printf(TEXT("Definition for %s has an empty body; cannot locate dispatcher insertion point."), *CleanTryExecuteName);
					return false;
				}

				if (!TryFindFirstTopLevelIfAnchorInBody(SourceText, AfterSignatureOffset, BodyEndOffset, OutAnchor))
				{
					OutFailureReason = FString::Printf(TEXT("Definition for %s has no top-level 'if (' dispatcher branch."), *CleanTryExecuteName);
					return false;
				}

				OutDispatcherBodyScope = SourceText.Mid(
					AfterSignatureOffset,
					BodyEndOffset - AfterSignatureOffset + 1);
				return true;
			}

			OutFailureReason = FString::Printf(TEXT("Could not find a non-declaration definition for %s; skipped comments, string literals, and prototype-only lines."), *CleanTryExecuteName);
			return false;
		}

		bool ReadScaffoldPatchFile(
			const FString& ScaffoldDirectory,
			const FString& FileName,
			bool bRequired,
			FString& OutText,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutFailureReason)
		{
			const FString Path = FPaths::Combine(ScaffoldDirectory, FileName);
			if (!FPaths::FileExists(Path))
			{
				if (bRequired)
				{
					OutFailureReason = FString::Printf(TEXT("Descriptor-first patch file '%s' is required. Regenerate the scaffold with the current unreal.scaffold_mcp_tool."), *Path);
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("severity"), TEXT("error"));
					Issue->SetStringField(TEXT("file"), FileName);
					Issue->SetStringField(TEXT("message"), OutFailureReason);
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
					return false;
				}
				return true;
			}

			if (!FFileHelper::LoadFileToString(OutText, *Path))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to read patch file '%s'."), *Path);
				return false;
			}
			return true;
		}

		bool PlanOrApplyPatchInsertion(
			FString& SourceText,
			const FString& ConflictSearchScope,
			const FString& Section,
			const FString& SourcePath,
			const FString& ToolName,
			const FString& PatchText,
			const FString& Anchor,
			const FString& ConflictNeedle,
			bool bDryRun,
			TArray<TSharedPtr<FJsonValue>>& Changes,
			bool& bOutChanged)
		{
			const FString TrimmedPatch = PatchText.TrimStartAndEnd();
			if (TrimmedPatch.IsEmpty())
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::Conflict,
					TEXT("Patch fragment is empty."),
					INDEX_NONE,
					FString())));
				return false;
			}

			if (SourceText.Contains(TrimmedPatch, ESearchCase::CaseSensitive))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::SkippedAlreadyIntegrated,
					TEXT("Exact patch fragment is already present."),
					INDEX_NONE,
					FString())));
				return true;
			}

			if (!ConflictNeedle.IsEmpty() && ConflictSearchScope.Contains(ConflictNeedle, ESearchCase::CaseSensitive))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::Conflict,
					FString::Printf(TEXT("Source already contains conflict marker '%s' but not the exact patch fragment."), *ConflictNeedle),
					INDEX_NONE,
					TrimmedPatch.Left(800))));
				return false;
			}

			const int32 AnchorOffset = SourceText.Find(Anchor, ESearchCase::CaseSensitive);
			int32 ResolvedAnchorOffset = AnchorOffset;
			if (ResolvedAnchorOffset == INDEX_NONE)
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::MissingAnchor,
					FString::Printf(TEXT("Insertion anchor was not found in %s."), *SourcePath),
					INDEX_NONE,
					TrimmedPatch.Left(800))));
				return false;
			}

			const FString InsertionText = FString::Printf(TEXT("\n%s\n"), *TrimmedPatch);
			Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
				Section,
				bDryRun ? EMcpScaffoldInsertionStatus::WillInsert : EMcpScaffoldInsertionStatus::Inserted,
				bDryRun ? TEXT("Would insert patch fragment before anchor.") : TEXT("Inserted patch fragment before anchor."),
				ResolvedAnchorOffset,
				TrimmedPatch.Left(800))));

			SourceText.InsertAt(ResolvedAnchorOffset, InsertionText);
			bOutChanged = true;
			return true;
		}

		struct FScaffoldApplyContext
		{
			const FJsonObject* Arguments = nullptr;
			FString ScaffoldDirectory;
			FToolsReadResolution ScaffoldResolution;
			FString ToolName;
		};

		struct FApplyResult
		{
			FUnrealMcpExecutionResult ExecutionResult;
			TSharedPtr<FJsonObject> StructuredContent;
		};

		struct FResolvedScaffoldDependency
		{
			FString ToolName;
			FString ToolId;
			FString ScaffoldDirectory;
			TArray<FString> Aliases;
		};

		FString MakeScaffoldDependencyKey(const FString& ToolNameOrId)
		{
			return SanitizeMcpToolIdForPath(ToolNameOrId).ToLower();
		}

		FString NormalizeScaffoldDirectoryForDependency(const FString& Directory)
		{
			FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(Directory);
			FPaths::NormalizeDirectoryName(NormalizedDirectory);
			FPaths::CollapseRelativeDirectories(NormalizedDirectory);
			return NormalizedDirectory;
		}

		TSharedPtr<FJsonObject> MakeDependencyIssue(const FString& Code, const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("file"), TEXT("ScaffoldMetadata.json"));
			Issue->SetStringField(TEXT("message"), Message);
			return Issue;
		}

		bool ReadScaffoldDependsOn(const FString& ScaffoldDirectory, TArray<FString>& OutDependsOn)
		{
			OutDependsOn.Reset();

			const FString MetadataPath = FPaths::Combine(ScaffoldDirectory, TEXT("ScaffoldMetadata.json"));
			if (!FPaths::FileExists(MetadataPath))
			{
				return true;
			}

			TSharedPtr<FJsonObject> MetadataObject;
			FString FailureReason;
			if (!LoadJsonObjectFromFile(MetadataPath, MetadataObject, FailureReason) || !MetadataObject.IsValid())
			{
				// Preserve pre-G3b behavior: malformed/missing optional metadata never blocked
				// a single-scaffold apply. It simply contributes no dependency edges.
				return true;
			}

			FMcpScaffoldBuildRequirements Requirements;
			ParseScaffoldBuildRequirementsMetadata(MetadataObject, Requirements);
			OutDependsOn = Requirements.DependsOn;
			return true;
		}

		void ReadScaffoldDependencyIdentity(const FString& ScaffoldDirectory, FString& OutToolName, FString& OutToolId)
		{
			OutToolName.Reset();
			OutToolId.Reset();

			const FString MetadataPath = FPaths::Combine(ScaffoldDirectory, TEXT("ScaffoldMetadata.json"));
			TSharedPtr<FJsonObject> MetadataObject;
			FString FailureReason;
			if (LoadJsonObjectFromFile(MetadataPath, MetadataObject, FailureReason) && MetadataObject.IsValid())
			{
				MetadataObject->TryGetStringField(TEXT("toolName"), OutToolName);
				MetadataObject->TryGetStringField(TEXT("toolId"), OutToolId);
				OutToolName = OutToolName.TrimStartAndEnd();
				OutToolId = OutToolId.TrimStartAndEnd();
			}

			if (OutToolName.IsEmpty())
			{
				const FString RegistryPatchPath = FPaths::Combine(ScaffoldDirectory, TEXT("ToolRegistryPatch.json"));
				TSharedPtr<FJsonObject> RegistryPatchObject;
				if (LoadJsonObjectFromFile(RegistryPatchPath, RegistryPatchObject, FailureReason) && RegistryPatchObject.IsValid())
				{
					RegistryPatchObject->TryGetStringField(TEXT("name"), OutToolName);
					OutToolName = OutToolName.TrimStartAndEnd();
				}
			}

			if (OutToolId.IsEmpty())
			{
				OutToolId = FPaths::GetCleanFilename(NormalizeScaffoldDirectoryForDependency(ScaffoldDirectory));
			}
		}

		void AddScaffoldDependencyKeyFromString(const FString& Value, TArray<FString>& OutKeys)
		{
			if (Value.TrimStartAndEnd().IsEmpty())
			{
				return;
			}

			const FString Key = MakeScaffoldDependencyKey(Value);
			if (!Key.IsEmpty() && !OutKeys.Contains(Key))
			{
				OutKeys.Add(Key);
			}
		}

		void GetScaffoldDependencyKeys(const FResolvedScaffoldDependency& Node, TArray<FString>& OutKeys)
		{
			OutKeys.Reset();
			AddScaffoldDependencyKeyFromString(Node.ToolName, OutKeys);
			AddScaffoldDependencyKeyFromString(Node.ToolId, OutKeys);
			for (const FString& Alias : Node.Aliases)
			{
				AddScaffoldDependencyKeyFromString(Alias, OutKeys);
			}
			AddScaffoldDependencyKeyFromString(FPaths::GetCleanFilename(Node.ScaffoldDirectory), OutKeys);
		}

		bool DependencyKeyMatchesNode(const FString& DependencyKey, const FResolvedScaffoldDependency& Node)
		{
			TArray<FString> NodeKeys;
			GetScaffoldDependencyKeys(Node, NodeKeys);
			return NodeKeys.Contains(DependencyKey);
		}

		FString MakeScaffoldDependencyVisitKey(const FResolvedScaffoldDependency& Node)
		{
			return NormalizeScaffoldDirectoryForDependency(Node.ScaffoldDirectory).ToLower();
		}

		bool MakeResolvedDependencyNodeFromDirectory(
			const FString& DependencyReference,
			const FString& ScaffoldDirectory,
			FResolvedScaffoldDependency& OutNode)
		{
			OutNode.ScaffoldDirectory = NormalizeScaffoldDirectoryForDependency(ScaffoldDirectory);
			ReadScaffoldDependencyIdentity(OutNode.ScaffoldDirectory, OutNode.ToolName, OutNode.ToolId);
			if (OutNode.ToolName.IsEmpty())
			{
				OutNode.ToolName = DependencyReference.TrimStartAndEnd();
			}
			if (OutNode.ToolId.IsEmpty())
			{
				OutNode.ToolId = SanitizeMcpToolIdForPath(DependencyReference);
			}
			return !OutNode.ToolName.IsEmpty();
		}

		bool TryResolveDependencyScaffold(
			const FString& DependencyRoot,
			const FString& DependencyReference,
			FResolvedScaffoldDependency& OutNode)
		{
			OutNode = FResolvedScaffoldDependency();
			const FString DependencyKey = MakeScaffoldDependencyKey(DependencyReference);
			const FString DirectCandidate = NormalizeScaffoldDirectoryForDependency(FPaths::Combine(
				DependencyRoot,
				SanitizeMcpToolIdForPath(DependencyReference)));
			if (FPaths::DirectoryExists(DirectCandidate))
			{
				return MakeResolvedDependencyNodeFromDirectory(DependencyReference, DirectCandidate, OutNode);
			}

			TArray<FString> ChildDirectories;
			IFileManager::Get().FindFiles(ChildDirectories, *FPaths::Combine(DependencyRoot, TEXT("*")), false, true);
			ChildDirectories.Sort();
			for (const FString& ChildDirectory : ChildDirectories)
			{
				const FString CandidateDirectory = NormalizeScaffoldDirectoryForDependency(
					FPaths::IsRelative(ChildDirectory) ? FPaths::Combine(DependencyRoot, ChildDirectory) : ChildDirectory);
				if (!FPaths::DirectoryExists(CandidateDirectory))
				{
					continue;
				}

				FResolvedScaffoldDependency CandidateNode;
				if (!MakeResolvedDependencyNodeFromDirectory(DependencyReference, CandidateDirectory, CandidateNode))
				{
					continue;
				}

				TArray<FString> CandidateKeys;
				GetScaffoldDependencyKeys(CandidateNode, CandidateKeys);
				if (CandidateKeys.Contains(DependencyKey))
				{
					OutNode = CandidateNode;
					return true;
				}
			}

			return false;
		}

		bool ResolveDependencyScaffoldRoot(
			const FJsonObject& Arguments,
			const FString& CurrentScaffoldDirectory,
			FString& OutDependencyRoot,
			FString& OutFailureReason)
		{
			FString RequestedOutputRoot;
			Arguments.TryGetStringField(TEXT("outputRoot"), RequestedOutputRoot);
			if (!RequestedOutputRoot.TrimStartAndEnd().IsEmpty())
			{
				return ResolveProjectOutputDirectory(RequestedOutputRoot, OutDependencyRoot, OutFailureReason);
			}

			FString RequestedScaffoldDirectory;
			Arguments.TryGetStringField(TEXT("scaffoldDir"), RequestedScaffoldDirectory);
			if (!RequestedScaffoldDirectory.TrimStartAndEnd().IsEmpty())
			{
				OutDependencyRoot = NormalizeScaffoldDirectoryForDependency(FPaths::GetPath(CurrentScaffoldDirectory));
				return true;
			}

			OutDependencyRoot = NormalizeScaffoldDirectoryForDependency(FPaths::GetPath(CurrentScaffoldDirectory));
			return true;
		}

		bool ResolveScaffoldDependencyOrder(
			const FResolvedScaffoldDependency& RootNode,
			const FString& DependencyRoot,
			TArray<FResolvedScaffoldDependency>& OutApplyOrder,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutFailureReason)
		{
			OutApplyOrder.Reset();
			OutFailureReason.Reset();

			TMap<FString, int32> VisitState;
			TArray<FString> StackKeys;
			TArray<FString> StackDisplayNames;

			TFunction<bool(const FResolvedScaffoldDependency&)> VisitNode;
			VisitNode = [
				&DependencyRoot,
				&Issues,
				&OutApplyOrder,
				&OutFailureReason,
				&RootNode,
				&StackDisplayNames,
				&StackKeys,
				&VisitNode,
				&VisitState
			](const FResolvedScaffoldDependency& Node) -> bool
			{
				const FString NodeKey = MakeScaffoldDependencyVisitKey(Node);
				const int32* ExistingState = VisitState.Find(NodeKey);
				if (ExistingState && *ExistingState == 2)
				{
					return true;
				}
				if (ExistingState && *ExistingState == 1)
				{
					int32 CycleStartIndex = INDEX_NONE;
					for (int32 StackIndex = 0; StackIndex < StackKeys.Num(); ++StackIndex)
					{
						if (StackKeys[StackIndex] == NodeKey)
						{
							CycleStartIndex = StackIndex;
							break;
						}
					}

					TArray<FString> CyclePath;
					if (CycleStartIndex != INDEX_NONE)
					{
						for (int32 PathIndex = CycleStartIndex; PathIndex < StackDisplayNames.Num(); ++PathIndex)
						{
							CyclePath.Add(StackDisplayNames[PathIndex]);
						}
					}
					CyclePath.Add(Node.ToolName);

					OutFailureReason = FString::Printf(TEXT("dependency cycle detected: %s"), *FString::Join(CyclePath, TEXT(" -> ")));
					Issues.Add(MakeShared<FJsonValueObject>(MakeDependencyIssue(TEXT("dependency_cycle_detected"), OutFailureReason)));
					return false;
				}

				VisitState.Add(NodeKey, 1);
				StackKeys.Add(NodeKey);
				StackDisplayNames.Add(Node.ToolName);

				TArray<FString> DependsOn;
				ReadScaffoldDependsOn(Node.ScaffoldDirectory, DependsOn);
				for (const FString& DependencyToolId : DependsOn)
				{
					const FString DependencyKey = MakeScaffoldDependencyKey(DependencyToolId);
					if (DependencyKeyMatchesNode(DependencyKey, Node))
					{
						OutFailureReason = TEXT("scaffold cannot depend on itself");
						Issues.Add(MakeShared<FJsonValueObject>(MakeDependencyIssue(TEXT("dependency_self_reference"), OutFailureReason)));
						return false;
					}

					FResolvedScaffoldDependency DependencyNode;
					if (DependencyKeyMatchesNode(DependencyKey, RootNode))
					{
						DependencyNode = RootNode;
					}
					else if (!TryResolveDependencyScaffold(DependencyRoot, DependencyToolId, DependencyNode))
					{
						OutFailureReason = FString::Printf(TEXT("dependency scaffold not found: %s"), *DependencyToolId);
						Issues.Add(MakeShared<FJsonValueObject>(MakeDependencyIssue(TEXT("dependency_scaffold_not_found"), OutFailureReason)));
						return false;
					}

					if (!VisitNode(DependencyNode))
					{
						return false;
					}
				}

				StackKeys.Pop(EAllowShrinking::No);
				StackDisplayNames.Pop(EAllowShrinking::No);
				VisitState.Add(NodeKey, 2);
				OutApplyOrder.Add(Node);
				return true;
			};

			return VisitNode(RootNode);
		}

		bool TryGetJsonBoolField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool DefaultValue)
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			bool Value = DefaultValue;
			Object->TryGetBoolField(FieldName, Value);
			return Value;
		}

		int32 CountJsonArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
		{
			if (!Object.IsValid())
			{
				return 0;
			}

			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object->TryGetArrayField(FieldName, Values) && Values)
			{
				return Values->Num();
			}
			return 0;
		}

		TSharedPtr<FJsonObject> MakeDependencyChainEntry(
			const FResolvedScaffoldDependency& Node,
			const FApplyResult& Result,
			bool bDryRun)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("toolName"), Node.ToolName);
			Entry->SetStringField(TEXT("scaffoldDir"), Node.ScaffoldDirectory);
			Entry->SetBoolField(TEXT("applied"), !bDryRun && !Result.ExecutionResult.bIsError);
			if (bDryRun)
			{
				Entry->SetBoolField(TEXT("planned"), !Result.ExecutionResult.bIsError);
			}
			Entry->SetStringField(TEXT("summary"), Result.ExecutionResult.Text);

			const int32 SourcesWritten = bDryRun
				? 0
				: CountJsonArrayField(Result.StructuredContent, TEXT("manifestFiles"));
			Entry->SetNumberField(TEXT("sourcesWritten"), SourcesWritten);

			const TSharedPtr<FJsonObject>* BuildRequirementsObject = nullptr;
			if (Result.StructuredContent.IsValid()
				&& Result.StructuredContent->TryGetObjectField(TEXT("buildRequirements"), BuildRequirementsObject)
				&& BuildRequirementsObject
				&& (*BuildRequirementsObject).IsValid())
			{
				Entry->SetObjectField(TEXT("buildRequirements"), *BuildRequirementsObject);
			}
			else
			{
				Entry->SetObjectField(TEXT("buildRequirements"), MakeShared<FJsonObject>());
			}

			Entry->SetNumberField(TEXT("issues"), CountJsonArrayField(Result.StructuredContent, TEXT("issues")));
			return Entry;
		}

		TSharedPtr<FJsonObject> MakeDependencyFailureContent(
			const FString& ToolName,
			const FString& ScaffoldDirectory,
			const FToolsReadResolution& ScaffoldResolution,
			const FString& DependencyRoot,
			bool bDryRun,
			const FString& FailureReason,
			const TArray<TSharedPtr<FJsonValue>>& Issues,
			const TArray<TSharedPtr<FJsonValue>>& DependencyChain)
		{
			TSharedPtr<FJsonObject> RegistrationStatusObject = MakeShared<FJsonObject>();
			RegistrationStatusObject->SetBoolField(TEXT("scaffoldExists"), FPaths::DirectoryExists(ScaffoldDirectory));
			RegistrationStatusObject->SetBoolField(TEXT("registeredUsableNow"), false);
			RegistrationStatusObject->SetBoolField(TEXT("requiresApply"), true);
			RegistrationStatusObject->SetBoolField(TEXT("requiresBuildRestartForRuntimeVisibility"), true);
			RegistrationStatusObject->SetStringField(TEXT("notRegisteredReason"), FailureReason);

			TArray<TSharedPtr<FJsonValue>> NextSteps;
			AddScaffoldNextStep(NextSteps, TEXT("Repair ScaffoldMetadata.json dependsOn or create the missing prerequisite scaffold under the dependency root."), TEXT("unreal.scaffold_mcp_tool"), FailureReason);
			AddScaffoldNextStep(NextSteps, TEXT("Rerun mcp_apply_scaffold with dryRun=true after dependency resolution succeeds."), TEXT("unreal.mcp_apply_scaffold"), TEXT("Dependencies must apply in topological order before the requested scaffold."));

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
			StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
			StructuredContent->SetBoolField(TEXT("scaffoldFound"), ScaffoldResolution.bFound);
			StructuredContent->SetStringField(TEXT("scaffoldSourceKind"), LexToString(ScaffoldResolution.SourceKind));
			StructuredContent->SetArrayField(TEXT("scaffoldCandidates"), MakeToolsReadCandidateValues(ScaffoldResolution));
			if (!ScaffoldResolution.Warning.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("scaffoldResolutionWarning"), ScaffoldResolution.Warning);
			}
			StructuredContent->SetStringField(TEXT("dependencyRoot"), DependencyRoot);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("canApply"), false);
			StructuredContent->SetBoolField(TEXT("patchesSafe"), false);
			StructuredContent->SetArrayField(TEXT("issues"), Issues);
			StructuredContent->SetArrayField(TEXT("dependencyChain"), DependencyChain);
			StructuredContent->SetObjectField(TEXT("registrationStatus"), RegistrationStatusObject);
			StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);
			return StructuredContent;
		}

		FUnrealMcpExecutionResult ApplyResolvedScaffoldExecution(const FScaffoldApplyContext& Ctx)
		{
			if (!Ctx.Arguments)
			{
				return MakeExecutionResult(TEXT("ApplyResolvedScaffold requires non-null arguments."), nullptr, true);
			}

			const FJsonObject& Arguments = *Ctx.Arguments;
			FString ScaffoldDirectory = Ctx.ScaffoldDirectory;
			const FToolsReadResolution& ScaffoldResolution = Ctx.ScaffoldResolution;
			FString ToolName = Ctx.ToolName;
			FString FailureReason;

			bool bDryRun = true;
			bool bApplyChatCommand = false;
			bool bCreateBackup = true;
			bool bValidatePatches = true;
			bool bAllowUnsafePatches = false;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
			Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);
			Arguments.TryGetBoolField(TEXT("validatePatches"), bValidatePatches);
			Arguments.TryGetBoolField(TEXT("allowUnsafePatches"), bAllowUnsafePatches);
			const int32 TargetDiffPreviewLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("targetDiffPreviewLines"), 120), 1000);

			TArray<TSharedPtr<FJsonValue>> Issues;
			auto MakeEarlyFailureContent = [&ToolName, &ScaffoldDirectory, &Issues, &ScaffoldResolution](const FString& Reason, const FString& FirstStep, const FString& FirstTool)
			{
				TSharedPtr<FJsonObject> RegistrationStatusObject = MakeShared<FJsonObject>();
				RegistrationStatusObject->SetBoolField(TEXT("scaffoldExists"), FPaths::DirectoryExists(ScaffoldDirectory));
				RegistrationStatusObject->SetBoolField(TEXT("registeredUsableNow"), false);
				RegistrationStatusObject->SetBoolField(TEXT("requiresApply"), true);
				RegistrationStatusObject->SetBoolField(TEXT("requiresBuildRestartForRuntimeVisibility"), true);
				RegistrationStatusObject->SetStringField(TEXT("notRegisteredReason"), Reason);

				TArray<TSharedPtr<FJsonValue>> NextSteps;
				AddScaffoldNextStep(NextSteps, FirstStep, FirstTool, Reason);
				AddScaffoldNextStep(NextSteps, TEXT("Inspect the scaffold for missing files, patch safety, and registry metadata."), TEXT("unreal.mcp_inspect_scaffold"), TEXT("Inspection now checks readiness before apply."));
				AddScaffoldNextStep(NextSteps, TEXT("Rerun mcp_apply_scaffold with dryRun=true after repairing the scaffold."), TEXT("unreal.mcp_apply_scaffold"), TEXT("Dry run must pass before source integration."));

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
				StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				StructuredContent->SetBoolField(TEXT("scaffoldFound"), ScaffoldResolution.bFound);
				StructuredContent->SetStringField(TEXT("scaffoldSourceKind"), LexToString(ScaffoldResolution.SourceKind));
				StructuredContent->SetArrayField(TEXT("scaffoldCandidates"), MakeToolsReadCandidateValues(ScaffoldResolution));
				if (!ScaffoldResolution.Warning.IsEmpty())
				{
					StructuredContent->SetStringField(TEXT("scaffoldResolutionWarning"), ScaffoldResolution.Warning);
				}
				StructuredContent->SetBoolField(TEXT("canApply"), false);
				StructuredContent->SetBoolField(TEXT("patchesSafe"), false);
				StructuredContent->SetArrayField(TEXT("issues"), Issues);
				StructuredContent->SetObjectField(TEXT("registrationStatus"), RegistrationStatusObject);
				StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);
				return StructuredContent;
			};

			FString RegistrarPatch;
			FString RegistrarCallPatch;
			FString CategoryHandlerPatch;
			FString CategoryDispatcherPatch;
			FString ChatCommandPatch;
			if (!ReadScaffoldPatchFile(ScaffoldDirectory, TEXT("ToolRegistrar.patch.cpp"), true, RegistrarPatch, Issues, FailureReason)
				|| !ReadScaffoldPatchFile(ScaffoldDirectory, TEXT("ToolRegistrarCall.patch.cpp"), true, RegistrarCallPatch, Issues, FailureReason)
				|| !ReadScaffoldPatchFile(ScaffoldDirectory, TEXT("CategoryHandlerFunction.patch.cpp"), true, CategoryHandlerPatch, Issues, FailureReason)
				|| !ReadScaffoldPatchFile(ScaffoldDirectory, TEXT("CategoryDispatcherBranch.patch.cpp"), true, CategoryDispatcherPatch, Issues, FailureReason)
				|| !ReadScaffoldPatchFile(ScaffoldDirectory, TEXT("ChatCommand.patch.cpp"), bApplyChatCommand, ChatCommandPatch, Issues, FailureReason))
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeEarlyFailureContent(
					FailureReason,
					TEXT("Regenerate the scaffold or restore the missing descriptor-first patch file."),
					TEXT("unreal.scaffold_mcp_tool"));
				return MakeExecutionResult(FailureReason, StructuredContent, true);
			}

			const FString RegistryPatchPath = FPaths::Combine(ScaffoldDirectory, TEXT("ToolRegistryPatch.json"));
			FString RegistryPatchText;
			if (!FFileHelper::LoadFileToString(RegistryPatchText, *RegistryPatchPath))
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("file"), TEXT("ToolRegistryPatch.json"));
				Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to read ToolRegistry patch file '%s'."), *RegistryPatchPath));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				TSharedPtr<FJsonObject> StructuredContent = MakeEarlyFailureContent(
					FString::Printf(TEXT("Failed to read ToolRegistry patch file '%s'."), *RegistryPatchPath),
					TEXT("Regenerate the scaffold or restore ToolRegistryPatch.json."),
					TEXT("unreal.scaffold_mcp_tool"));
				return MakeExecutionResult(
					FString::Printf(TEXT("Failed to read ToolRegistry patch file '%s'."), *RegistryPatchPath),
					StructuredContent,
					true);
			}

			TSharedPtr<FJsonObject> RegistryPatchObject;
			if (!LoadJsonObjectFromFile(RegistryPatchPath, RegistryPatchObject, FailureReason) || !RegistryPatchObject.IsValid())
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeEarlyFailureContent(
					FailureReason,
					TEXT("Fix ToolRegistryPatch.json so it is valid JSON and matches the generated tool name."),
					TEXT("unreal.scaffold_mcp_tool"));
				StructuredContent->SetStringField(TEXT("registryPatchPath"), RegistryPatchPath);
				return MakeExecutionResult(FailureReason, StructuredContent, true);
			}

			FString RegistryToolName;
			FString Category;
			RegistryPatchObject->TryGetStringField(TEXT("name"), RegistryToolName);
			RegistryPatchObject->TryGetStringField(TEXT("category"), Category);
			if (!RegistryToolName.IsEmpty() && !RegistryToolName.Equals(ToolName, ESearchCase::CaseSensitive))
			{
				const FString Reason = FString::Printf(TEXT("ToolRegistryPatch.json name '%s' does not match requested tool '%s'."), *RegistryToolName, *ToolName);
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("file"), TEXT("ToolRegistryPatch.json"));
				Issue->SetStringField(TEXT("message"), Reason);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				return MakeExecutionResult(
					Reason,
					MakeEarlyFailureContent(
						Reason,
						TEXT("Fix ToolRegistryPatch.json name or rerun scaffold generation with the intended toolName."),
						TEXT("unreal.scaffold_mcp_tool")),
					true);
			}
			if (Category.TrimStartAndEnd().IsEmpty())
			{
				Category = TEXT("self-extension");
			}

			FString CategorySourceFile;
			FString TryExecuteName;
			FMcpScaffoldBuildRequirements BuildRequirements;
			TSharedPtr<FJsonObject> MetadataObject;
			if (LoadJsonObjectFromFile(FPaths::Combine(ScaffoldDirectory, TEXT("ScaffoldMetadata.json")), MetadataObject, FailureReason) && MetadataObject.IsValid())
			{
				MetadataObject->TryGetStringField(TEXT("categorySourceFile"), CategorySourceFile);
				MetadataObject->TryGetStringField(TEXT("categoryTryExecute"), TryExecuteName);
				ParseScaffoldBuildRequirementsMetadata(MetadataObject, BuildRequirements);
			}
			if (CategorySourceFile.TrimStartAndEnd().IsEmpty())
			{
				CategorySourceFile = GetCategorySourceFileForApply(Category);
			}
			if (TryExecuteName.TrimStartAndEnd().IsEmpty())
			{
				TryExecuteName = GetCategoryTryExecuteForApply(Category);
			}

			TArray<TSharedPtr<FJsonValue>> PatchValidations;
			bool bPatchesSafe = true;
			if (bValidatePatches)
			{
				TSharedPtr<FJsonObject> RegistrarValidation = ValidateCppSnippetText(RegistrarPatch, TEXT("ToolRegistrar.patch.cpp"), ToolName);
				TSharedPtr<FJsonObject> RegistrarCallValidation = ValidateCppSnippetText(RegistrarCallPatch, TEXT("ToolRegistrarCall.patch.cpp"), ToolName);
				TSharedPtr<FJsonObject> CategoryHandlerValidation = ValidateCppSnippetText(CategoryHandlerPatch, TEXT("CategoryHandlerFunction.patch.cpp"), ToolName);
				TSharedPtr<FJsonObject> CategoryDispatcherValidation = ValidateCppSnippetText(CategoryDispatcherPatch, TEXT("CategoryDispatcherBranch.patch.cpp"), ToolName);
				bPatchesSafe &= RegistrarValidation->GetBoolField(TEXT("safe"));
				bPatchesSafe &= RegistrarCallValidation->GetBoolField(TEXT("safe"));
				bPatchesSafe &= CategoryHandlerValidation->GetBoolField(TEXT("safe"));
				bPatchesSafe &= CategoryDispatcherValidation->GetBoolField(TEXT("safe"));
				PatchValidations.Add(MakeShared<FJsonValueObject>(RegistrarValidation));
				PatchValidations.Add(MakeShared<FJsonValueObject>(RegistrarCallValidation));
				PatchValidations.Add(MakeShared<FJsonValueObject>(CategoryHandlerValidation));
				PatchValidations.Add(MakeShared<FJsonValueObject>(CategoryDispatcherValidation));
				if (bApplyChatCommand)
				{
					TSharedPtr<FJsonObject> ChatValidation = ValidateCppSnippetText(ChatCommandPatch, TEXT("ChatCommand.patch.cpp"), ToolName);
					bPatchesSafe &= ChatValidation->GetBoolField(TEXT("safe"));
					PatchValidations.Add(MakeShared<FJsonValueObject>(ChatValidation));
				}
			}

			FString RegistrarPath;
			if (!MakeApplySourcePath(TEXT("UnrealMcpToolRegistrar.cpp"), RegistrarPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString CategorySourcePath;
			if (!MakeApplySourcePath(CategorySourceFile, CategorySourcePath, FailureReason))
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("source_path_outside_plugin_source"));
				Issue->SetStringField(TEXT("file"), TEXT("ScaffoldMetadata.json"));
				Issue->SetStringField(TEXT("message"), FailureReason);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));

				TSharedPtr<FJsonObject> StructuredContent = MakeEarlyFailureContent(
					FailureReason,
					TEXT("Fix ScaffoldMetadata.json categorySourceFile so it stays within Plugins/UnrealMcp/Source."),
					TEXT("unreal.scaffold_mcp_tool"));
				return MakeExecutionResult(FailureReason, StructuredContent, true);
			}
			const FString ModuleSourcePath = GetMcpModuleSourcePath();
			// Reader domain: registry source prefers project Tools and falls back to shared repo-root Tools.
			const FToolsReadResolution RegistryResolution = ResolveToolsReadSubpath(
				TEXT("UnrealMcpToolRegistry/tools.json"),
				{ TEXT("tools.json") });
			const FString RegistrySourcePath = FPaths::ConvertRelativePathToFull(RegistryResolution.Path);
			const FString RegistryMirrorPath = GetToolRegistryMirrorPath();
			const FString BuildCsPath = GetMcpBuildCsPath();
			if (RegistryMirrorPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("UnrealMcp plugin base directory could not be resolved (IPluginManager not initialized)."), nullptr, true);
			}
			if (BuildRequirements.RequiredModules.Num() > 0 && BuildCsPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("UnrealMcp plugin source directory could not be resolved (IPluginManager not initialized)."), nullptr, true);
			}
			if (bApplyChatCommand && ModuleSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("UnrealMcp module source path could not be resolved (IPluginManager not initialized)."), nullptr, true);
			}
			TMap<FString, FString> BuildRequirementIncludeTargets;
			for (const FMcpScaffoldRequiredInclude& IncludeEntry : BuildRequirements.RequiredIncludes)
			{
				FString IncludeTargetPath;
				if (!ResolveBuildRequirementTargetFile(IncludeEntry.TargetFile, IncludeTargetPath, FailureReason))
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("severity"), TEXT("error"));
					Issue->SetStringField(TEXT("code"), TEXT("build_requirement_include_target_outside_allowed_source"));
					Issue->SetStringField(TEXT("file"), TEXT("ScaffoldMetadata.json"));
					Issue->SetStringField(TEXT("message"), FailureReason);
					Issues.Add(MakeShared<FJsonValueObject>(Issue));

					TSharedPtr<FJsonObject> StructuredContent = MakeEarlyFailureContent(
						FailureReason,
						TEXT("Fix ScaffoldMetadata.json buildRequirements.requiredIncludes so each file targets Plugins/UnrealMcp/Source/UnrealMcp/Private or Public."),
						TEXT("unreal.scaffold_mcp_tool"));
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
				BuildRequirementIncludeTargets.Add(IncludeEntry.TargetFile, IncludeTargetPath);
			}

			TMap<FString, FString> BeforeTexts;
			TMap<FString, FString> PlannedTexts;
			auto LoadTextTarget = [&BeforeTexts, &PlannedTexts](const FString& Path, FString& OutFailureReason) -> bool
			{
				if (BeforeTexts.Contains(Path))
				{
					return true;
				}

				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *Path))
				{
					OutFailureReason = FString::Printf(TEXT("Failed to read source file '%s'."), *Path);
					return false;
				}
				BeforeTexts.Add(Path, Text);
				PlannedTexts.Add(Path, Text);
				return true;
			};

			if (!LoadTextTarget(RegistrarPath, FailureReason)
				|| !LoadTextTarget(CategorySourcePath, FailureReason)
				|| !LoadTextTarget(RegistrySourcePath, FailureReason)
				|| !LoadTextTarget(RegistryMirrorPath, FailureReason)
				|| (bApplyChatCommand && !LoadTextTarget(ModuleSourcePath, FailureReason)))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			for (const TPair<FString, FString>& Pair : BuildRequirementIncludeTargets)
			{
				if (!LoadTextTarget(Pair.Value, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
			}
			if (BuildRequirements.RequiredModules.Num() > 0 && !LoadTextTarget(BuildCsPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TArray<TSharedPtr<FJsonValue>> Changes;
			TArray<TSharedPtr<FJsonValue>> BuildRequirementIncludesPlanned;
			TArray<TSharedPtr<FJsonValue>> BuildRequirementModulesPlanned;
			bool bChanged = false;
			bool bCanApply = true;
			const FString GeneratedFunctionSuffix = MakeMcpGeneratedFunctionSuffixForApply(ToolName);

			for (const FMcpScaffoldRequiredInclude& IncludeEntry : BuildRequirements.RequiredIncludes)
			{
				const FString* IncludeTargetPath = BuildRequirementIncludeTargets.Find(IncludeEntry.TargetFile);
				if (!IncludeTargetPath)
				{
					continue;
				}
				FString& IncludeTargetText = PlannedTexts.FindChecked(*IncludeTargetPath);
				bCanApply &= PlanBuildRequirementIncludes(
					IncludeTargetText,
					*IncludeTargetPath,
					IncludeEntry.TargetFile,
					IncludeEntry.Includes,
					bDryRun,
					BuildRequirementIncludesPlanned,
					Changes,
					bChanged);
			}
			if (BuildRequirements.RequiredModules.Num() > 0)
			{
				FString& BuildCsText = PlannedTexts.FindChecked(BuildCsPath);
				bCanApply &= PlanBuildRequirementModules(
					BuildCsText,
					BuildCsPath,
					BuildRequirements.RequiredModules,
					bDryRun,
					BuildRequirementModulesPlanned,
					Changes,
					Issues,
					bChanged);
			}

			FString& RegistrarText = PlannedTexts.FindChecked(RegistrarPath);
			const FString RegistrarBefore = BeforeTexts.FindChecked(RegistrarPath);
			const FString ToolNameNeedle = FString::Printf(TEXT("TEXT(\"%s\")"), *ToolName);
			bCanApply &= PlanOrApplyPatchInsertion(
				RegistrarText,
				RegistrarBefore,
				TEXT("ToolRegistrarDescriptor"),
				RegistrarPath,
				ToolName,
				RegistrarPatch,
				TEXT("\n\t\tvoid RegisterAllMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)"),
				ToolNameNeedle,
				bDryRun,
				Changes,
				bChanged);

			bCanApply &= PlanOrApplyPatchInsertion(
				RegistrarText,
				RegistrarBefore,
				TEXT("ToolRegistrarCall"),
				RegistrarPath,
				ToolName,
				RegistrarCallPatch,
				TEXT("\n\t\t}\n\t}\n\n\tvoid FUnrealMcpToolRegistrar::Add"),
				FString::Printf(TEXT("RegisterGenerated%sDescriptor"), *GeneratedFunctionSuffix),
				bDryRun,
				Changes,
				bChanged);

			FString& CategoryText = PlannedTexts.FindChecked(CategorySourcePath);
			const FString CategoryBefore = BeforeTexts.FindChecked(CategorySourcePath);
			bCanApply &= PlanOrApplyPatchInsertion(
				CategoryText,
				CategoryBefore,
				TEXT("CategoryHandlerFunction"),
				CategorySourcePath,
				ToolName,
				CategoryHandlerPatch,
				FString::Printf(TEXT("\n\tbool %s("), *TryExecuteName),
				FString::Printf(TEXT("ExecuteGenerated%sTool"), *GeneratedFunctionSuffix),
				bDryRun,
				Changes,
				bChanged);

			FString DispatcherAnchor;
			FString DispatcherConflictSearchScope;
			FString DispatcherAnchorFailureReason;
			if (!FindTryExecuteFirstIfAnchor(
				CategoryBefore,
				TryExecuteName,
				DispatcherAnchor,
				DispatcherConflictSearchScope,
				DispatcherAnchorFailureReason))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("CategoryDispatcherBranch"),
					EMcpScaffoldInsertionStatus::MissingAnchor,
					DispatcherAnchorFailureReason.IsEmpty()
						? FString::Printf(TEXT("Could not find first top-level if branch in %s."), *TryExecuteName)
						: DispatcherAnchorFailureReason,
					INDEX_NONE,
					CategoryDispatcherPatch.Left(800))));
				bCanApply = false;
			}
			else
			{
				bCanApply &= PlanOrApplyPatchInsertion(
					CategoryText,
					DispatcherConflictSearchScope,
					TEXT("CategoryDispatcherBranch"),
					CategorySourcePath,
					ToolName,
					CategoryDispatcherPatch,
					DispatcherAnchor,
					ToolNameNeedle,
					bDryRun,
					Changes,
					bChanged);
			}

			const FString ChatCommandAnchor =
				TEXT("\n\t\treturn UnrealMcp::MakeExecutionResult(TEXT(\"Unknown command. Try /help.\"), nullptr, true);");
			const FString ChatCommandNeedle = FString::Printf(TEXT("TEXT(\"/%s\")"), *SanitizeMcpToolIdForPath(ToolName));
			if (bApplyChatCommand)
			{
				FString& ModuleText = PlannedTexts.FindChecked(ModuleSourcePath);
				const FString ModuleBefore = BeforeTexts.FindChecked(ModuleSourcePath);
				bCanApply &= PlanOrApplyPatchInsertion(
					ModuleText,
					ModuleBefore,
					TEXT("ExecuteChatCommand"),
					ModuleSourcePath,
					ToolName,
					ChatCommandPatch,
					ChatCommandAnchor,
					ChatCommandNeedle,
					bDryRun,
					Changes,
					bChanged);
			}
			else
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("ExecuteChatCommand"),
					EMcpScaffoldInsertionStatus::SkippedOptionalMissing,
					TEXT("applyChatCommand=false; skipped optional chat command patch."),
					INDEX_NONE,
					FString())));
			}

			TSharedPtr<FJsonObject> RegistryObject;
			if (!LoadJsonObjectFromFile(RegistrySourcePath, RegistryObject, FailureReason) || !RegistryObject.IsValid())
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const TArray<TSharedPtr<FJsonValue>>* ExistingTools = nullptr;
			TArray<TSharedPtr<FJsonValue>> ToolsArray;
			if (RegistryObject->TryGetArrayField(TEXT("tools"), ExistingTools) && ExistingTools)
			{
				ToolsArray = *ExistingTools;
			}

			bool bRegistryAlreadyHasTool = false;
			for (const TSharedPtr<FJsonValue>& ToolValue : ToolsArray)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
				{
					continue;
				}
				FString ExistingName;
				if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), ExistingName) && ExistingName == ToolName)
				{
					bRegistryAlreadyHasTool = true;
					break;
				}
			}

			if (bRegistryAlreadyHasTool)
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("ToolRegistryPatch"),
					EMcpScaffoldInsertionStatus::SkippedAlreadyIntegrated,
					TEXT("ToolRegistry already contains this tool."),
					INDEX_NONE,
					FString())));
			}
			else
			{
				FString RegistryAfterText;
				const FString& RegistryBeforeText = PlannedTexts.FindChecked(RegistrySourcePath);
				if (!AppendRegistryPatchText(RegistryBeforeText, RegistryPatchText, ToolsArray.Num() > 0, RegistryAfterText, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				PlannedTexts.FindChecked(RegistrySourcePath) = RegistryAfterText;
				PlannedTexts.FindChecked(RegistryMirrorPath) = RegistryAfterText;
				bChanged = true;
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("ToolRegistryPatch"),
					bDryRun ? EMcpScaffoldInsertionStatus::WillInsert : EMcpScaffoldInsertionStatus::Inserted,
					bDryRun ? TEXT("Would append ToolRegistryPatch.json to both registry files.") : TEXT("Appended ToolRegistryPatch.json to both registry files."),
					INDEX_NONE,
					ToolName)));
			}

			if (bValidatePatches && !bPatchesSafe && !bAllowUnsafePatches)
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("patch_validation_failed"));
				Issue->SetStringField(TEXT("message"), TEXT("One or more scaffold patch fragments failed static validation. The generated tool is still only a scaffold and will not be registered until the patches are repaired, applied, built, and the editor is restarted."));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				bCanApply = false;
			}

			TArray<TSharedPtr<FJsonValue>> TargetDiffs;
			TArray<TSharedPtr<FJsonValue>> ChangedFiles;
			for (const TPair<FString, FString>& Pair : BeforeTexts)
			{
				const FString* PlannedText = PlannedTexts.Find(Pair.Key);
				if (!PlannedText)
				{
					continue;
				}

				TSharedPtr<FJsonObject> FileObject = MakeShared<FJsonObject>();
				const FString HashBefore = HashTextForManifest(Pair.Value);
				const FString HashAfter = HashTextForManifest(*PlannedText);
				FileObject->SetStringField(TEXT("sourcePath"), Pair.Key);
				FileObject->SetStringField(TEXT("relativePath"), MakeApplyRelativePath(Pair.Key));
				FileObject->SetStringField(TEXT("hashBefore"), HashBefore);
				FileObject->SetStringField(TEXT("hashAfter"), HashAfter);
				FileObject->SetBoolField(TEXT("changed"), Pair.Value != *PlannedText);
				FileObject->SetObjectField(TEXT("diff"), MakeTextDiffObject(Pair.Value, *PlannedText, TargetDiffPreviewLines));
				TSharedPtr<FJsonObject> DiskVerificationObject = MakeShared<FJsonObject>();
				DiskVerificationObject->SetStringField(TEXT("status"), bDryRun ? TEXT("dry_run") : TEXT("pending"));
				DiskVerificationObject->SetStringField(TEXT("expectedHashBefore"), HashBefore);
				DiskVerificationObject->SetStringField(TEXT("expectedHashAfter"), HashAfter);
				DiskVerificationObject->SetBoolField(TEXT("verifiedOnDisk"), false);
				FileObject->SetObjectField(TEXT("diskVerification"), DiskVerificationObject);
				TargetDiffs.Add(MakeShared<FJsonValueObject>(FileObject));
				if (Pair.Value != *PlannedText)
				{
					ChangedFiles.Add(MakeShared<FJsonValueObject>(FileObject));
				}
			}

			const int32 ConflictCount = CountScaffoldChangesByStatus(Changes, TEXT("conflict"));
			const int32 MissingAnchorCount = CountScaffoldChangesByStatus(Changes, TEXT("missing_anchor"));
			const FString ExtensionSessionId = GetActiveExtensionSessionIdForManifest();
			const TSharedPtr<FJsonObject> ConflictPolicy = MakeScaffoldConflictPolicyObject();
			const bool bDescriptorRegisteredNow = FindRegisteredMcpToolDescriptor(ToolName) != nullptr;
			const bool bHandlerRegisteredNow = IsRegisteredToolHandler(ToolName);
			const bool bToolRegistryLoadedNow = FindToolRegistryEntry(ToolName) != nullptr;
			const FString& RegistryPlannedText = PlannedTexts.FindChecked(RegistrySourcePath);
			const bool bRegistryPatchIntegrated =
				bRegistryAlreadyHasTool
				|| RegistryPlannedText.Contains(FString::Printf(TEXT("\"name\": \"%s\""), *ToolName), ESearchCase::CaseSensitive)
				|| RegistryPlannedText.Contains(FString::Printf(TEXT("\"name\":\"%s\""), *ToolName), ESearchCase::CaseSensitive);

			TSharedPtr<FJsonObject> PostcheckObject = MakeShared<FJsonObject>();
			PostcheckObject->SetStringField(TEXT("mode"), bDryRun ? TEXT("planned") : TEXT("applied"));
			PostcheckObject->SetBoolField(TEXT("descriptorSourceIntegrated"), RegistrarText.Contains(ToolName, ESearchCase::CaseSensitive) && RegistrarText.Contains(RegistrarCallPatch.TrimStartAndEnd(), ESearchCase::CaseSensitive));
			PostcheckObject->SetBoolField(TEXT("handlerSourceIntegrated"), CategoryText.Contains(CategoryHandlerPatch.TrimStartAndEnd(), ESearchCase::CaseSensitive) && CategoryText.Contains(CategoryDispatcherPatch.TrimStartAndEnd(), ESearchCase::CaseSensitive));
			PostcheckObject->SetBoolField(TEXT("registryPatchIntegrated"), bRegistryPatchIntegrated);
			PostcheckObject->SetBoolField(TEXT("descriptorRegisteredInCurrentSession"), bDescriptorRegisteredNow);
			PostcheckObject->SetBoolField(TEXT("handlerMapEntryInCurrentSession"), bHandlerRegisteredNow);
			PostcheckObject->SetBoolField(TEXT("toolListedInCurrentSession"), bDescriptorRegisteredNow && bToolRegistryLoadedNow && bHandlerRegisteredNow);
			PostcheckObject->SetBoolField(TEXT("requiresBuildRestartForRuntimeVisibility"), !(bDescriptorRegisteredNow && bToolRegistryLoadedNow && bHandlerRegisteredNow));
			PostcheckObject->SetStringField(TEXT("verificationTarget"), TEXT("descriptor registered + handler map entry + tools/list visibility + generated test suite after build/restart"));

			FString NotRegisteredReason;
			if (bValidatePatches && !bPatchesSafe && !bAllowUnsafePatches)
			{
				NotRegisteredReason = TEXT("Patch validation failed, so no source integration should proceed. Repair the patch fragments first.");
			}
			else if (ConflictCount > 0)
			{
				NotRegisteredReason = TEXT("One or more patch fragments conflict with existing source. Resolve the conflict before applying.");
			}
			else if (MissingAnchorCount > 0)
			{
				NotRegisteredReason = TEXT("The target source anchor was not found. Regenerate the scaffold for the current code layout or patch the dispatcher/registrar fragment.");
			}
			else if (bDryRun)
			{
				NotRegisteredReason = TEXT("This was a dry run. Source files were not changed, so the tool is not registered yet.");
			}
			else if (!(bDescriptorRegisteredNow && bToolRegistryLoadedNow && bHandlerRegisteredNow))
			{
				NotRegisteredReason = TEXT("Source/registry changes may be on disk, but the running editor still has the old compiled module and loaded registry. Build, restart the editor, then run tools/list or the generated tests.");
			}

			TSharedPtr<FJsonObject> RegistrationStatusObject = MakeShared<FJsonObject>();
			RegistrationStatusObject->SetBoolField(TEXT("scaffoldExists"), true);
			RegistrationStatusObject->SetBoolField(TEXT("requiredPatchFilesPresent"), true);
			RegistrationStatusObject->SetBoolField(TEXT("registryPatchPresent"), true);
			RegistrationStatusObject->SetBoolField(TEXT("patchesSafe"), bPatchesSafe);
			RegistrationStatusObject->SetBoolField(TEXT("sourcePlannedOrApplied"), bCanApply);
			RegistrationStatusObject->SetBoolField(TEXT("descriptorSourceIntegrated"), PostcheckObject->GetBoolField(TEXT("descriptorSourceIntegrated")));
			RegistrationStatusObject->SetBoolField(TEXT("handlerSourceIntegrated"), PostcheckObject->GetBoolField(TEXT("handlerSourceIntegrated")));
			RegistrationStatusObject->SetBoolField(TEXT("registryPatchIntegrated"), bRegistryPatchIntegrated);
			RegistrationStatusObject->SetBoolField(TEXT("descriptorRegisteredInCurrentSession"), bDescriptorRegisteredNow);
			RegistrationStatusObject->SetBoolField(TEXT("handlerRegisteredInCurrentSession"), bHandlerRegisteredNow);
			RegistrationStatusObject->SetBoolField(TEXT("toolListedInCurrentSession"), bDescriptorRegisteredNow && bToolRegistryLoadedNow && bHandlerRegisteredNow);
			RegistrationStatusObject->SetBoolField(TEXT("registeredUsableNow"), bDescriptorRegisteredNow && bToolRegistryLoadedNow && bHandlerRegisteredNow);
			RegistrationStatusObject->SetBoolField(TEXT("requiresBuildRestartForRuntimeVisibility"), !(bDescriptorRegisteredNow && bToolRegistryLoadedNow && bHandlerRegisteredNow));
			RegistrationStatusObject->SetStringField(TEXT("notRegisteredReason"), NotRegisteredReason);

			TArray<TSharedPtr<FJsonValue>> NextSteps;
			if (bValidatePatches && !bPatchesSafe && !bAllowUnsafePatches)
			{
				AddScaffoldNextStep(NextSteps, TEXT("Validate the failing fragment to see exact static-safety issues."), TEXT("unreal.mcp_validate_cpp_patch"), TEXT("Unsafe or truncated patches are blocked before source integration."));
				AddScaffoldNextStep(NextSteps, TEXT("Patch the failing fragment, then rerun mcp_apply_scaffold with dryRun=true."), TEXT("unreal.mcp_patch_scaffold_patch"), TEXT("Generated tools are not registered until all required fragments are safe."));
			}
			else if (ConflictCount > 0 || MissingAnchorCount > 0)
			{
				AddScaffoldNextStep(NextSteps, TEXT("Inspect scaffold integration points and target diffs."), TEXT("unreal.mcp_apply_scaffold"), TEXT("Conflicts or missing anchors must be resolved before writing source."));
				AddScaffoldNextStep(NextSteps, TEXT("Patch the scaffold fragment that no longer matches current source layout."), TEXT("unreal.mcp_patch_scaffold_patch"), TEXT("The registrar, handler, or dispatcher patch needs to match the current module split."));
			}
			else if (bDryRun)
			{
				AddScaffoldNextStep(NextSteps, TEXT("Apply the scaffold with dryRun=false after reviewing targetDiffs."), TEXT("unreal.mcp_apply_scaffold"), TEXT("Dry run only previews source, registry, and handler changes."));
			}
			else
			{
				AddScaffoldNextStep(NextSteps, TEXT("Build the editor target so the generated descriptor and handler are compiled."), TEXT("unreal.mcp_build_editor"), TEXT("C++ self-extension changes are not visible in the running module until rebuild/restart."));
				AddScaffoldNextStep(NextSteps, TEXT("Restart Unreal Editor and confirm tools/list exposes the new tool."), TEXT("tools/list"), TEXT("The endpoint loads compiled descriptors and handlers at plugin startup."));
				AddScaffoldNextStep(NextSteps, TEXT("Run the generated tool test or category suite after restart."), TEXT("unreal.mcp_run_tool_test"), TEXT("Registration is only useful if the handler executes successfully."));
			}
			AddScaffoldNextStep(NextSteps, TEXT("Verify the final outcome with explicit evidence."), TEXT("unreal.verify_task_outcome"), TEXT("Prevents treating scaffold generation as completed tool integration."));

			TSharedPtr<FJsonObject> BuildRequirementsObject = MakeShared<FJsonObject>();
			BuildRequirementsObject->SetBoolField(TEXT("present"), BuildRequirements.RequiredIncludes.Num() > 0 || BuildRequirements.RequiredModules.Num() > 0 || BuildRequirements.DependsOn.Num() > 0);
			BuildRequirementsObject->SetNumberField(TEXT("requiredIncludeTargets"), BuildRequirements.RequiredIncludes.Num());
			BuildRequirementsObject->SetArrayField(TEXT("requiredModules"), MakeJsonStringArray(BuildRequirements.RequiredModules));
			BuildRequirementsObject->SetArrayField(TEXT("dependsOn"), MakeJsonStringArray(BuildRequirements.DependsOn));
			BuildRequirementsObject->SetArrayField(TEXT("includesPlanned"), BuildRequirementIncludesPlanned);
			BuildRequirementsObject->SetArrayField(TEXT("modulesPlanned"), BuildRequirementModulesPlanned);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
			StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
			StructuredContent->SetNumberField(TEXT("manifestSchemaVersion"), GUnrealMcpExtensionManifestSchemaVersion);
			StructuredContent->SetStringField(TEXT("manifestSchema"), GetUnrealMcpExtensionManifestSchemaName());
			StructuredContent->SetStringField(TEXT("sessionId"), ExtensionSessionId);
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
			StructuredContent->SetBoolField(TEXT("scaffoldFound"), ScaffoldResolution.bFound);
			StructuredContent->SetStringField(TEXT("scaffoldSourceKind"), LexToString(ScaffoldResolution.SourceKind));
			StructuredContent->SetArrayField(TEXT("scaffoldCandidates"), MakeToolsReadCandidateValues(ScaffoldResolution));
			if (!ScaffoldResolution.Warning.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("scaffoldResolutionWarning"), ScaffoldResolution.Warning);
			}
			StructuredContent->SetStringField(TEXT("registrySourcePath"), RegistrySourcePath);
			StructuredContent->SetBoolField(TEXT("registrySourceFound"), RegistryResolution.bFound);
			StructuredContent->SetStringField(TEXT("registrySourceKind"), LexToString(RegistryResolution.SourceKind));
			StructuredContent->SetArrayField(TEXT("registrySourceCandidates"), MakeToolsReadCandidateValues(RegistryResolution));
			if (!RegistryResolution.Warning.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("registryResolutionWarning"), RegistryResolution.Warning);
			}
			StructuredContent->SetStringField(TEXT("category"), Category);
			StructuredContent->SetStringField(TEXT("categorySourceFile"), CategorySourceFile);
			StructuredContent->SetStringField(TEXT("categoryTryExecute"), TryExecuteName);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetObjectField(TEXT("buildRequirements"), BuildRequirementsObject);
			StructuredContent->SetBoolField(TEXT("canApply"), bCanApply);
			StructuredContent->SetBoolField(TEXT("changed"), ChangedFiles.Num() > 0);
			StructuredContent->SetBoolField(TEXT("validatePatches"), bValidatePatches);
			StructuredContent->SetBoolField(TEXT("patchesSafe"), bPatchesSafe);
			StructuredContent->SetBoolField(TEXT("allowUnsafePatches"), bAllowUnsafePatches);
			StructuredContent->SetNumberField(TEXT("conflictCount"), ConflictCount);
			StructuredContent->SetNumberField(TEXT("missingAnchorCount"), MissingAnchorCount);
			StructuredContent->SetObjectField(TEXT("conflictPolicy"), ConflictPolicy);
			StructuredContent->SetArrayField(TEXT("issues"), Issues);
			StructuredContent->SetArrayField(TEXT("patchValidations"), PatchValidations);
			StructuredContent->SetArrayField(TEXT("changes"), Changes);
			StructuredContent->SetArrayField(TEXT("targetDiffs"), TargetDiffs);
			StructuredContent->SetArrayField(TEXT("changedFiles"), ChangedFiles);
			StructuredContent->SetObjectField(TEXT("postcheck"), PostcheckObject);
			StructuredContent->SetObjectField(TEXT("registrationStatus"), RegistrationStatusObject);
			StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);
			StructuredContent->SetBoolField(TEXT("rolledBackOnApplyFailure"), false);

			if (!bCanApply)
			{
				return MakeExecutionResult(TEXT("Descriptor-first scaffold is not registered yet and cannot be applied safely. See registrationStatus, nextSteps, issues, patchValidations, and targetDiffs."), StructuredContent, true);
			}

			const bool bManifestPostcheckOk = bCanApply
				&& ConflictCount == 0
				&& MissingAnchorCount == 0
				&& (!bValidatePatches || bPatchesSafe || bAllowUnsafePatches);

			if (bDryRun)
			{
				WriteManifestActivityEvent(
					TEXT("manifest_dryrun"),
					ToolName,
					ExtensionSessionId,
					FString(),
					FString(),
					false,
					Changes.Num(),
					ChangedFiles.Num(),
					bManifestPostcheckOk);
				return MakeExecutionResult(
					FString::Printf(TEXT("Descriptor-first dry run complete for %s. canApply=true"), *ToolName),
					StructuredContent,
					false);
			}

			if (ChangedFiles.Num() == 0)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("No source changes needed for %s; descriptor-first scaffold appears already integrated."), *ToolName),
					StructuredContent,
					false);
			}

			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BackupDirectory = FPaths::Combine(GetMcpExtensionBackupRoot(), Timestamp + TEXT("_") + SanitizeMcpToolIdForPath(ToolName));
			TArray<TSharedPtr<FJsonValue>> ManifestFiles;
			StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);

			auto ReadAndHashFile = [](const FString& FilePath, FString& OutText, FString& OutHash) -> bool
			{
				if (!FFileHelper::LoadFileToString(OutText, *FilePath))
				{
					OutHash = FString();
					return false;
				}
				OutHash = HashTextForManifest(OutText);
				return true;
			};

			auto MakeOrGetDiskVerification = [](const TSharedPtr<FJsonObject>& FileObject) -> TSharedPtr<FJsonObject>
			{
				if (!FileObject.IsValid())
				{
					return MakeShared<FJsonObject>();
				}

				const TSharedPtr<FJsonObject>* ExistingObject = nullptr;
				if (FileObject->TryGetObjectField(TEXT("diskVerification"), ExistingObject) && ExistingObject && (*ExistingObject).IsValid())
				{
					return *ExistingObject;
				}

				TSharedPtr<FJsonObject> DiskVerificationObject = MakeShared<FJsonObject>();
				FileObject->SetObjectField(TEXT("diskVerification"), DiskVerificationObject);
				return DiskVerificationObject;
			};

			if (!IFileManager::Get().MakeDirectory(*BackupDirectory, true))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to create atomic apply backup directory '%s'."), *BackupDirectory), StructuredContent, true);
			}

			for (const TSharedPtr<FJsonValue>& ChangedFileValue : ChangedFiles)
			{
				TSharedPtr<FJsonObject> ChangedFileObject = ChangedFileValue->AsObject();
				if (!ChangedFileObject.IsValid())
				{
					continue;
				}

				FString SourcePath;
				FString RelativePath;
				ChangedFileObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
				ChangedFileObject->TryGetStringField(TEXT("relativePath"), RelativePath);
				const FString* BeforeText = BeforeTexts.Find(SourcePath);
				const FString* AfterText = PlannedTexts.Find(SourcePath);
				TSharedPtr<FJsonObject> DiskVerificationObject = MakeOrGetDiskVerification(ChangedFileObject);
				DiskVerificationObject->SetStringField(TEXT("status"), TEXT("staging"));

				if (SourcePath.IsEmpty() || RelativePath.IsEmpty() || !BeforeText || !AfterText)
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("plan_lookup"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Missing planned text for changed source file '%s'."), *SourcePath), StructuredContent, true);
				}

				const FString BackupPath = FPaths::Combine(BackupDirectory, RelativePath + TEXT(".before"));
				const FString AfterPath = FPaths::Combine(BackupDirectory, RelativePath + TEXT(".after"));
				const FString TempPath = FPaths::Combine(
					FPaths::GetPath(SourcePath),
					FString::Printf(TEXT(".%s.%s.%d.unrealmcp.tmp"), *FPaths::GetCleanFilename(SourcePath), *Timestamp, ManifestFiles.Num()));
				const FString ExpectedBeforeHash = HashTextForManifest(*BeforeText);
				const FString ExpectedAfterHash = HashTextForManifest(*AfterText);

				ChangedFileObject->SetStringField(TEXT("backupPath"), BackupPath);
				ChangedFileObject->SetStringField(TEXT("afterPath"), AfterPath);
				ChangedFileObject->SetStringField(TEXT("tempPath"), TempPath);
				DiskVerificationObject->SetStringField(TEXT("tempPath"), TempPath);
				DiskVerificationObject->SetStringField(TEXT("backupPath"), BackupPath);
				DiskVerificationObject->SetStringField(TEXT("afterPath"), AfterPath);
				ManifestFiles.Add(MakeShared<FJsonValueObject>(ChangedFileObject));

				if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(BackupPath), true)
					|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(AfterPath), true)
					|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempPath), true))
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("create_staging_directories"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Failed to create atomic apply staging directories for '%s'."), *SourcePath), StructuredContent, true);
				}

				if (!FFileHelper::SaveStringToFile(*AfterText, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("write_temp_after"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Failed to write atomic temp file '%s'."), *TempPath), StructuredContent, true);
				}

				FString TempText;
				FString TempHash;
				const bool bTempReadable = ReadAndHashFile(TempPath, TempText, TempHash);
				DiskVerificationObject->SetStringField(TEXT("tempHash"), TempHash);
				DiskVerificationObject->SetBoolField(TEXT("tempHashMatchesExpectedAfter"), bTempReadable && TempHash == ExpectedAfterHash);
				if (!bTempReadable || TempHash != ExpectedAfterHash)
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("verify_temp_after"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Atomic temp file hash verification failed for '%s'."), *SourcePath), StructuredContent, true);
				}

				if (!FFileHelper::SaveStringToFile(*BeforeText, *BackupPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("write_backup_before"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Failed to write before snapshot '%s'."), *BackupPath), StructuredContent, true);
				}

				FString BackupText;
				FString BackupHash;
				const bool bBackupReadable = ReadAndHashFile(BackupPath, BackupText, BackupHash);
				DiskVerificationObject->SetStringField(TEXT("backupHash"), BackupHash);
				DiskVerificationObject->SetBoolField(TEXT("backupHashMatchesExpectedBefore"), bBackupReadable && BackupHash == ExpectedBeforeHash);
				if (!bBackupReadable || BackupHash != ExpectedBeforeHash)
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("verify_backup_before"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Before snapshot hash verification failed for '%s'."), *SourcePath), StructuredContent, true);
				}

				if (!FFileHelper::SaveStringToFile(*AfterText, *AfterPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("write_after_snapshot"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("Failed to write after snapshot '%s'."), *AfterPath), StructuredContent, true);
				}

				FString AfterSnapshotText;
				FString AfterSnapshotHash;
				const bool bAfterSnapshotReadable = ReadAndHashFile(AfterPath, AfterSnapshotText, AfterSnapshotHash);
				DiskVerificationObject->SetStringField(TEXT("afterSnapshotHash"), AfterSnapshotHash);
				DiskVerificationObject->SetBoolField(TEXT("afterSnapshotHashMatchesExpectedAfter"), bAfterSnapshotReadable && AfterSnapshotHash == ExpectedAfterHash);
				if (!bAfterSnapshotReadable || AfterSnapshotHash != ExpectedAfterHash)
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("verify_after_snapshot"));
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(FString::Printf(TEXT("After snapshot hash verification failed for '%s'."), *SourcePath), StructuredContent, true);
				}

				DiskVerificationObject->SetStringField(TEXT("status"), TEXT("staged"));
			}

			StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
			TArray<TSharedPtr<FJsonObject>> AppliedFileObjects;
			auto RollbackAppliedFiles = [&StructuredContent, &ReadAndHashFile, &MakeOrGetDiskVerification](const TArray<TSharedPtr<FJsonObject>>& FilesToRollback, FString& OutRollbackFailure) -> bool
			{
				bool bAllRollbackSucceeded = true;
				for (int32 FileIndex = FilesToRollback.Num() - 1; FileIndex >= 0; --FileIndex)
				{
					TSharedPtr<FJsonObject> FileObject = FilesToRollback[FileIndex];
					if (!FileObject.IsValid())
					{
						continue;
					}

					FString SourcePath;
					FString BackupPath;
					FString ExpectedBeforeHash;
					FileObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
					FileObject->TryGetStringField(TEXT("backupPath"), BackupPath);
					FileObject->TryGetStringField(TEXT("hashBefore"), ExpectedBeforeHash);
					TSharedPtr<FJsonObject> DiskVerificationObject = MakeOrGetDiskVerification(FileObject);
					DiskVerificationObject->SetBoolField(TEXT("rollbackAttempted"), true);

					FString BackupText;
					if (SourcePath.IsEmpty() || BackupPath.IsEmpty() || !FFileHelper::LoadFileToString(BackupText, *BackupPath))
					{
						bAllRollbackSucceeded = false;
						DiskVerificationObject->SetStringField(TEXT("status"), TEXT("rollback_failed"));
						DiskVerificationObject->SetBoolField(TEXT("verifiedOnDisk"), false);
						DiskVerificationObject->SetBoolField(TEXT("rollbackSucceeded"), false);
						DiskVerificationObject->SetStringField(TEXT("rollbackFailure"), FString::Printf(TEXT("Failed to read before snapshot '%s'."), *BackupPath));
						if (OutRollbackFailure.IsEmpty())
						{
							OutRollbackFailure = FString::Printf(TEXT("Failed to read before snapshot '%s'."), *BackupPath);
						}
						continue;
					}

					if (!FFileHelper::SaveStringToFile(BackupText, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						bAllRollbackSucceeded = false;
						DiskVerificationObject->SetStringField(TEXT("status"), TEXT("rollback_failed"));
						DiskVerificationObject->SetBoolField(TEXT("verifiedOnDisk"), false);
						DiskVerificationObject->SetBoolField(TEXT("rollbackSucceeded"), false);
						DiskVerificationObject->SetStringField(TEXT("rollbackFailure"), FString::Printf(TEXT("Failed to restore source file '%s'."), *SourcePath));
						if (OutRollbackFailure.IsEmpty())
						{
							OutRollbackFailure = FString::Printf(TEXT("Failed to restore source file '%s'."), *SourcePath);
						}
						continue;
					}

					FString RestoredText;
					FString RestoredHash;
					const bool bRestoredReadable = ReadAndHashFile(SourcePath, RestoredText, RestoredHash);
					const bool bRestoredHashMatches = bRestoredReadable && (ExpectedBeforeHash.IsEmpty() || RestoredHash == ExpectedBeforeHash);
					DiskVerificationObject->SetStringField(TEXT("rollbackHash"), RestoredHash);
					DiskVerificationObject->SetStringField(TEXT("finalDiskHash"), RestoredHash);
					DiskVerificationObject->SetBoolField(TEXT("rollbackHashMatchesExpectedBefore"), bRestoredHashMatches);
					DiskVerificationObject->SetBoolField(TEXT("finalDiskHashMatchesExpectedBefore"), bRestoredHashMatches);
					DiskVerificationObject->SetBoolField(TEXT("verifiedOnDisk"), false);
					DiskVerificationObject->SetBoolField(TEXT("rollbackSucceeded"), bRestoredHashMatches);
					DiskVerificationObject->SetStringField(TEXT("status"), bRestoredHashMatches ? TEXT("rolled_back") : TEXT("rollback_failed"));
					if (!bRestoredHashMatches)
					{
						bAllRollbackSucceeded = false;
						DiskVerificationObject->SetStringField(TEXT("rollbackFailure"), FString::Printf(TEXT("Restored source hash verification failed for '%s'."), *SourcePath));
						if (OutRollbackFailure.IsEmpty())
						{
							OutRollbackFailure = FString::Printf(TEXT("Restored source hash verification failed for '%s'."), *SourcePath);
						}
					}
				}

				StructuredContent->SetBoolField(TEXT("rolledBackOnApplyFailure"), true);
				StructuredContent->SetBoolField(TEXT("rollbackSucceeded"), bAllRollbackSucceeded);
				return bAllRollbackSucceeded;
			};

			for (const TSharedPtr<FJsonValue>& ChangedFileValue : ChangedFiles)
			{
				TSharedPtr<FJsonObject> ChangedFileObject = ChangedFileValue->AsObject();
				if (!ChangedFileObject.IsValid())
				{
					continue;
				}

				FString SourcePath;
				FString TempPath;
				FString ExpectedBeforeHash;
				FString ExpectedAfterHash;
				ChangedFileObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
				ChangedFileObject->TryGetStringField(TEXT("tempPath"), TempPath);
				ChangedFileObject->TryGetStringField(TEXT("hashBefore"), ExpectedBeforeHash);
				ChangedFileObject->TryGetStringField(TEXT("hashAfter"), ExpectedAfterHash);
				TSharedPtr<FJsonObject> DiskVerificationObject = MakeOrGetDiskVerification(ChangedFileObject);
				DiskVerificationObject->SetStringField(TEXT("status"), TEXT("replacing"));

				if (SourcePath.IsEmpty() || TempPath.IsEmpty())
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("replace_target"));
					FString RollbackFailure;
					const bool bHadAppliedFiles = AppliedFileObjects.Num() > 0;
					const bool bRollbackSucceeded = !bHadAppliedFiles || RollbackAppliedFiles(AppliedFileObjects, RollbackFailure);
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(
						bHadAppliedFiles
							? (bRollbackSucceeded
								? FString::Printf(TEXT("Failed to atomically replace source file '%s'. Previously written files were restored from .before snapshots."), *SourcePath)
								: FString::Printf(TEXT("Failed to atomically replace source file '%s', and rollback was incomplete: %s"), *SourcePath, *RollbackFailure))
							: FString::Printf(TEXT("Failed to atomically replace source file '%s'. No source files had been replaced yet."), *SourcePath),
						StructuredContent,
						true);
				}

				FString DiskBeforeText;
				FString DiskBeforeHash;
				const bool bDiskBeforeReadable = ReadAndHashFile(SourcePath, DiskBeforeText, DiskBeforeHash);
				const bool bDiskBeforeHashMatches = bDiskBeforeReadable && DiskBeforeHash == ExpectedBeforeHash;
				DiskVerificationObject->SetStringField(TEXT("diskHashBefore"), DiskBeforeHash);
				DiskVerificationObject->SetBoolField(TEXT("diskHashBeforeMatchesExpected"), bDiskBeforeHashMatches);
				if (!bDiskBeforeHashMatches)
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("verify_disk_before"));
					FString RollbackFailure;
					const bool bHadAppliedFiles = AppliedFileObjects.Num() > 0;
					const bool bRollbackSucceeded = !bHadAppliedFiles || RollbackAppliedFiles(AppliedFileObjects, RollbackFailure);
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(
						bHadAppliedFiles
							? (bRollbackSucceeded
								? FString::Printf(TEXT("Disk hash verification failed before replacing '%s'. Previously written files were restored from .before snapshots."), *SourcePath)
								: FString::Printf(TEXT("Disk hash verification failed before replacing '%s', and rollback was incomplete: %s"), *SourcePath, *RollbackFailure))
							: FString::Printf(TEXT("Disk hash verification failed before replacing '%s'. No source files had been replaced yet."), *SourcePath),
						StructuredContent,
						true);
				}

				DiskVerificationObject->SetStringField(TEXT("replaceMethod"), TEXT("platform_move_file"));
				if (!FPlatformFileManager::Get().GetPlatformFile().MoveFile(*SourcePath, *TempPath))
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("replace_target"));
					FString FailedReplaceDiskText;
					FString FailedReplaceDiskHash;
					const bool bFailedReplaceTargetReadable = ReadAndHashFile(SourcePath, FailedReplaceDiskText, FailedReplaceDiskHash);
					const bool bFailedReplaceTargetStillBefore = bFailedReplaceTargetReadable && FailedReplaceDiskHash == ExpectedBeforeHash;
					DiskVerificationObject->SetStringField(TEXT("diskHashAfterFailedReplace"), FailedReplaceDiskHash);
					DiskVerificationObject->SetBoolField(TEXT("targetStillMatchesExpectedBeforeAfterFailedReplace"), bFailedReplaceTargetStillBefore);

					TArray<TSharedPtr<FJsonObject>> RollbackFileObjects = AppliedFileObjects;
					if (!bFailedReplaceTargetStillBefore)
					{
						RollbackFileObjects.Add(ChangedFileObject);
					}

					FString RollbackFailure;
					const bool bHadRollbackTargets = RollbackFileObjects.Num() > 0;
					const bool bRollbackSucceeded = !bHadRollbackTargets || RollbackAppliedFiles(RollbackFileObjects, RollbackFailure);
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(
						bHadRollbackTargets
							? (bRollbackSucceeded
								? FString::Printf(TEXT("Failed to atomically replace source file '%s'. Written files were restored from .before snapshots."), *SourcePath)
								: FString::Printf(TEXT("Failed to atomically replace source file '%s', and rollback was incomplete: %s"), *SourcePath, *RollbackFailure))
							: FString::Printf(TEXT("Failed to atomically replace source file '%s'. No source files had been replaced yet."), *SourcePath),
						StructuredContent,
						true);
				}

				AppliedFileObjects.Add(ChangedFileObject);
				DiskVerificationObject->SetBoolField(TEXT("targetReplaceSucceeded"), true);

				FString DiskText;
				FString DiskHash;
				const bool bDiskReadable = ReadAndHashFile(SourcePath, DiskText, DiskHash);
				const bool bDiskHashMatches = bDiskReadable && DiskHash == ExpectedAfterHash;
				DiskVerificationObject->SetStringField(TEXT("diskHashAfter"), DiskHash);
				DiskVerificationObject->SetBoolField(TEXT("diskHashAfterMatchesExpected"), bDiskHashMatches);
				DiskVerificationObject->SetBoolField(TEXT("verifiedOnDisk"), bDiskHashMatches);
				if (!bDiskHashMatches)
				{
					DiskVerificationObject->SetStringField(TEXT("status"), TEXT("failed"));
					DiskVerificationObject->SetStringField(TEXT("failureStage"), TEXT("verify_disk_after"));
					FString RollbackFailure;
					const bool bRollbackSucceeded = RollbackAppliedFiles(AppliedFileObjects, RollbackFailure);
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(
						bRollbackSucceeded
							? FString::Printf(TEXT("Disk hash verification failed after replacing '%s'. Written files were restored from .before snapshots."), *SourcePath)
							: FString::Printf(TEXT("Disk hash verification failed after replacing '%s', and rollback was incomplete: %s"), *SourcePath, *RollbackFailure),
						StructuredContent,
						true);
				}

				DiskVerificationObject->SetStringField(TEXT("status"), TEXT("verified"));
			}

			StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
			FString ManifestPath;
			FString LatestManifestPath;

			TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
			ManifestObject->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
			ManifestObject->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
			ManifestObject->SetNumberField(TEXT("schemaVersion"), GUnrealMcpExtensionManifestSchemaVersion);
			ManifestObject->SetStringField(TEXT("manifestSchema"), GetUnrealMcpExtensionManifestSchemaName());
			ManifestObject->SetStringField(TEXT("sessionId"), ExtensionSessionId);
			ManifestObject->SetStringField(TEXT("toolName"), ToolName);
			ManifestObject->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			ManifestObject->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
			ManifestObject->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			ManifestObject->SetStringField(TEXT("appliedAtUtc"), FDateTime::UtcNow().ToIso8601());
			ManifestObject->SetNumberField(TEXT("conflictCount"), ConflictCount);
			ManifestObject->SetNumberField(TEXT("missingAnchorCount"), MissingAnchorCount);
			ManifestObject->SetObjectField(TEXT("conflictPolicy"), ConflictPolicy);
			ManifestObject->SetArrayField(TEXT("changes"), Changes);
			ManifestObject->SetArrayField(TEXT("files"), ManifestFiles);
			ManifestObject->SetObjectField(TEXT("postcheck"), PostcheckObject);
			ManifestObject->SetBoolField(TEXT("rolledBackOnApplyFailure"), false);

			if (bCreateBackup)
			{
				FString ManifestFailure;
				ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				LatestManifestPath = GetLatestMcpExtensionManifestPath();
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure)
					|| !SaveJsonObjectToFile(ManifestObject, LatestManifestPath, ManifestFailure))
				{
					FString RollbackFailure;
					const bool bRollbackSucceeded = RollbackAppliedFiles(AppliedFileObjects, RollbackFailure);
					StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);
					return MakeExecutionResult(
						bRollbackSucceeded
							? FString::Printf(TEXT("%s Written files were restored from .before snapshots."), *ManifestFailure)
							: FString::Printf(TEXT("%s Rollback was incomplete: %s"), *ManifestFailure, *RollbackFailure),
						StructuredContent,
						true);
				}
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
				StructuredContent->SetStringField(TEXT("latestManifestPath"), LatestManifestPath);
			}

			WriteManifestActivityEvent(
				TEXT("manifest_apply"),
				ToolName,
				ExtensionSessionId,
				ManifestPath,
				BackupDirectory,
				true,
				Changes.Num(),
				ChangedFiles.Num(),
				bManifestPostcheckOk);

			return MakeExecutionResult(
				FString::Printf(TEXT("Applied descriptor-first scaffold for %s. Backup: %s"), *ToolName, *BackupDirectory),
				StructuredContent,
				false);
		}

		bool ApplyResolvedScaffold(const FScaffoldApplyContext& Ctx, FApplyResult& OutResult)
		{
			OutResult = FApplyResult();
			OutResult.ExecutionResult = ApplyResolvedScaffoldExecution(Ctx);
			OutResult.StructuredContent = OutResult.ExecutionResult.StructuredContent;
			return !OutResult.ExecutionResult.bIsError;
		}

		FUnrealMcpExecutionResult ApplyMcpScaffold(const FJsonObject& Arguments)
		{
			FString ScaffoldDirectory;
			FString ToolName;
			FString FailureReason;
			FToolsReadResolution RootScaffoldResolution;
			if (!ResolveMcpScaffoldDirectory(Arguments, ScaffoldDirectory, ToolName, FailureReason, &RootScaffoldResolution))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			bool bDryRun = true;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			FResolvedScaffoldDependency RootNode;
			RootNode.ToolName = ToolName;
			RootNode.ToolId = SanitizeMcpToolIdForPath(ToolName);
			RootNode.ScaffoldDirectory = NormalizeScaffoldDirectoryForDependency(ScaffoldDirectory);
			FString RootMetadataToolName;
			FString RootMetadataToolId;
			ReadScaffoldDependencyIdentity(RootNode.ScaffoldDirectory, RootMetadataToolName, RootMetadataToolId);
			if (!RootMetadataToolId.IsEmpty())
			{
				RootNode.ToolId = RootMetadataToolId;
			}
			if (!RootMetadataToolName.IsEmpty() && !RootMetadataToolName.Equals(RootNode.ToolName, ESearchCase::CaseSensitive))
			{
				RootNode.Aliases.Add(RootMetadataToolName);
			}

			TArray<FString> RootDependsOn;
			ReadScaffoldDependsOn(RootNode.ScaffoldDirectory, RootDependsOn);
			if (RootDependsOn.Num() == 0)
			{
				FApplyResult SingleResult;
				FScaffoldApplyContext SingleCtx;
				SingleCtx.Arguments = &Arguments;
				SingleCtx.ScaffoldDirectory = RootNode.ScaffoldDirectory;
				SingleCtx.ScaffoldResolution = RootScaffoldResolution;
				SingleCtx.ToolName = ToolName;
				ApplyResolvedScaffold(SingleCtx, SingleResult);
				return SingleResult.ExecutionResult;
			}

			FString DependencyRoot;
			if (!ResolveDependencyScaffoldRoot(Arguments, ScaffoldDirectory, DependencyRoot, FailureReason))
			{
				TArray<TSharedPtr<FJsonValue>> Issues;
				Issues.Add(MakeShared<FJsonValueObject>(MakeDependencyIssue(TEXT("dependency_root_invalid"), FailureReason)));
					return MakeExecutionResult(
						FailureReason,
						MakeDependencyFailureContent(ToolName, ScaffoldDirectory, RootScaffoldResolution, DependencyRoot, bDryRun, FailureReason, Issues, TArray<TSharedPtr<FJsonValue>>()),
						true);
			}

			TArray<FResolvedScaffoldDependency> ApplyOrder;
			TArray<TSharedPtr<FJsonValue>> DependencyIssues;
			if (!ResolveScaffoldDependencyOrder(RootNode, DependencyRoot, ApplyOrder, DependencyIssues, FailureReason))
			{
					return MakeExecutionResult(
						FailureReason,
						MakeDependencyFailureContent(ToolName, RootNode.ScaffoldDirectory, RootScaffoldResolution, DependencyRoot, bDryRun, FailureReason, DependencyIssues, TArray<TSharedPtr<FJsonValue>>()),
						true);
			}

			TArray<TSharedPtr<FJsonValue>> DependencyChain;
			bool bChainCanApply = true;
			FApplyResult LastResult;
			for (const FResolvedScaffoldDependency& Node : ApplyOrder)
			{
				FScaffoldApplyContext NodeCtx;
				NodeCtx.Arguments = &Arguments;
				NodeCtx.ScaffoldDirectory = Node.ScaffoldDirectory;
				NodeCtx.ScaffoldResolution.Path = Node.ScaffoldDirectory;
				NodeCtx.ScaffoldResolution.bFound = FPaths::DirectoryExists(Node.ScaffoldDirectory);
				NodeCtx.ScaffoldResolution.SourceKind = RootScaffoldResolution.SourceKind;
				NodeCtx.ScaffoldResolution.Candidates.Add(Node.ScaffoldDirectory);
				NodeCtx.ToolName = Node.ToolName;

				FApplyResult NodeResult;
				ApplyResolvedScaffold(NodeCtx, NodeResult);
				const bool bNodeCanApply = TryGetJsonBoolField(NodeResult.StructuredContent, TEXT("canApply"), !NodeResult.ExecutionResult.bIsError);
				bChainCanApply &= bNodeCanApply;
				DependencyChain.Add(MakeShared<FJsonValueObject>(MakeDependencyChainEntry(Node, NodeResult, bDryRun)));

				if (NodeResult.ExecutionResult.bIsError)
				{
					if (!NodeResult.StructuredContent.IsValid())
					{
						TArray<TSharedPtr<FJsonValue>> EmptyIssues;
							NodeResult.StructuredContent = MakeDependencyFailureContent(
								ToolName,
								RootNode.ScaffoldDirectory,
								RootScaffoldResolution,
								DependencyRoot,
							bDryRun,
							NodeResult.ExecutionResult.Text,
							EmptyIssues,
							DependencyChain);
					}

					NodeResult.StructuredContent->SetBoolField(TEXT("canApply"), false);
					NodeResult.StructuredContent->SetArrayField(TEXT("dependencyChain"), DependencyChain);

					TSharedPtr<FJsonObject> DependencyFailureObject = MakeShared<FJsonObject>();
					DependencyFailureObject->SetStringField(TEXT("toolName"), Node.ToolName);
					DependencyFailureObject->SetStringField(TEXT("scaffoldDir"), Node.ScaffoldDirectory);
					DependencyFailureObject->SetStringField(TEXT("summary"), NodeResult.ExecutionResult.Text);
					DependencyFailureObject->SetNumberField(TEXT("appliedBeforeFailure"), DependencyChain.Num() - 1);
					NodeResult.StructuredContent->SetObjectField(TEXT("dependencyFailure"), DependencyFailureObject);

					return MakeExecutionResult(
						FString::Printf(TEXT("Dependency apply failed at %s: %s"), *Node.ToolName, *NodeResult.ExecutionResult.Text),
						NodeResult.StructuredContent,
						true);
				}

				LastResult = NodeResult;
			}

			if (LastResult.StructuredContent.IsValid())
			{
				LastResult.StructuredContent->SetArrayField(TEXT("dependencyChain"), DependencyChain);
				LastResult.StructuredContent->SetBoolField(TEXT("canApply"), bChainCanApply);
			}

			LastResult.ExecutionResult.Text = bDryRun
				? FString::Printf(TEXT("Descriptor-first dependency dry run complete for %s. canApply=%s, scaffolds=%d"), *ToolName, bChainCanApply ? TEXT("true") : TEXT("false"), ApplyOrder.Num())
				: FString::Printf(TEXT("Applied descriptor-first dependency chain for %s. scaffolds=%d"), *ToolName, ApplyOrder.Num());
			return LastResult.ExecutionResult;
		}

}
