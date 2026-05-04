#include "UnrealMcpSelfExtensionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UnrealMcp
{
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FString GetMcpBuildLogRoot();
	FString TailLines(const FString& Text, int32 MaxLines);
	bool FindNewestFile(const FString& Directory, const FString& Pattern, FString& OutPath);
	bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason);
	FString JsonObjectToString(const TSharedPtr<FJsonObject>& Object);
	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Strings);
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

		FString QuoteCommandLineArgument(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Escaped);
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
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("If this followed an automatic snippet insertion, diff the inserted block boundaries first.")));
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
			const FUnrealMcpExecutionResult* Result = nullptr)
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

}
