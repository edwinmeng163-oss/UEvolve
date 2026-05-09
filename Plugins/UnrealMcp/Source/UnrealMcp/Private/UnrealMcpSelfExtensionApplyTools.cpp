#include "UnrealMcpSelfExtensionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistrar.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FString HashTextForManifest(const FString& Text);
	FString GetMcpModuleSourcePath();
	FString GetMcpExtensionBackupRoot();
	FString GetLatestMcpExtensionManifestPath();
	FString GetMcpExtensionLockPath();
	FString SanitizeMcpToolIdForPath(const FString& ToolName);
	bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason);
	bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason);
	bool ResolveMcpScaffoldDirectory(const FJsonObject& Arguments, FString& OutDirectory, FString& OutToolName, FString& OutFailureReason);
	bool LoadScaffoldSnippet(
		const FString& ScaffoldDirectory,
		const FString& FileName,
		bool bRequired,
		FString& OutSnippet,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutFailureReason);
	TSharedPtr<FJsonObject> ValidateCppSnippetText(
		const FString& SnippetText,
		const FString& SnippetName,
		const FString& ToolName);
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

		FString MakeApplySourcePath(const FString& RelativePrivateSourceFile)
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Plugins/UnrealMcp/Source/UnrealMcp/Private"),
				RelativePrivateSourceFile));
		}

		FString GetToolRegistryMirrorPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Plugins/UnrealMcp/Resources/ToolRegistry/tools.json")));
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

		FString MakeApplyRelativePath(const FString& Path)
		{
			FString RelativePath = FPaths::ConvertRelativePathToFull(Path);
			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeFilename(RelativePath);
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::MakePathRelativeTo(RelativePath, *ProjectDir);
			return RelativePath;
		}

		FString FindTryExecuteFirstIfAnchor(const FString& SourceText, const FString& TryExecuteName)
		{
			const int32 FunctionOffset = SourceText.Find(FString::Printf(TEXT("bool %s("), *TryExecuteName), ESearchCase::CaseSensitive);
			if (FunctionOffset == INDEX_NONE)
			{
				return FString();
			}

			const int32 BodyOffset = SourceText.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FunctionOffset);
			if (BodyOffset == INDEX_NONE)
			{
				return FString();
			}

			const int32 FirstIfOffset = SourceText.Find(TEXT("\n\t\tif ("), ESearchCase::CaseSensitive, ESearchDir::FromStart, BodyOffset);
			if (FirstIfOffset == INDEX_NONE)
			{
				return FString();
			}

			const int32 LineEndOffset = SourceText.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstIfOffset + 1);
			if (LineEndOffset == INDEX_NONE)
			{
				return SourceText.Mid(FirstIfOffset);
			}
			return SourceText.Mid(FirstIfOffset, LineEndOffset - FirstIfOffset);
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
			const FString& ConflictSourceText,
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

			if (!ConflictNeedle.IsEmpty() && ConflictSourceText.Contains(ConflictNeedle, ESearchCase::CaseSensitive))
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

		FUnrealMcpExecutionResult ApplyMcpScaffold(const FJsonObject& Arguments)
		{
			FString ScaffoldDirectory;
			FString ToolName;
			FString FailureReason;
			if (!ResolveMcpScaffoldDirectory(Arguments, ScaffoldDirectory, ToolName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

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
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
				StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				StructuredContent->SetArrayField(TEXT("issues"), Issues);
				return MakeExecutionResult(FailureReason, StructuredContent, true);
			}

			const FString RegistryPatchPath = FPaths::Combine(ScaffoldDirectory, TEXT("ToolRegistryPatch.json"));
			FString RegistryPatchText;
			if (!FFileHelper::LoadFileToString(RegistryPatchText, *RegistryPatchPath))
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Failed to read ToolRegistry patch file '%s'."), *RegistryPatchPath),
					nullptr,
					true);
			}

			TSharedPtr<FJsonObject> RegistryPatchObject;
			if (!LoadJsonObjectFromFile(RegistryPatchPath, RegistryPatchObject, FailureReason) || !RegistryPatchObject.IsValid())
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
				StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				StructuredContent->SetStringField(TEXT("registryPatchPath"), RegistryPatchPath);
				StructuredContent->SetArrayField(TEXT("issues"), Issues);
				return MakeExecutionResult(FailureReason, StructuredContent, true);
			}

			FString RegistryToolName;
			FString Category;
			RegistryPatchObject->TryGetStringField(TEXT("name"), RegistryToolName);
			RegistryPatchObject->TryGetStringField(TEXT("category"), Category);
			if (!RegistryToolName.IsEmpty() && !RegistryToolName.Equals(ToolName, ESearchCase::CaseSensitive))
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("ToolRegistryPatch.json name '%s' does not match requested tool '%s'."), *RegistryToolName, *ToolName),
					nullptr,
					true);
			}
			if (Category.TrimStartAndEnd().IsEmpty())
			{
				Category = TEXT("self-extension");
			}

			FString CategorySourceFile;
			FString TryExecuteName;
			TSharedPtr<FJsonObject> MetadataObject;
			if (LoadJsonObjectFromFile(FPaths::Combine(ScaffoldDirectory, TEXT("ScaffoldMetadata.json")), MetadataObject, FailureReason) && MetadataObject.IsValid())
			{
				MetadataObject->TryGetStringField(TEXT("categorySourceFile"), CategorySourceFile);
				MetadataObject->TryGetStringField(TEXT("categoryTryExecute"), TryExecuteName);
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

			const FString RegistrarPath = MakeApplySourcePath(TEXT("UnrealMcpToolRegistrar.cpp"));
			const FString CategorySourcePath = MakeApplySourcePath(CategorySourceFile);
			const FString ModuleSourcePath = GetMcpModuleSourcePath();
			const FString RegistrySourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpToolRegistry/tools.json")));
			const FString RegistryMirrorPath = GetToolRegistryMirrorPath();

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

			TArray<TSharedPtr<FJsonValue>> Changes;
			bool bChanged = false;
			bool bCanApply = true;
			const FString GeneratedFunctionSuffix = MakeMcpGeneratedFunctionSuffixForApply(ToolName);

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

			const FString DispatcherAnchor = FindTryExecuteFirstIfAnchor(CategoryBefore, TryExecuteName);
			if (DispatcherAnchor.IsEmpty())
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("CategoryDispatcherBranch"),
					EMcpScaffoldInsertionStatus::MissingAnchor,
					FString::Printf(TEXT("Could not find first if branch in %s."), *TryExecuteName),
					INDEX_NONE,
					CategoryDispatcherPatch.Left(800))));
				bCanApply = false;
			}
			else
			{
				bCanApply &= PlanOrApplyPatchInsertion(
					CategoryText,
					CategoryBefore,
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
				FileObject->SetStringField(TEXT("sourcePath"), Pair.Key);
				FileObject->SetStringField(TEXT("relativePath"), MakeApplyRelativePath(Pair.Key));
				FileObject->SetStringField(TEXT("hashBefore"), HashTextForManifest(Pair.Value));
				FileObject->SetStringField(TEXT("hashAfter"), HashTextForManifest(*PlannedText));
				FileObject->SetBoolField(TEXT("changed"), Pair.Value != *PlannedText);
				FileObject->SetObjectField(TEXT("diff"), MakeTextDiffObject(Pair.Value, *PlannedText, TargetDiffPreviewLines));
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

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
			StructuredContent->SetStringField(TEXT("applyMode"), TEXT("descriptor_first"));
			StructuredContent->SetNumberField(TEXT("manifestSchemaVersion"), GUnrealMcpExtensionManifestSchemaVersion);
			StructuredContent->SetStringField(TEXT("manifestSchema"), GetUnrealMcpExtensionManifestSchemaName());
			StructuredContent->SetStringField(TEXT("sessionId"), ExtensionSessionId);
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
			StructuredContent->SetStringField(TEXT("category"), Category);
			StructuredContent->SetStringField(TEXT("categorySourceFile"), CategorySourceFile);
			StructuredContent->SetStringField(TEXT("categoryTryExecute"), TryExecuteName);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
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

			if (!bCanApply)
			{
				return MakeExecutionResult(TEXT("Descriptor-first scaffold is not registered yet and cannot be applied safely. See registrationStatus, nextSteps, issues, patchValidations, and targetDiffs."), StructuredContent, true);
			}

			if (bDryRun)
			{
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
			if (bCreateBackup)
			{
				if (!IFileManager::Get().MakeDirectory(*BackupDirectory, true))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to create backup directory '%s'."), *BackupDirectory), StructuredContent, true);
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
					const FString BackupPath = FPaths::Combine(BackupDirectory, RelativePath + TEXT(".before"));
					const FString AfterPath = FPaths::Combine(BackupDirectory, RelativePath + TEXT(".after"));
					if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(BackupPath), true)
						|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(AfterPath), true))
					{
						return MakeExecutionResult(FString::Printf(TEXT("Failed to create backup directories for '%s'."), *SourcePath), StructuredContent, true);
					}
					const FString BeforeText = BeforeTexts.FindChecked(SourcePath);
					const FString AfterText = PlannedTexts.FindChecked(SourcePath);
					if (!FFileHelper::SaveStringToFile(BeforeText, *BackupPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
						|| !FFileHelper::SaveStringToFile(AfterText, *AfterPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						return MakeExecutionResult(FString::Printf(TEXT("Failed to write backup snapshots for '%s'."), *SourcePath), StructuredContent, true);
					}
					ChangedFileObject->SetStringField(TEXT("backupPath"), BackupPath);
					ChangedFileObject->SetStringField(TEXT("afterPath"), AfterPath);
					ManifestFiles.Add(MakeShared<FJsonValueObject>(ChangedFileObject));
				}
			}

			for (const TSharedPtr<FJsonValue>& ChangedFileValue : ChangedFiles)
			{
				TSharedPtr<FJsonObject> ChangedFileObject = ChangedFileValue->AsObject();
				if (!ChangedFileObject.IsValid())
				{
					continue;
				}
				FString SourcePath;
				ChangedFileObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
				const FString* PlannedText = PlannedTexts.Find(SourcePath);
				if (!PlannedText || !FFileHelper::SaveStringToFile(*PlannedText, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to write source file '%s'."), *SourcePath), StructuredContent, true);
				}
			}

			StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			StructuredContent->SetArrayField(TEXT("manifestFiles"), ManifestFiles);

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

			if (bCreateBackup)
			{
				FString ManifestFailure;
				const FString ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure)
					|| !SaveJsonObjectToFile(ManifestObject, GetLatestMcpExtensionManifestPath(), ManifestFailure))
				{
					return MakeExecutionResult(ManifestFailure, StructuredContent, true);
				}
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
				StructuredContent->SetStringField(TEXT("latestManifestPath"), GetLatestMcpExtensionManifestPath());
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Applied descriptor-first scaffold for %s. Backup: %s"), *ToolName, *BackupDirectory),
				StructuredContent,
				false);
		}

}
