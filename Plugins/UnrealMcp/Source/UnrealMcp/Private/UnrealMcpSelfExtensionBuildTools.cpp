#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpSharedPathResolver.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult ProjectMemoryWrite(const FJsonObject& Arguments);

		FString GetHostBuildPlatformName()
		{
#if PLATFORM_MAC
			return TEXT("Mac");
#elif PLATFORM_WINDOWS
			return TEXT("Win64");
#elif PLATFORM_LINUX
			return TEXT("Linux");
#else
			return FPlatformProperties::PlatformName();
#endif
		}

		FString GetUnrealBuildScriptPath()
		{
			const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
#if PLATFORM_MAC
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Mac/Build.sh"));
#elif PLATFORM_WINDOWS
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Build.bat"));
#elif PLATFORM_LINUX
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Linux/Build.sh"));
#else
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Build.sh"));
#endif
		}

		FString GetUnrealAutomationScriptPath()
		{
			const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
#if PLATFORM_WINDOWS
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/RunUAT.bat"));
#else
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/RunUAT.sh"));
#endif
		}

		FString QuoteCommandLineArgument(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}

		bool SanitizeBuildExtraArgs(const FString& InExtraArgs, FString& OutExtraArgs, FString& OutFailureReason)
		{
			OutExtraArgs = InExtraArgs.TrimStartAndEnd();

			TArray<FString> DisallowedCharacters;
			for (const TCHAR Character : OutExtraArgs)
			{
				if (Character == TEXT('\n'))
				{
					DisallowedCharacters.AddUnique(TEXT("\\n"));
					continue;
				}

				if (Character == TEXT('\r'))
				{
					DisallowedCharacters.AddUnique(TEXT("\\r"));
					continue;
				}

				if (FCString::Strchr(TEXT(";|&`$()><"), Character) != nullptr)
				{
					DisallowedCharacters.AddUnique(FString::Printf(TEXT("'%c'"), Character));
				}
			}

			if (DisallowedCharacters.Num() > 0)
			{
				OutFailureReason = FString::Printf(
					TEXT("extraArgs contains disallowed shell metacharacters: %s"),
					*FString::Join(DisallowedCharacters, TEXT(", ")));
				return false;
			}

			return true;
		}

		bool LooksLikeBuildErrorLine(const FString& Line)
		{
			const FString Lower = Line.ToLower();
			return Lower.Contains(TEXT(": error"))
				|| Lower.Contains(TEXT(" error c"))
				|| Lower.Contains(TEXT("fatal error"))
				|| Lower.Contains(TEXT(" error:"))
				|| Lower.Contains(TEXT("error:"))
				|| Lower.Contains(TEXT("error "));
		}

		bool LooksLikeImportantBuildLine(const FString& Line)
		{
			const FString Lower = Line.ToLower();
			return LooksLikeBuildErrorLine(Line)
				|| Lower.Contains(TEXT("warning"))
				|| Lower.Contains(TEXT("result:"))
				|| Lower.Contains(TEXT("total execution time"))
				|| Lower.Contains(TEXT("failed"))
				|| Lower.Contains(TEXT("succeeded"));
		}

		TSharedPtr<FJsonObject> MakeBuildLineObject(const FString& Line)
		{
			TSharedPtr<FJsonObject> LineObject = MakeShared<FJsonObject>();
			LineObject->SetStringField(TEXT("raw"), Line.TrimStartAndEnd());

			FString File;
			int32 LineNumber = 0;
			FString Message = Line.TrimStartAndEnd();

			const int32 MsvcMarker = Line.Find(TEXT("): error"), ESearchCase::IgnoreCase);
			if (MsvcMarker != INDEX_NONE)
			{
				const FString Prefix = Line.Left(MsvcMarker);
				const int32 OpenParen = Prefix.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (OpenParen != INDEX_NONE)
				{
					File = Prefix.Left(OpenParen);
					const FString LinePart = Prefix.Mid(OpenParen + 1);
					LexTryParseString(LineNumber, *LinePart);
					Message = Line.Mid(MsvcMarker + 3).TrimStartAndEnd();
				}
			}

			if (File.IsEmpty())
			{
				int32 ErrorMarker = Line.Find(TEXT(": error"), ESearchCase::IgnoreCase);
				if (ErrorMarker == INDEX_NONE)
				{
					ErrorMarker = Line.Find(TEXT(": fatal error"), ESearchCase::IgnoreCase);
				}
				if (ErrorMarker != INDEX_NONE)
				{
					const FString Prefix = Line.Left(ErrorMarker);
					Message = Line.Mid(ErrorMarker + 2).TrimStartAndEnd();

					const int32 LastColon = Prefix.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (LastColon != INDEX_NONE)
					{
						const FString MaybeLine = Prefix.Mid(LastColon + 1);
						if (LexTryParseString(LineNumber, *MaybeLine))
						{
							File = Prefix.Left(LastColon);
						}
						else
						{
							File = Prefix;
						}
					}
					else
					{
						File = Prefix;
					}
				}
			}

			if (!File.TrimStartAndEnd().IsEmpty())
			{
				LineObject->SetStringField(TEXT("file"), File.TrimStartAndEnd());
			}
			if (LineNumber > 0)
			{
				LineObject->SetNumberField(TEXT("line"), LineNumber);
			}
			LineObject->SetStringField(TEXT("message"), Message);
			return LineObject;
		}

		void ParseBuildLog(const FString& LogText, int32 ReturnCode, const TSharedPtr<FJsonObject>& StructuredContent)
		{
			TArray<FString> Lines;
			LogText.ParseIntoArrayLines(Lines, false);

			TArray<TSharedPtr<FJsonValue>> ErrorLines;
			TArray<TSharedPtr<FJsonValue>> KeyLines;
			for (const FString& Line : Lines)
			{
				if (LooksLikeBuildErrorLine(Line))
				{
					ErrorLines.Add(MakeShared<FJsonValueObject>(MakeBuildLineObject(Line)));
				}

				if (LooksLikeImportantBuildLine(Line))
				{
					KeyLines.Add(MakeShared<FJsonValueString>(Line.TrimStartAndEnd()));
					if (KeyLines.Num() > 120)
					{
						KeyLines.RemoveAt(0);
					}
				}
			}

			const bool bSucceeded = ReturnCode == 0 && !LogText.Contains(TEXT("Result: Failed"), ESearchCase::IgnoreCase);
			StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
			StructuredContent->SetNumberField(TEXT("returnCode"), ReturnCode);
			StructuredContent->SetNumberField(TEXT("errorCount"), ErrorLines.Num());
			StructuredContent->SetArrayField(TEXT("errors"), ErrorLines);
			StructuredContent->SetArrayField(TEXT("keyLogLines"), KeyLines);
		}

		FString ResolveBuildSourceFilePath(const FString& FilePath)
		{
			FString TrimmedPath = FilePath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				return FString();
			}

			if (FPaths::FileExists(TrimmedPath))
			{
				return FPaths::ConvertRelativePathToFull(TrimmedPath);
			}

			const TArray<FString> CandidateRoots = {
				FPaths::ProjectDir(),
				FPaths::EngineDir()
			};
			for (const FString& Root : CandidateRoots)
			{
				const FString CandidatePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, TrimmedPath));
				if (FPaths::FileExists(CandidatePath))
				{
					return CandidatePath;
				}
			}

			return TrimmedPath;
		}

		TSharedPtr<FJsonObject> MakeSourceContextObject(const FString& SourcePath, int32 LineNumber, int32 ContextLines)
		{
			TSharedPtr<FJsonObject> ContextObject = MakeShared<FJsonObject>();
			ContextObject->SetStringField(TEXT("path"), SourcePath);
			ContextObject->SetNumberField(TEXT("line"), LineNumber);
			ContextObject->SetBoolField(TEXT("available"), false);

			if (SourcePath.IsEmpty() || LineNumber <= 0 || !FPaths::FileExists(SourcePath))
			{
				return ContextObject;
			}

			FString SourceText;
			if (!FFileHelper::LoadFileToString(SourceText, *SourcePath))
			{
				return ContextObject;
			}

			TArray<FString> Lines;
			SourceText.ParseIntoArrayLines(Lines, false);
			const int32 ZeroBasedLine = LineNumber - 1;
			const int32 FirstLine = FMath::Clamp(ZeroBasedLine - FMath::Max(0, ContextLines), 0, FMath::Max(0, Lines.Num() - 1));
			const int32 LastLine = FMath::Clamp(ZeroBasedLine + FMath::Max(0, ContextLines), 0, FMath::Max(0, Lines.Num() - 1));

			TArray<TSharedPtr<FJsonValue>> ContextLinesArray;
			for (int32 Index = FirstLine; Index <= LastLine && Lines.IsValidIndex(Index); ++Index)
			{
				TSharedPtr<FJsonObject> LineObject = MakeShared<FJsonObject>();
				LineObject->SetNumberField(TEXT("line"), Index + 1);
				LineObject->SetStringField(TEXT("text"), Lines[Index]);
				LineObject->SetBoolField(TEXT("isErrorLine"), Index == ZeroBasedLine);
				ContextLinesArray.Add(MakeShared<FJsonValueObject>(LineObject));
			}

			ContextObject->SetBoolField(TEXT("available"), ContextLinesArray.Num() > 0);
			ContextObject->SetArrayField(TEXT("lines"), ContextLinesArray);
			return ContextObject;
		}

		FString GuessCompileErrorCause(const FString& Message)
		{
			const FString Lower = Message.ToLower();
			if (Lower.Contains(TEXT("undeclared identifier")) || Lower.Contains(TEXT("use of undeclared identifier")) || Lower.Contains(TEXT("was not declared")))
			{
				return TEXT("A symbol is missing, misspelled, out of scope, or requires an include/header declaration.");
			}
			if (Lower.Contains(TEXT("no member named")) || Lower.Contains(TEXT("has no member")) || Lower.Contains(TEXT("is not a member")))
			{
				return TEXT("The code is calling a member that does not exist for this type, often due to an Unreal API mismatch or wrong object type.");
			}
			if (Lower.Contains(TEXT("cannot convert")) || Lower.Contains(TEXT("no viable conversion")) || Lower.Contains(TEXT("cannot initialize")) || Lower.Contains(TEXT("incompatible")))
			{
				return TEXT("A type mismatch or invalid implicit conversion is likely near the reported line.");
			}
			if (Lower.Contains(TEXT("expected ';'")) || Lower.Contains(TEXT("expected expression")) || Lower.Contains(TEXT("expected ')'")) || Lower.Contains(TEXT("expected '}'")))
			{
				return TEXT("A syntax issue such as missing punctuation, mismatched parentheses/braces, or malformed statement is likely near the reported line.");
			}
			if (Lower.Contains(TEXT("cannot open include")) || Lower.Contains(TEXT("file not found")) || Lower.Contains(TEXT("no such file")))
			{
				return TEXT("An include path, module dependency, generated header, or file path is missing.");
			}
			if (Lower.Contains(TEXT("undefined symbol")) || Lower.Contains(TEXT("unresolved external")) || Lower.Contains(TEXT("linker command failed")))
			{
				return TEXT("A function or symbol is declared but not linked/defined, or a Build.cs module dependency is missing.");
			}
			if (Lower.Contains(TEXT("generated.h")))
			{
				return TEXT("Unreal Header Tool ordering or reflection markup may be wrong; generated.h must usually be the final include in a UObject header.");
			}
			return TEXT("Review the nearby source context and preceding build errors; this may be a cascading compiler error.");
		}

		TArray<TSharedPtr<FJsonValue>> MakeSuggestedFixesForCompileError(const FString& Message)
		{
			TArray<TSharedPtr<FJsonValue>> SuggestedFixes;
			const FString Lower = Message.ToLower();
			if (Lower.Contains(TEXT("undeclared identifier")) || Lower.Contains(TEXT("use of undeclared identifier")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Check spelling and scope of the reported symbol.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Add the required declaration or include the header that defines the symbol.")));
			}
			else if (Lower.Contains(TEXT("no member named")) || Lower.Contains(TEXT("has no member")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Inspect the expression before the member access and confirm its actual type.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Update the call to the correct Unreal API/member for this engine version.")));
			}
			else if (Lower.Contains(TEXT("expected")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Check the current and previous line for missing semicolons, braces, parentheses, or commas.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("If this followed an automatic patch-fragment insertion, diff the inserted block boundaries first.")));
			}
			else if (Lower.Contains(TEXT("cannot convert")) || Lower.Contains(TEXT("no viable conversion")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Confirm the expected parameter/property type and add an explicit conversion only if safe.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Prefer matching Unreal types exactly, especially FString/FName/FText and TSharedPtr variants.")));
			}
			else
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Fix the earliest reported error first; later errors may be cascading.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Run unreal.mcp_diff_last_apply if this happened after applying a scaffold.")));
			}
			return SuggestedFixes;
		}

		FUnrealMcpExecutionResult CompileErrorFixPlan(const FJsonObject& Arguments)
		{
			FString BuildLogPath;
			bool bIncludeSourceContext = true;
			bool bAutoPatch = false;
			bool bDryRun = true;
			double MaxErrorsDouble = 8.0;
			double ContextLinesDouble = 4.0;

			Arguments.TryGetStringField(TEXT("buildLogPath"), BuildLogPath);
			Arguments.TryGetBoolField(TEXT("includeSourceContext"), bIncludeSourceContext);
			Arguments.TryGetBoolField(TEXT("autoPatch"), bAutoPatch);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetNumberField(TEXT("maxErrors"), MaxErrorsDouble);
			Arguments.TryGetNumberField(TEXT("contextLines"), ContextLinesDouble);

			if (BuildLogPath.TrimStartAndEnd().IsEmpty())
			{
				if (!FindNewestFile(GetMcpBuildLogRoot(), TEXT("*.log"), BuildLogPath))
				{
					return MakeExecutionResult(TEXT("No build log was found. Run unreal.mcp_build_editor first or pass buildLogPath."), nullptr, true);
				}
			}

			FString ResolvedBuildLogPath;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(BuildLogPath, ResolvedBuildLogPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString LogText;
			if (!FFileHelper::LoadFileToString(LogText, *ResolvedBuildLogPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read build log '%s'."), *ResolvedBuildLogPath), nullptr, true);
			}

			TArray<FString> Lines;
			LogText.ParseIntoArrayLines(Lines, false);
			const int32 MaxErrors = FMath::Clamp(static_cast<int32>(MaxErrorsDouble), 1, 50);
			const int32 ContextLines = FMath::Clamp(static_cast<int32>(ContextLinesDouble), 0, 20);

			TArray<TSharedPtr<FJsonValue>> ErrorPlans;
			for (const FString& Line : Lines)
			{
				if (!LooksLikeBuildErrorLine(Line))
				{
					continue;
				}

				TSharedPtr<FJsonObject> ErrorObject = MakeBuildLineObject(Line);
				FString FilePath;
				FString Message;
				double LineNumberDouble = 0.0;
				ErrorObject->TryGetStringField(TEXT("file"), FilePath);
				ErrorObject->TryGetStringField(TEXT("message"), Message);
				ErrorObject->TryGetNumberField(TEXT("line"), LineNumberDouble);

				const FString ResolvedSourcePath = ResolveBuildSourceFilePath(FilePath);
				ErrorObject->SetStringField(TEXT("resolvedFile"), ResolvedSourcePath);
				ErrorObject->SetStringField(TEXT("probableCause"), GuessCompileErrorCause(Message));
				ErrorObject->SetArrayField(TEXT("suggestedFixes"), MakeSuggestedFixesForCompileError(Message));
				if (bIncludeSourceContext)
				{
					ErrorObject->SetObjectField(TEXT("sourceContext"), MakeSourceContextObject(ResolvedSourcePath, static_cast<int32>(LineNumberDouble), ContextLines));
				}
				ErrorObject->SetBoolField(TEXT("autoPatchSupported"), false);
				ErrorObject->SetStringField(TEXT("autoPatchReason"), TEXT("No deterministic safe patch pattern matched. Use this fix plan to patch the scaffold/source intentionally."));

				ErrorPlans.Add(MakeShared<FJsonValueObject>(ErrorObject));
				if (ErrorPlans.Num() >= MaxErrors)
				{
					break;
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_compile_error_fix_plan"));
			StructuredContent->SetStringField(TEXT("buildLogPath"), ResolvedBuildLogPath);
			StructuredContent->SetBoolField(TEXT("includeSourceContext"), bIncludeSourceContext);
			StructuredContent->SetBoolField(TEXT("autoPatch"), bAutoPatch);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("patchApplied"), false);
			StructuredContent->SetStringField(TEXT("autoPatchStatus"), bAutoPatch ? TEXT("requested_but_no_safe_patch_matched") : TEXT("not_requested"));
			StructuredContent->SetNumberField(TEXT("plannedErrorCount"), ErrorPlans.Num());
			StructuredContent->SetArrayField(TEXT("fixPlan"), ErrorPlans);
			StructuredContent->SetStringField(TEXT("nextStep"), TEXT("Patch the earliest root-cause error, rebuild with unreal.mcp_build_editor, then rerun this tool if errors remain."));

			return MakeExecutionResult(
				ErrorPlans.Num() > 0
					? FString::Printf(TEXT("Generated compile error fix plan for %d error(s)."), ErrorPlans.Num())
					: TEXT("No compiler error lines were detected in the build log."),
				StructuredContent,
				false);
		}

		void WriteBuildTestMemory(
			const FString& MemoryKey,
			const FString& Summary,
			const FString& Status,
			const FString& NextStep,
			const TSharedPtr<FJsonObject>& ContentObject)
		{
			TSharedPtr<FJsonObject> MemoryArgs = MakeShared<FJsonObject>();
			MemoryArgs->SetStringField(TEXT("key"), MemoryKey);
			MemoryArgs->SetStringField(TEXT("summary"), Summary);
			MemoryArgs->SetStringField(TEXT("status"), Status);
			MemoryArgs->SetStringField(TEXT("nextStep"), NextStep);
			MemoryArgs->SetStringField(TEXT("contentJson"), JsonObjectToString(ContentObject));
			MemoryArgs->SetArrayField(TEXT("tags"), MakeJsonStringArray({ TEXT("mcp"), TEXT("build"), TEXT("test"), TEXT("restart") }));
			ProjectMemoryWrite(*MemoryArgs);
		}

		TSharedPtr<FJsonObject> MakePipelineStepObject(
			const FString& StepName,
			const FString& Status,
			const FString& Message,
			const FUnrealMcpExecutionResult* Result)
		{
			TSharedPtr<FJsonObject> StepObject = MakeShared<FJsonObject>();
			StepObject->SetStringField(TEXT("step"), StepName);
			StepObject->SetStringField(TEXT("status"), Status);
			StepObject->SetStringField(TEXT("message"), Message);
			if (Result)
			{
				StepObject->SetBoolField(TEXT("isError"), Result->bIsError);
				StepObject->SetStringField(TEXT("text"), Result->Text);
				if (Result->StructuredContent.IsValid())
				{
					StepObject->SetObjectField(TEXT("structuredContent"), Result->StructuredContent);
				}
			}
			return StepObject;
		}

		FString GetProjectFilePathForBuild()
		{
			FString ProjectFilePath = FPaths::GetProjectFilePath();
			if (ProjectFilePath.IsEmpty())
			{
				ProjectFilePath = FPaths::Combine(FPaths::ProjectDir(), FString::Printf(TEXT("%s.uproject"), FApp::GetProjectName()));
			}
			return FPaths::ConvertRelativePathToFull(ProjectFilePath);
		}

		FUnrealMcpExecutionResult BuildNonEditorTarget(
			const FJsonObject& Arguments,
			const FString& ActionTag,
			const FString& DefaultTarget,
			const FString& MemoryKeyDefault)
		{
			FString Target = DefaultTarget;
			FString Platform = GetHostBuildPlatformName();
			FString Configuration = TEXT("Development");
			FString ExtraArgs;
			FString ToolName;
			FString TestRequestPath;
			FString TestsDir;
			FString ScaffoldDir;
			FString MemoryKey = MemoryKeyDefault;
			bool bWriteProjectMemory = true;

			Arguments.TryGetStringField(TEXT("target"), Target);
			Arguments.TryGetStringField(TEXT("platform"), Platform);
			Arguments.TryGetStringField(TEXT("configuration"), Configuration);
			Arguments.TryGetStringField(TEXT("extraArgs"), ExtraArgs);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
			Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);

			Target = Target.TrimStartAndEnd();
			Platform = Platform.TrimStartAndEnd();
			Configuration = Configuration.TrimStartAndEnd();
			ExtraArgs = ExtraArgs.TrimStartAndEnd();
			MemoryKey = MemoryKey.TrimStartAndEnd();
			if (Target.IsEmpty() || Platform.IsEmpty() || Configuration.IsEmpty())
			{
				return MakeExecutionResult(TEXT("target, platform, and configuration must not be empty."), nullptr, true);
			}
			if (MemoryKey.IsEmpty())
			{
				MemoryKey = MemoryKeyDefault;
			}

			FString ExtraArgsValidationFailure;
			if (!SanitizeBuildExtraArgs(ExtraArgs, ExtraArgs, ExtraArgsValidationFailure))
			{
				return MakeExecutionResult(ExtraArgsValidationFailure, nullptr, true);
			}

			const FString BuildScriptPath = GetUnrealBuildScriptPath();
			if (!FPaths::FileExists(BuildScriptPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unreal Build script was not found: %s"), *BuildScriptPath), nullptr, true);
			}

			const FString ProjectFilePath = GetProjectFilePathForBuild();
			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BuildLogDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/BuildLogs")));
			IFileManager::Get().MakeDirectory(*BuildLogDirectory, true);
			const FString BuildLogPath = FPaths::Combine(BuildLogDirectory, FString::Printf(TEXT("Build_%s_%s_%s.log"), *Target, *Configuration, *Timestamp));

			if (TestRequestPath.TrimStartAndEnd().IsEmpty() && !ScaffoldDir.TrimStartAndEnd().IsEmpty())
			{
				FString ResolvedScaffoldDir;
				FString ResolveFailure;
				if (ResolveProjectPathInsideProject(ScaffoldDir, ResolvedScaffoldDir, ResolveFailure))
				{
					TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
					if (TestsDir.TrimStartAndEnd().IsEmpty())
					{
						TestsDir = FPaths::Combine(ResolvedScaffoldDir, TEXT("Tests"));
					}
				}
			}

			TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
			MemoryContent->SetStringField(TEXT("action"), ActionTag);
			MemoryContent->SetStringField(TEXT("target"), Target);
			MemoryContent->SetStringField(TEXT("platform"), Platform);
			MemoryContent->SetStringField(TEXT("configuration"), Configuration);
			MemoryContent->SetStringField(TEXT("toolName"), ToolName);
			MemoryContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
			MemoryContent->SetStringField(TEXT("testsDir"), TestsDir);
			MemoryContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			MemoryContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			MemoryContent->SetBoolField(TEXT("editorWasRunningDuringBuild"), true);
			MemoryContent->SetBoolField(TEXT("editorRestartRequiredBeforeTestingNewTools"), false);
			MemoryContent->SetStringField(TEXT("recommendation"), TEXT("The editor can remain open while this non-editor target builds. Use the parsed log as compile coverage evidence."));

			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					FString::Printf(TEXT("Waiting for %s build result."), *Target),
					TEXT("build_running"),
					FString::Printf(TEXT("Inspect %s after the build completes."), *BuildLogPath),
					MemoryContent);
			}

			const FString Params = FString::Printf(
				TEXT("%s %s %s -Project=%s -WaitMutex%s%s"),
				*Target,
				*Platform,
				*Configuration,
				*QuoteCommandLineArgument(ProjectFilePath),
				ExtraArgs.IsEmpty() ? TEXT("") : TEXT(" "),
				*ExtraArgs);

			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*BuildScriptPath,
				*Params,
				&ReturnCode,
				&StdOut,
				&StdErr,
				*FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));

			const FString CombinedLog = StdOut + (StdErr.IsEmpty() ? FString() : FString::Printf(TEXT("\n\n[stderr]\n%s"), *StdErr));
			FFileHelper::SaveStringToFile(CombinedLog, *BuildLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), ActionTag);
			StructuredContent->SetStringField(TEXT("target"), Target);
			StructuredContent->SetStringField(TEXT("platform"), Platform);
			StructuredContent->SetStringField(TEXT("configuration"), Configuration);
			StructuredContent->SetStringField(TEXT("extraArgs"), ExtraArgs);
			StructuredContent->SetStringField(TEXT("projectFile"), ProjectFilePath);
			StructuredContent->SetStringField(TEXT("buildScript"), BuildScriptPath);
			StructuredContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			StructuredContent->SetBoolField(TEXT("launched"), bLaunched);
			StructuredContent->SetBoolField(TEXT("editorRunningDuringBuild"), true);
			StructuredContent->SetBoolField(TEXT("editorRestartRequiredBeforeTestingNewTools"), false);
			StructuredContent->SetStringField(TEXT("restartAdvice"), TEXT("The editor can remain open while this non-editor target builds. This target alone does not require an editor restart."));

			ParseBuildLog(CombinedLog, bLaunched ? ReturnCode : -1, StructuredContent);
			const bool bSucceeded = StructuredContent->GetBoolField(TEXT("succeeded"));

			MemoryContent->SetBoolField(TEXT("buildSucceeded"), bSucceeded);
			MemoryContent->SetNumberField(TEXT("returnCode"), bLaunched ? ReturnCode : -1);
			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					bSucceeded ? FString::Printf(TEXT("%s build succeeded."), *Target) : FString::Printf(TEXT("%s build failed; inspect parsed errors and build log."), *Target),
					bSucceeded ? TEXT("build_succeeded") : TEXT("build_failed"),
					bSucceeded ? TEXT("Use this result as non-editor compile coverage evidence.") : FString::Printf(TEXT("Fix compile errors, then rerun unreal.%s."), *ActionTag),
					MemoryContent);
			}

			const FString Text = bSucceeded
				? FString::Printf(TEXT("Build succeeded for %s %s %s."), *Target, *Platform, *Configuration)
				: FString::Printf(TEXT("Build failed for %s %s %s. See parsed errors and log: %s"), *Target, *Platform, *Configuration, *BuildLogPath);
			return MakeExecutionResult(Text, StructuredContent, !bSucceeded);
		}

		FUnrealMcpExecutionResult BuildGame(const FJsonObject& Arguments)
		{
			return BuildNonEditorTarget(
				Arguments,
				TEXT("mcp_build_game"),
				FApp::GetProjectName(),
				TEXT("mcp.extension.build_game"));
		}

		FUnrealMcpExecutionResult BuildServer(const FJsonObject& Arguments)
		{
			return BuildNonEditorTarget(
				Arguments,
				TEXT("mcp_build_server"),
				FString::Printf(TEXT("%sServer"), FApp::GetProjectName()),
				TEXT("mcp.extension.build_server"));
		}

		FUnrealMcpExecutionResult BuildClient(const FJsonObject& Arguments)
		{
			return BuildNonEditorTarget(
				Arguments,
				TEXT("mcp_build_client"),
				FString::Printf(TEXT("%sClient"), FApp::GetProjectName()),
				TEXT("mcp.extension.build_client"));
		}

		FUnrealMcpExecutionResult BuildPackaged(const FJsonObject& Arguments)
		{
			FString TargetPlatform = GetHostBuildPlatformName();
			FString Configuration = TEXT("Development");
			FString ExtraArgs;
			FString OutputDirectory;
			FString Map;
			FString MemoryKey = TEXT("mcp.extension.build_packaged");
			bool bWriteProjectMemory = true;

			Arguments.TryGetStringField(TEXT("platform"), TargetPlatform);
			Arguments.TryGetStringField(TEXT("targetPlatform"), TargetPlatform);
			Arguments.TryGetStringField(TEXT("configuration"), Configuration);
			Arguments.TryGetStringField(TEXT("extraArgs"), ExtraArgs);
			const bool bHasOutputDirectory = Arguments.TryGetStringField(TEXT("outputDirectory"), OutputDirectory);
			Arguments.TryGetStringField(TEXT("map"), Map);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);

			TargetPlatform = TargetPlatform.TrimStartAndEnd();
			Configuration = Configuration.TrimStartAndEnd();
			ExtraArgs = ExtraArgs.TrimStartAndEnd();
			Map = Map.TrimStartAndEnd();
			MemoryKey = MemoryKey.TrimStartAndEnd();
			if (TargetPlatform.IsEmpty() || Configuration.IsEmpty())
			{
				return MakeExecutionResult(TEXT("targetPlatform and configuration must not be empty."), nullptr, true);
			}
			if (MemoryKey.IsEmpty())
			{
				MemoryKey = TEXT("mcp.extension.build_packaged");
			}

			FString ExtraArgsValidationFailure;
			if (!SanitizeBuildExtraArgs(ExtraArgs, ExtraArgs, ExtraArgsValidationFailure))
			{
				return MakeExecutionResult(ExtraArgsValidationFailure, nullptr, true);
			}

			const FString AutomationScriptPath = GetUnrealAutomationScriptPath();
			if (!FPaths::FileExists(AutomationScriptPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unreal Automation Tool script was not found: %s"), *AutomationScriptPath), nullptr, true);
			}

			if (bHasOutputDirectory && !OutputDirectory.TrimStartAndEnd().IsEmpty())
			{
				FString ResolvedOutputDirectory;
				FString FailureReason;
				if (!ResolveProjectPathInsideProject(OutputDirectory, ResolvedOutputDirectory, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				OutputDirectory = ResolvedOutputDirectory;
			}
			else
			{
				OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StagedBuilds")));
			}
			FPaths::NormalizeDirectoryName(OutputDirectory);
			FPaths::CollapseRelativeDirectories(OutputDirectory);
			IFileManager::Get().MakeDirectory(*OutputDirectory, true);

			const FString ProjectFilePath = GetProjectFilePathForBuild();
			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BuildLogDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/BuildLogs")));
			IFileManager::Get().MakeDirectory(*BuildLogDirectory, true);
			const FString BuildLogPath = FPaths::Combine(BuildLogDirectory, FString::Printf(TEXT("PackagedBuild_%s_%s_%s.log"), *TargetPlatform, *Configuration, *Timestamp));
			const FString StagedBuildDirectory = FPaths::Combine(OutputDirectory, TargetPlatform);

			TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
			MemoryContent->SetStringField(TEXT("action"), TEXT("mcp_build_packaged"));
			MemoryContent->SetStringField(TEXT("platform"), TargetPlatform);
			MemoryContent->SetStringField(TEXT("targetPlatform"), TargetPlatform);
			MemoryContent->SetStringField(TEXT("configuration"), Configuration);
			MemoryContent->SetStringField(TEXT("map"), Map);
			MemoryContent->SetStringField(TEXT("outputDirectory"), OutputDirectory);
			MemoryContent->SetStringField(TEXT("stagedBuildDirectory"), StagedBuildDirectory);
			MemoryContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			MemoryContent->SetStringField(TEXT("recommendation"), TEXT("Inspect the packaged output directory and parsed UAT log after completion."));

			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					FString::Printf(TEXT("Waiting for packaged %s build result."), *TargetPlatform),
					TEXT("packaged_build_running"),
					FString::Printf(TEXT("Inspect %s after BuildCookRun completes."), *BuildLogPath),
					MemoryContent);
			}

			TArray<FString> ParamParts;
			ParamParts.Add(TEXT("BuildCookRun"));
			ParamParts.Add(FString::Printf(TEXT("-project=%s"), *QuoteCommandLineArgument(ProjectFilePath)));
			ParamParts.Add(FString::Printf(TEXT("-platform=%s"), *TargetPlatform));
			ParamParts.Add(FString::Printf(TEXT("-configuration=%s"), *Configuration));
			ParamParts.Add(TEXT("-cook"));
			ParamParts.Add(TEXT("-build"));
			ParamParts.Add(TEXT("-stage"));
			ParamParts.Add(TEXT("-package"));
			ParamParts.Add(TEXT("-pak"));
			ParamParts.Add(TEXT("-archive"));
			ParamParts.Add(FString::Printf(TEXT("-archivedirectory=%s"), *QuoteCommandLineArgument(OutputDirectory)));
			if (!Map.IsEmpty())
			{
				ParamParts.Add(FString::Printf(TEXT("-map=%s"), *QuoteCommandLineArgument(Map)));
			}
			if (!ExtraArgs.IsEmpty())
			{
				ParamParts.Add(ExtraArgs);
			}
			const FString Params = FString::Join(ParamParts, TEXT(" "));

			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*AutomationScriptPath,
				*Params,
				&ReturnCode,
				&StdOut,
				&StdErr,
				*FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));

			const FString CombinedLog = StdOut + (StdErr.IsEmpty() ? FString() : FString::Printf(TEXT("\n\n[stderr]\n%s"), *StdErr));
			FFileHelper::SaveStringToFile(CombinedLog, *BuildLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_build_packaged"));
			StructuredContent->SetStringField(TEXT("platform"), TargetPlatform);
			StructuredContent->SetStringField(TEXT("targetPlatform"), TargetPlatform);
			StructuredContent->SetStringField(TEXT("configuration"), Configuration);
			StructuredContent->SetStringField(TEXT("extraArgs"), ExtraArgs);
			StructuredContent->SetStringField(TEXT("map"), Map);
			StructuredContent->SetStringField(TEXT("outputDirectory"), OutputDirectory);
			StructuredContent->SetStringField(TEXT("stagedBuildDirectory"), StagedBuildDirectory);
			StructuredContent->SetStringField(TEXT("projectFile"), ProjectFilePath);
			StructuredContent->SetStringField(TEXT("buildScript"), AutomationScriptPath);
			StructuredContent->SetStringField(TEXT("automationScript"), AutomationScriptPath);
			StructuredContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			StructuredContent->SetStringField(TEXT("params"), Params);
			StructuredContent->SetBoolField(TEXT("launched"), bLaunched);

			ParseBuildLog(CombinedLog, bLaunched ? ReturnCode : -1, StructuredContent);
			const bool bSucceeded = StructuredContent->GetBoolField(TEXT("succeeded"));

			MemoryContent->SetBoolField(TEXT("buildSucceeded"), bSucceeded);
			MemoryContent->SetNumberField(TEXT("returnCode"), bLaunched ? ReturnCode : -1);
			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					bSucceeded ? FString::Printf(TEXT("Packaged %s build succeeded."), *TargetPlatform) : FString::Printf(TEXT("Packaged %s build failed; inspect parsed errors and UAT log."), *TargetPlatform),
					bSucceeded ? TEXT("packaged_build_succeeded") : TEXT("packaged_build_failed"),
					bSucceeded ? FString::Printf(TEXT("Inspect packaged output under %s."), *StagedBuildDirectory) : TEXT("Fix cook/package errors, then rerun unreal.mcp_build_packaged."),
					MemoryContent);
			}

			const FString Text = bSucceeded
				? FString::Printf(TEXT("Packaged build succeeded for %s %s. Output: %s"), *TargetPlatform, *Configuration, *StagedBuildDirectory)
				: FString::Printf(TEXT("Packaged build failed for %s %s. See parsed errors and log: %s"), *TargetPlatform, *Configuration, *BuildLogPath);
			return MakeExecutionResult(Text, StructuredContent, !bSucceeded);
		}

		bool TryExecuteSelfExtensionBuildTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
		{
			if (ToolName == TEXT("unreal.mcp_build_editor"))
			{
				OutResult = BuildEditor(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_build_game"))
			{
				OutResult = BuildGame(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_build_server"))
			{
				OutResult = BuildServer(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_build_client"))
			{
				OutResult = BuildClient(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_build_packaged"))
			{
				OutResult = BuildPackaged(Arguments);
				return true;
			}

			return false;
		}

			FUnrealMcpExecutionResult BuildEditor(const FJsonObject& Arguments)
			{
			FString Target = FString::Printf(TEXT("%sEditor"), FApp::GetProjectName());
			FString Platform = GetHostBuildPlatformName();
			FString Configuration = TEXT("Development");
			FString ExtraArgs;
			FString ToolName;
			FString TestRequestPath;
			FString TestsDir;
			FString ScaffoldDir;
			FString MemoryKey = TEXT("mcp.extension.build_test");
			bool bWriteProjectMemory = true;

			Arguments.TryGetStringField(TEXT("target"), Target);
			Arguments.TryGetStringField(TEXT("platform"), Platform);
			Arguments.TryGetStringField(TEXT("configuration"), Configuration);
			Arguments.TryGetStringField(TEXT("extraArgs"), ExtraArgs);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
			Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);

			Target = Target.TrimStartAndEnd();
			Platform = Platform.TrimStartAndEnd();
			Configuration = Configuration.TrimStartAndEnd();
			ExtraArgs = ExtraArgs.TrimStartAndEnd();
			MemoryKey = MemoryKey.TrimStartAndEnd();
			if (Target.IsEmpty() || Platform.IsEmpty() || Configuration.IsEmpty())
			{
				return MakeExecutionResult(TEXT("target, platform, and configuration must not be empty."), nullptr, true);
			}
			if (MemoryKey.IsEmpty())
			{
				MemoryKey = TEXT("mcp.extension.build_test");
			}

			FString ExtraArgsValidationFailure;
			if (!SanitizeBuildExtraArgs(ExtraArgs, ExtraArgs, ExtraArgsValidationFailure))
			{
				return MakeExecutionResult(ExtraArgsValidationFailure, nullptr, true);
			}

			const FString BuildScriptPath = GetUnrealBuildScriptPath();
			if (!FPaths::FileExists(BuildScriptPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unreal Build script was not found: %s"), *BuildScriptPath), nullptr, true);
			}

			FString ProjectFilePath = FPaths::GetProjectFilePath();
			if (ProjectFilePath.IsEmpty())
			{
				ProjectFilePath = FPaths::Combine(FPaths::ProjectDir(), FString::Printf(TEXT("%s.uproject"), FApp::GetProjectName()));
			}
			ProjectFilePath = FPaths::ConvertRelativePathToFull(ProjectFilePath);

			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BuildLogDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/BuildLogs")));
			IFileManager::Get().MakeDirectory(*BuildLogDirectory, true);
			const FString BuildLogPath = FPaths::Combine(BuildLogDirectory, FString::Printf(TEXT("Build_%s_%s_%s.log"), *Target, *Configuration, *Timestamp));

			if (TestRequestPath.TrimStartAndEnd().IsEmpty() && !ScaffoldDir.TrimStartAndEnd().IsEmpty())
			{
				FString ResolvedScaffoldDir;
				FString ResolveFailure;
				if (ResolveProjectPathInsideProject(ScaffoldDir, ResolvedScaffoldDir, ResolveFailure))
				{
					TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
					if (TestsDir.TrimStartAndEnd().IsEmpty())
					{
						TestsDir = FPaths::Combine(ResolvedScaffoldDir, TEXT("Tests"));
					}
				}
			}

			TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
			MemoryContent->SetStringField(TEXT("target"), Target);
			MemoryContent->SetStringField(TEXT("platform"), Platform);
			MemoryContent->SetStringField(TEXT("configuration"), Configuration);
			MemoryContent->SetStringField(TEXT("toolName"), ToolName);
			MemoryContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
			MemoryContent->SetStringField(TEXT("testsDir"), TestsDir);
			MemoryContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			MemoryContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			MemoryContent->SetBoolField(TEXT("editorWasRunningDuringBuild"), true);
			MemoryContent->SetBoolField(TEXT("editorRestartRequiredBeforeTestingNewTools"), true);
			MemoryContent->SetStringField(TEXT("recommendation"), TEXT("Restart Unreal Editor after a successful plugin build before running unreal.mcp_run_tool_test for newly added tools."));

			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					TEXT("Waiting for Unreal Editor restart before MCP tool testing."),
					TEXT("waiting_for_editor_restart_after_build"),
					TEXT("If build succeeds, restart Unreal Editor, then run unreal.mcp_run_tool_test with this memoryKey."),
					MemoryContent);
			}

			const FString Params = FString::Printf(
				TEXT("%s %s %s -Project=%s -WaitMutex%s%s"),
				*Target,
				*Platform,
				*Configuration,
				*QuoteCommandLineArgument(ProjectFilePath),
				ExtraArgs.IsEmpty() ? TEXT("") : TEXT(" "),
				*ExtraArgs);

			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*BuildScriptPath,
				*Params,
				&ReturnCode,
				&StdOut,
				&StdErr,
				*FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));

			const FString CombinedLog = StdOut + (StdErr.IsEmpty() ? FString() : FString::Printf(TEXT("\n\n[stderr]\n%s"), *StdErr));
			FFileHelper::SaveStringToFile(CombinedLog, *BuildLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_build_editor"));
			StructuredContent->SetStringField(TEXT("target"), Target);
			StructuredContent->SetStringField(TEXT("platform"), Platform);
			StructuredContent->SetStringField(TEXT("configuration"), Configuration);
			StructuredContent->SetStringField(TEXT("extraArgs"), ExtraArgs);
			StructuredContent->SetStringField(TEXT("projectFile"), ProjectFilePath);
			StructuredContent->SetStringField(TEXT("buildScript"), BuildScriptPath);
			StructuredContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			StructuredContent->SetBoolField(TEXT("launched"), bLaunched);
			StructuredContent->SetBoolField(TEXT("editorRunningDuringBuild"), true);
			StructuredContent->SetBoolField(TEXT("editorRestartRequiredBeforeTestingNewTools"), true);
			StructuredContent->SetStringField(TEXT("restartAdvice"), TEXT("The editor is running because this tool is invoked from Chat. A successful plugin build still requires restarting Unreal Editor before new tool definitions are loaded."));

			ParseBuildLog(CombinedLog, bLaunched ? ReturnCode : -1, StructuredContent);
			const bool bSucceeded = StructuredContent->GetBoolField(TEXT("succeeded"));

			MemoryContent->SetBoolField(TEXT("buildSucceeded"), bSucceeded);
			MemoryContent->SetNumberField(TEXT("returnCode"), bLaunched ? ReturnCode : -1);
			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					bSucceeded ? TEXT("Editor build succeeded; restart before MCP tool test.") : TEXT("Editor build failed; inspect parsed errors and build log."),
					bSucceeded ? TEXT("build_succeeded_restart_required") : TEXT("build_failed"),
					bSucceeded ? TEXT("Restart Unreal Editor, then run unreal.mcp_run_tool_test.") : TEXT("Fix compile errors, then rerun unreal.mcp_build_editor."),
					MemoryContent);
			}

			const FString Text = bSucceeded
				? FString::Printf(TEXT("Build succeeded for %s %s %s. Restart Unreal Editor before testing newly compiled MCP tools."), *Target, *Platform, *Configuration)
				: FString::Printf(TEXT("Build failed for %s %s %s. See parsed errors and log: %s"), *Target, *Platform, *Configuration, *BuildLogPath);
			return MakeExecutionResult(Text, StructuredContent, !bSucceeded);
		}

		FString GetMcpSupervisorScriptPath()
		{
			TArray<FString> ToolsRootCandidates;
			FString ToolsRoot;
			ResolveSharedRepoRoot(FString(), { TEXT("unreal_mcp_supervisor.py") }, ToolsRoot, ToolsRootCandidates);
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(ToolsRoot, TEXT("unreal_mcp_supervisor.py")));
		}

		FString MakeSupervisorDefaultArgsJson(const FString& MemoryKey)
		{
			FString EscapedMemoryKey = MemoryKey;
			EscapedMemoryKey.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			EscapedMemoryKey.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("{\"memoryKey\":\"%s\"}"), *EscapedMemoryKey);
		}

		FUnrealMcpExecutionResult SupervisorInstall(const FJsonObject& Arguments)
		{
			FString Platform = TEXT("all");
			FString OutputDir = TEXT("Tools/UnrealMcpSupervisor");
			FString Label = FString::Printf(TEXT("com.unrealmcp.%s"), *SanitizeMcpToolIdForPath(FApp::GetProjectName()).ToLower());
			FString MemoryKey = TEXT("mcp.extension.pipeline");
			FString ArgsJson;
			FString EndpointUrl = TEXT("http://127.0.0.1:8765/mcp");
			FString SupervisorLogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/SupervisorLogs"));
			FString EditorCmd;
			bool bInstallLaunchAgent = false;
			bool bLaunchAtLoad = false;
			bool bAutoRestart = true;
			bool bOverwrite = true;
			bool bDryRun = false;

			Arguments.TryGetStringField(TEXT("platform"), Platform);
			const bool bHasExplicitOutputDir = Arguments.TryGetStringField(TEXT("outputDir"), OutputDir);
			Arguments.TryGetStringField(TEXT("label"), Label);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetStringField(TEXT("argsJson"), ArgsJson);
			Arguments.TryGetStringField(TEXT("endpointUrl"), EndpointUrl);
			Arguments.TryGetStringField(TEXT("supervisorLogDir"), SupervisorLogDir);
			Arguments.TryGetStringField(TEXT("editorCmd"), EditorCmd);
			Arguments.TryGetBoolField(TEXT("installLaunchAgent"), bInstallLaunchAgent);
			Arguments.TryGetBoolField(TEXT("launchAtLoad"), bLaunchAtLoad);
			Arguments.TryGetBoolField(TEXT("autoRestart"), bAutoRestart);
			Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			Platform = Platform.TrimStartAndEnd().ToLower();
			if (Platform.IsEmpty())
			{
				Platform = TEXT("all");
			}
			if (MemoryKey.TrimStartAndEnd().IsEmpty())
			{
				MemoryKey = TEXT("mcp.extension.pipeline");
			}
			if (ArgsJson.TrimStartAndEnd().IsEmpty())
			{
				ArgsJson = MakeSupervisorDefaultArgsJson(MemoryKey);
			}

			const FString SupervisorScriptPath = GetMcpSupervisorScriptPath();
			if (!FPaths::FileExists(SupervisorScriptPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Supervisor script was not found: %s"), *SupervisorScriptPath), nullptr, true);
			}

			FString ResolvedOutputDir;
			FString FailureReason;
			TArray<FString> SupervisorOutputCandidates;
			if (!bHasExplicitOutputDir)
			{
				ResolveSharedRepoRoot(TEXT("UnrealMcpSupervisor"), TArray<FString>(), ResolvedOutputDir, SupervisorOutputCandidates);
			}
			else if (!ResolveProjectPathInsideProject(OutputDir, ResolvedOutputDir, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ProjectFilePath = FPaths::GetProjectFilePath();
			if (ProjectFilePath.IsEmpty())
			{
				ProjectFilePath = FPaths::Combine(FPaths::ProjectDir(), FString::Printf(TEXT("%s.uproject"), FApp::GetProjectName()));
			}
			ProjectFilePath = FPaths::ConvertRelativePathToFull(ProjectFilePath);

			TArray<FString> ParamParts;
			ParamParts.Add(QuoteCommandLineArgument(SupervisorScriptPath));
			ParamParts.Add(TEXT("--url"));
			ParamParts.Add(QuoteCommandLineArgument(EndpointUrl));
			ParamParts.Add(TEXT("--uproject"));
			ParamParts.Add(QuoteCommandLineArgument(ProjectFilePath));
			if (!EditorCmd.TrimStartAndEnd().IsEmpty())
			{
				ParamParts.Add(TEXT("--editor-cmd"));
				ParamParts.Add(QuoteCommandLineArgument(EditorCmd.TrimStartAndEnd()));
			}
			ParamParts.Add(TEXT("install"));
			ParamParts.Add(TEXT("--output-dir"));
			ParamParts.Add(QuoteCommandLineArgument(ResolvedOutputDir));
			ParamParts.Add(TEXT("--platform"));
			ParamParts.Add(QuoteCommandLineArgument(Platform));
			ParamParts.Add(TEXT("--label"));
			ParamParts.Add(QuoteCommandLineArgument(Label));
			ParamParts.Add(TEXT("--memory-key"));
			ParamParts.Add(QuoteCommandLineArgument(MemoryKey));
			ParamParts.Add(TEXT("--args-json"));
			ParamParts.Add(QuoteCommandLineArgument(ArgsJson));
			ParamParts.Add(TEXT("--supervisor-log-dir"));
			ParamParts.Add(QuoteCommandLineArgument(FPaths::ConvertRelativePathToFull(SupervisorLogDir)));
			if (bInstallLaunchAgent)
			{
				ParamParts.Add(TEXT("--install-launch-agent"));
			}
			if (bLaunchAtLoad)
			{
				ParamParts.Add(TEXT("--launch-at-load"));
			}
			if (!bAutoRestart)
			{
				ParamParts.Add(TEXT("--no-auto-restart"));
			}
			if (bOverwrite)
			{
				ParamParts.Add(TEXT("--overwrite"));
			}

#if PLATFORM_WINDOWS
			const FString PythonExecutable = TEXT("py");
			const FString Params = TEXT("-3 ") + FString::Join(ParamParts, TEXT(" "));
#else
			const FString PythonExecutable = TEXT("/usr/bin/env");
			const FString Params = TEXT("python3 ") + FString::Join(ParamParts, TEXT(" "));
#endif

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_supervisor_install"));
			StructuredContent->SetStringField(TEXT("platform"), Platform);
			StructuredContent->SetStringField(TEXT("outputDir"), ResolvedOutputDir);
			StructuredContent->SetStringField(TEXT("label"), Label);
			StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
			StructuredContent->SetStringField(TEXT("argsJson"), ArgsJson);
			StructuredContent->SetStringField(TEXT("endpointUrl"), EndpointUrl);
			StructuredContent->SetStringField(TEXT("supervisorLogDir"), FPaths::ConvertRelativePathToFull(SupervisorLogDir));
			StructuredContent->SetStringField(TEXT("supervisorScriptPath"), SupervisorScriptPath);
			if (SupervisorOutputCandidates.Num() > 0)
			{
				StructuredContent->SetArrayField(TEXT("outputDirCandidates"), MakeSharedRepoRootCandidateValues(SupervisorOutputCandidates, TArray<FString>()));
			}
			StructuredContent->SetStringField(TEXT("executable"), PythonExecutable);
			StructuredContent->SetStringField(TEXT("params"), Params);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("autoRestart"), bAutoRestart);
			StructuredContent->SetBoolField(TEXT("installLaunchAgent"), bInstallLaunchAgent);

			if (bDryRun)
			{
				return MakeExecutionResult(TEXT("Dry run supervisor install prepared the generator command."), StructuredContent, false);
			}

			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*PythonExecutable,
				*Params,
				&ReturnCode,
				&StdOut,
				&StdErr,
				*FPaths::ProjectDir());

			StructuredContent->SetBoolField(TEXT("launched"), bLaunched);
			StructuredContent->SetNumberField(TEXT("returnCode"), ReturnCode);
			StructuredContent->SetStringField(TEXT("stdout"), StdOut);
			StructuredContent->SetStringField(TEXT("stderr"), StdErr);

			TSharedPtr<FJsonObject> InstallResult;
			if (LoadJsonObject(StdOut, InstallResult) && InstallResult.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("installResult"), InstallResult);
			}

			const bool bSucceeded = bLaunched && ReturnCode == 0;
			return MakeExecutionResult(
				bSucceeded
					? FString::Printf(TEXT("Supervisor launcher files generated under %s."), *ResolvedOutputDir)
					: FString::Printf(TEXT("Supervisor install failed with returnCode=%d. stderr: %s"), ReturnCode, *StdErr),
				StructuredContent,
				!bSucceeded);
		}



}
