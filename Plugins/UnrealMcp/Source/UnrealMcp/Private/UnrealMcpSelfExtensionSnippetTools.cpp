#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UnrealMcp
{
		void BuildSimpleLineDiffPreview(
			const FString& BeforeText,
			const FString& AfterText,
			int32 MaxPreviewLines,
			TArray<TSharedPtr<FJsonValue>>& OutChangedLines,
			FString& OutPreviewText,
			int32& OutChangedLineCount,
			bool& bOutTruncated)
		{
			TArray<FString> BeforeLines;
			TArray<FString> AfterLines;
			BeforeText.ParseIntoArrayLines(BeforeLines, false);
			AfterText.ParseIntoArrayLines(AfterLines, false);

			const int32 SafeMaxPreviewLines = FMath::Max(1, MaxPreviewLines);
			TArray<FString> PreviewLines;
			OutChangedLineCount = 0;
			bOutTruncated = false;

			auto AddPreviewLine = [&](
				const FString& Kind,
				int32 BeforeLineNumber,
				int32 AfterLineNumber,
				const FString& BeforeLine,
				const FString& AfterLine)
			{
				++OutChangedLineCount;
				if (OutChangedLines.Num() >= SafeMaxPreviewLines)
				{
					bOutTruncated = true;
					return;
				}

				TSharedPtr<FJsonObject> LineObject = MakeShared<FJsonObject>();
				LineObject->SetStringField(TEXT("kind"), Kind);
				if (BeforeLineNumber > 0)
				{
					LineObject->SetNumberField(TEXT("beforeLine"), BeforeLineNumber);
				}
				LineObject->SetStringField(TEXT("before"), BeforeLine.Left(1000));
				if (AfterLineNumber > 0)
				{
					LineObject->SetNumberField(TEXT("afterLine"), AfterLineNumber);
				}
				LineObject->SetStringField(TEXT("after"), AfterLine.Left(1000));
				OutChangedLines.Add(MakeShared<FJsonValueObject>(LineObject));

				PreviewLines.Add(FString::Printf(TEXT("@@ %s before:%d after:%d @@"), *Kind, BeforeLineNumber, AfterLineNumber));
				if (!BeforeLine.IsEmpty())
				{
					PreviewLines.Add(TEXT("- ") + BeforeLine.Left(1000));
				}
				if (!AfterLine.IsEmpty())
				{
					PreviewLines.Add(TEXT("+ ") + AfterLine.Left(1000));
				}
			};

			auto FindLineForward = [](const TArray<FString>& Lines, const FString& Needle, int32 StartIndex, int32 Lookahead) -> int32
			{
				const int32 EndIndex = FMath::Min(Lines.Num(), StartIndex + Lookahead);
				for (int32 Index = StartIndex; Index < EndIndex; ++Index)
				{
					if (Lines[Index] == Needle)
					{
						return Index;
					}
				}
				return INDEX_NONE;
			};

			const int32 Lookahead = 300;
			int32 BeforeIndex = 0;
			int32 AfterIndex = 0;
			while (BeforeIndex < BeforeLines.Num() || AfterIndex < AfterLines.Num())
			{
				if (BeforeIndex < BeforeLines.Num()
					&& AfterIndex < AfterLines.Num()
					&& BeforeLines[BeforeIndex] == AfterLines[AfterIndex])
				{
					++BeforeIndex;
					++AfterIndex;
					continue;
				}

				bool bHandled = false;
				if (BeforeIndex < BeforeLines.Num() && AfterIndex < AfterLines.Num())
				{
					const int32 MatchingAfterIndex = FindLineForward(AfterLines, BeforeLines[BeforeIndex], AfterIndex + 1, Lookahead);
					if (MatchingAfterIndex != INDEX_NONE)
					{
						for (int32 InsertIndex = AfterIndex; InsertIndex < MatchingAfterIndex; ++InsertIndex)
						{
							AddPreviewLine(TEXT("inserted"), BeforeIndex + 1, InsertIndex + 1, FString(), AfterLines[InsertIndex]);
						}
						AfterIndex = MatchingAfterIndex;
						bHandled = true;
					}
				}

				if (!bHandled && BeforeIndex < BeforeLines.Num() && AfterIndex < AfterLines.Num())
				{
					const int32 MatchingBeforeIndex = FindLineForward(BeforeLines, AfterLines[AfterIndex], BeforeIndex + 1, Lookahead);
					if (MatchingBeforeIndex != INDEX_NONE)
					{
						for (int32 DeleteIndex = BeforeIndex; DeleteIndex < MatchingBeforeIndex; ++DeleteIndex)
						{
							AddPreviewLine(TEXT("deleted"), DeleteIndex + 1, AfterIndex + 1, BeforeLines[DeleteIndex], FString());
						}
						BeforeIndex = MatchingBeforeIndex;
						bHandled = true;
					}
				}

				if (bHandled)
				{
					continue;
				}

				const FString BeforeLine = BeforeLines.IsValidIndex(BeforeIndex) ? BeforeLines[BeforeIndex] : FString();
				const FString AfterLine = AfterLines.IsValidIndex(AfterIndex) ? AfterLines[AfterIndex] : FString();
				AddPreviewLine(
					BeforeLines.IsValidIndex(BeforeIndex) && AfterLines.IsValidIndex(AfterIndex) ? TEXT("changed") : (BeforeLines.IsValidIndex(BeforeIndex) ? TEXT("deleted") : TEXT("inserted")),
					BeforeLines.IsValidIndex(BeforeIndex) ? BeforeIndex + 1 : 0,
					AfterLines.IsValidIndex(AfterIndex) ? AfterIndex + 1 : 0,
					BeforeLine,
					AfterLine);
				if (BeforeLines.IsValidIndex(BeforeIndex))
				{
					++BeforeIndex;
				}
				if (AfterLines.IsValidIndex(AfterIndex))
				{
					++AfterIndex;
				}
			}

			if (bOutTruncated)
			{
				PreviewLines.Add(FString::Printf(TEXT("... truncated after %d changed preview lines ..."), SafeMaxPreviewLines));
			}
			OutPreviewText = FString::Join(PreviewLines, TEXT("\n"));
		}

		TSharedPtr<FJsonObject> MakeTextDiffObject(const FString& BeforeText, const FString& AfterText, int32 MaxPreviewLines)
		{
			TArray<FString> BeforeLines;
			TArray<FString> AfterLines;
			BeforeText.ParseIntoArrayLines(BeforeLines, false);
			AfterText.ParseIntoArrayLines(AfterLines, false);

			TArray<TSharedPtr<FJsonValue>> ChangedLines;
			FString PreviewText;
			int32 ChangedLineCount = 0;
			bool bTruncated = false;
			BuildSimpleLineDiffPreview(BeforeText, AfterText, MaxPreviewLines, ChangedLines, PreviewText, ChangedLineCount, bTruncated);

			TSharedPtr<FJsonObject> DiffObject = MakeShared<FJsonObject>();
			DiffObject->SetNumberField(TEXT("beforeLineCount"), BeforeLines.Num());
			DiffObject->SetNumberField(TEXT("afterLineCount"), AfterLines.Num());
			DiffObject->SetNumberField(TEXT("changedLineCount"), ChangedLineCount);
			DiffObject->SetBoolField(TEXT("hasChanges"), ChangedLineCount > 0);
			DiffObject->SetBoolField(TEXT("truncated"), bTruncated);
			DiffObject->SetStringField(TEXT("previewText"), PreviewText);
			DiffObject->SetArrayField(TEXT("changedLines"), ChangedLines);
			return DiffObject;
		}

		FUnrealMcpExecutionResult DiffLastMcpApply(const FJsonObject& Arguments)
		{
			FString ManifestPath = GetLatestMcpExtensionManifestPath();
			bool bIncludeFullText = false;
			Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
			Arguments.TryGetBoolField(TEXT("includeFullText"), bIncludeFullText);
			const int32 MaxPreviewLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("maxPreviewLines"), 120), 1000);

			FString ResolvedManifestPath;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(ManifestPath, ResolvedManifestPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TSharedPtr<FJsonObject> ManifestObject;
			if (!LoadJsonObjectFromFile(ResolvedManifestPath, ManifestObject, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ToolName;
			FString SourcePath;
			FString BackupSourcePath;
			FString AfterSourcePath;
			FString SourceHashBefore;
			FString SourceHashAfter;
			ManifestObject->TryGetStringField(TEXT("toolName"), ToolName);
			ManifestObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
			ManifestObject->TryGetStringField(TEXT("backupSourcePath"), BackupSourcePath);
			ManifestObject->TryGetStringField(TEXT("afterSourcePath"), AfterSourcePath);
			ManifestObject->TryGetStringField(TEXT("sourceHashBefore"), SourceHashBefore);
			ManifestObject->TryGetStringField(TEXT("sourceHashAfter"), SourceHashAfter);

			if (BackupSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("Manifest is missing backupSourcePath."), nullptr, true);
			}
			if (AfterSourcePath.IsEmpty())
			{
				AfterSourcePath = SourcePath;
			}
			if (AfterSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("Manifest is missing afterSourcePath and sourcePath."), nullptr, true);
			}

			FString ResolvedBeforePath;
			FString ResolvedAfterPath;
			if (!ResolveProjectPathInsideProject(BackupSourcePath, ResolvedBeforePath, FailureReason)
				|| !ResolveProjectPathInsideProject(AfterSourcePath, ResolvedAfterPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString BeforeText;
			FString AfterText;
			if (!FFileHelper::LoadFileToString(BeforeText, *ResolvedBeforePath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read before snapshot '%s'."), *ResolvedBeforePath), nullptr, true);
			}
			if (!FFileHelper::LoadFileToString(AfterText, *ResolvedAfterPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read after snapshot '%s'."), *ResolvedAfterPath), nullptr, true);
			}

			TArray<FString> BeforeLines;
			TArray<FString> AfterLines;
			BeforeText.ParseIntoArrayLines(BeforeLines, false);
			AfterText.ParseIntoArrayLines(AfterLines, false);

			TArray<TSharedPtr<FJsonValue>> ChangedLines;
			FString PreviewText;
			int32 ChangedLineCount = 0;
			bool bTruncated = false;
			BuildSimpleLineDiffPreview(BeforeText, AfterText, MaxPreviewLines, ChangedLines, PreviewText, ChangedLineCount, bTruncated);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_diff_last_apply"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("manifestPath"), ResolvedManifestPath);
			StructuredContent->SetStringField(TEXT("sourcePath"), SourcePath);
			StructuredContent->SetStringField(TEXT("beforePath"), ResolvedBeforePath);
			StructuredContent->SetStringField(TEXT("afterPath"), ResolvedAfterPath);
			StructuredContent->SetStringField(TEXT("sourceHashBefore"), SourceHashBefore);
			StructuredContent->SetStringField(TEXT("sourceHashAfter"), SourceHashAfter);
			StructuredContent->SetStringField(TEXT("computedBeforeHash"), HashTextForManifest(BeforeText));
			StructuredContent->SetStringField(TEXT("computedAfterHash"), HashTextForManifest(AfterText));
			StructuredContent->SetNumberField(TEXT("beforeLineCount"), BeforeLines.Num());
			StructuredContent->SetNumberField(TEXT("afterLineCount"), AfterLines.Num());
			StructuredContent->SetNumberField(TEXT("changedLineCount"), ChangedLineCount);
			StructuredContent->SetBoolField(TEXT("hasChanges"), ChangedLineCount > 0);
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetStringField(TEXT("previewText"), PreviewText);
			StructuredContent->SetArrayField(TEXT("changedLines"), ChangedLines);
			const TArray<TSharedPtr<FJsonValue>>* ManifestChanges = nullptr;
			if (ManifestObject->TryGetArrayField(TEXT("changes"), ManifestChanges) && ManifestChanges)
			{
				StructuredContent->SetArrayField(TEXT("manifestChanges"), *ManifestChanges);
			}
			if (bIncludeFullText)
			{
				StructuredContent->SetStringField(TEXT("beforeText"), BeforeText);
				StructuredContent->SetStringField(TEXT("afterText"), AfterText);
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Last MCP apply diff for %s: changedLineCount=%d truncated=%s."),
					ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName,
					ChangedLineCount,
					bTruncated ? TEXT("true") : TEXT("false")),
				StructuredContent,
				false);
		}

		bool LoadScaffoldSnippet(
			const FString& ScaffoldDirectory,
			const FString& FileName,
			bool bRequired,
			FString& OutSnippet,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutFailureReason)
		{
			const FString SnippetPath = FPaths::Combine(ScaffoldDirectory, FileName);
			if (!FPaths::FileExists(SnippetPath))
			{
				AddAuditIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), SnippetPath, TEXT("Snippet file is missing."));
				if (bRequired)
				{
					OutFailureReason = FString::Printf(TEXT("Required patch fragment is missing: %s"), *SnippetPath);
					return false;
				}
				return true;
			}

			if (!FFileHelper::LoadFileToString(OutSnippet, *SnippetPath))
			{
				AddAuditIssue(Issues, TEXT("error"), SnippetPath, TEXT("Failed to read patch fragment."));
				OutFailureReason = FString::Printf(TEXT("Failed to read patch fragment: %s"), *SnippetPath);
				return false;
			}

			if (OutSnippet.TrimStartAndEnd().IsEmpty())
			{
				AddAuditIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), SnippetPath, TEXT("Patch fragment is empty."));
				if (bRequired)
				{
					OutFailureReason = FString::Printf(TEXT("Required patch fragment is empty: %s"), *SnippetPath);
					return false;
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeTextDiffObject(const FString& BeforeText, const FString& AfterText, int32 MaxPreviewLines);

		bool CanonicalizeScaffoldSnippetName(const FString& SnippetName, FString& OutSnippetName, FString& OutFailureReason)
		{
			const FString CleanName = SnippetName.TrimStartAndEnd();
			if (CleanName.Equals(TEXT("LegacyToolDefinition.legacy.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("ToolDefinition.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("tool_definition"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("definition"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("LegacyToolDefinition.legacy.cpp");
				return true;
			}

			if (CleanName.Equals(TEXT("LegacyExecuteToolHandler.legacy.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("ExecuteToolHandler.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("handler"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("execute"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("LegacyExecuteToolHandler.legacy.cpp");
				return true;
			}

			if (CleanName.Equals(TEXT("ToolRegistrar.patch.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("ToolRegistrar.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("registrar"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("tool_registrar"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("ToolRegistrar.patch.cpp");
				return true;
			}

			if (CleanName.Equals(TEXT("ToolRegistrarCall.patch.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("ToolRegistrarCall.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("registrar_call"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("ToolRegistrarCall.patch.cpp");
				return true;
			}

			if (CleanName.Equals(TEXT("CategoryHandlerFunction.patch.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("CategoryHandlerFunction.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("category_handler"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("handler_function"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("CategoryHandlerFunction.patch.cpp");
				return true;
			}

			if (CleanName.Equals(TEXT("CategoryDispatcherBranch.patch.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("CategoryDispatcherBranch.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("category_dispatcher"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("dispatcher_branch"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("CategoryDispatcherBranch.patch.cpp");
				return true;
			}

			if (CleanName.Equals(TEXT("ChatCommand.patch.cpp"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("ChatCommand.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("chat"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("chat_command"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("ChatCommand.patch.cpp");
				return true;
			}

			OutFailureReason = TEXT("snippetName must be one of ToolRegistrar.patch.cpp, ToolRegistrarCall.patch.cpp, CategoryHandlerFunction.patch.cpp, CategoryDispatcherBranch.patch.cpp, ChatCommand.patch.cpp, or legacy fragments LegacyToolDefinition.legacy.cpp / LegacyExecuteToolHandler.legacy.cpp.");
			return false;
		}

		void AddSnippetIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Code,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> IssueObject = MakeShared<FJsonObject>();
			IssueObject->SetStringField(TEXT("severity"), Severity);
			IssueObject->SetStringField(TEXT("code"), Code);
			IssueObject->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(IssueObject));
		}

		bool ContainsAnyPattern(const FString& Text, const TArray<FString>& Patterns, FString& OutPattern)
		{
			for (const FString& Pattern : Patterns)
			{
				if (Text.Contains(Pattern, ESearchCase::IgnoreCase))
				{
					OutPattern = Pattern;
					return true;
				}
			}
			return false;
		}

		bool TryParseRawStringStart(const FString& Text, int32 Index, FString& OutTerminator, int32& OutContentStart)
		{
			if (Index + 2 >= Text.Len() || Text[Index] != TEXT('R') || Text[Index + 1] != TEXT('"'))
			{
				return false;
			}

			const int32 TagStart = Index + 2;
			int32 ParenIndex = INDEX_NONE;
			for (int32 SearchIndex = TagStart; SearchIndex < Text.Len(); ++SearchIndex)
			{
				const TCHAR Ch = Text[SearchIndex];
				if (Ch == TEXT('('))
				{
					ParenIndex = SearchIndex;
					break;
				}
				if (Ch == TEXT('\\') || Ch == TEXT(')') || Ch == TEXT('"') || Ch == TEXT('\r') || Ch == TEXT('\n')
					|| FChar::IsWhitespace(Ch)
					|| SearchIndex - TagStart > 32)
				{
					return false;
				}
			}

			if (ParenIndex == INDEX_NONE)
			{
				return false;
			}

			const FString Tag = Text.Mid(TagStart, ParenIndex - TagStart);
			OutTerminator = FString::Printf(TEXT(")%s\""), *Tag);
			OutContentStart = ParenIndex + 1;
			return true;
		}

		void AddSnippetNextStep(
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

		TSharedPtr<FJsonObject> ValidateCppSnippetText(
			const FString& SnippetText,
			const FString& SnippetName,
			const FString& ToolName)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			int32 ErrorCount = 0;
			int32 WarningCount = 0;
			auto AddIssue = [&](const FString& Severity, const FString& Code, const FString& Message)
			{
				AddSnippetIssue(Issues, Severity, Code, Message);
				if (Severity == TEXT("error"))
				{
					++ErrorCount;
				}
				else
				{
					++WarningCount;
				}
			};

			const FString CleanSnippetName = SnippetName.TrimStartAndEnd();
			const FString CleanToolName = ToolName.TrimStartAndEnd();
			const FString TrimmedSnippet = SnippetText.TrimStartAndEnd();
			if (TrimmedSnippet.IsEmpty())
			{
				AddIssue(TEXT("error"), TEXT("empty_snippet"), TEXT("Patch fragment text is empty."));
			}
			if (SnippetText.Len() > 50000)
			{
				AddIssue(TEXT("warning"), TEXT("large_snippet"), TEXT("Patch fragment is larger than 50k characters; review before applying."));
			}

			int32 FinalBraceDepth = 0;
			int32 FinalParenDepth = 0;
			int32 FinalBracketDepth = 0;
			{
				bool bInLineComment = false;
				bool bInBlockComment = false;
				bool bInString = false;
				bool bInChar = false;
				bool bInRawString = false;
				bool bEscape = false;
				bool bReportedBraceUnderflow = false;
				bool bReportedParenUnderflow = false;
				bool bReportedBracketUnderflow = false;
				FString RawTerminator;

				for (int32 Index = 0; Index < SnippetText.Len(); ++Index)
				{
					const TCHAR Ch = SnippetText[Index];
					const TCHAR Next = Index + 1 < SnippetText.Len() ? SnippetText[Index + 1] : TEXT('\0');

					if (bInRawString)
					{
						if (!RawTerminator.IsEmpty()
							&& Index + RawTerminator.Len() <= SnippetText.Len()
							&& SnippetText.Mid(Index, RawTerminator.Len()) == RawTerminator)
						{
							bInRawString = false;
							Index += RawTerminator.Len() - 1;
							RawTerminator.Reset();
						}
						continue;
					}

					if (bInLineComment)
					{
						if (Ch == TEXT('\n') || Ch == TEXT('\r'))
						{
							bInLineComment = false;
						}
						continue;
					}

					if (bInBlockComment)
					{
						if (Ch == TEXT('*') && Next == TEXT('/'))
						{
							bInBlockComment = false;
							++Index;
						}
						continue;
					}

					if (bInString)
					{
						if (bEscape)
						{
							bEscape = false;
						}
						else if (Ch == TEXT('\\'))
						{
							bEscape = true;
						}
						else if (Ch == TEXT('"'))
						{
							bInString = false;
						}
						continue;
					}

					if (bInChar)
					{
						if (bEscape)
						{
							bEscape = false;
						}
						else if (Ch == TEXT('\\'))
						{
							bEscape = true;
						}
						else if (Ch == TEXT('\''))
						{
							bInChar = false;
						}
						continue;
					}

					if (Ch == TEXT('/') && Next == TEXT('/'))
					{
						bInLineComment = true;
						++Index;
						continue;
					}

					if (Ch == TEXT('/') && Next == TEXT('*'))
					{
						bInBlockComment = true;
						++Index;
						continue;
					}

					int32 RawContentStart = INDEX_NONE;
					if (TryParseRawStringStart(SnippetText, Index, RawTerminator, RawContentStart))
					{
						bInRawString = true;
						Index = RawContentStart - 1;
						continue;
					}

					if (Ch == TEXT('"'))
					{
						bInString = true;
						continue;
					}

					if (Ch == TEXT('\''))
					{
						bInChar = true;
						continue;
					}

					if (Ch == TEXT('{'))
					{
						++FinalBraceDepth;
					}
					else if (Ch == TEXT('}'))
					{
						--FinalBraceDepth;
						if (FinalBraceDepth < 0 && !bReportedBraceUnderflow)
						{
							AddIssue(TEXT("error"), TEXT("unbalanced_braces"), TEXT("Patch fragment closes more braces than it opens."));
							bReportedBraceUnderflow = true;
						}
					}
					else if (Ch == TEXT('('))
					{
						++FinalParenDepth;
					}
					else if (Ch == TEXT(')'))
					{
						--FinalParenDepth;
						if (FinalParenDepth < 0 && !bReportedParenUnderflow)
						{
							AddIssue(TEXT("error"), TEXT("unbalanced_parentheses"), TEXT("Patch fragment closes more parentheses than it opens."));
							bReportedParenUnderflow = true;
						}
					}
					else if (Ch == TEXT('['))
					{
						++FinalBracketDepth;
					}
					else if (Ch == TEXT(']'))
					{
						--FinalBracketDepth;
						if (FinalBracketDepth < 0 && !bReportedBracketUnderflow)
						{
							AddIssue(TEXT("error"), TEXT("unbalanced_brackets"), TEXT("Patch fragment closes more brackets than it opens."));
							bReportedBracketUnderflow = true;
						}
					}
				}

				if (bInRawString)
				{
					AddIssue(TEXT("error"), TEXT("unterminated_raw_string_literal"), TEXT("Patch fragment contains an unterminated raw string literal. This often means AI output was truncated."));
				}
				if (bInString)
				{
					AddIssue(TEXT("error"), TEXT("unterminated_string_literal"), TEXT("Patch fragment contains an unterminated string literal."));
				}
				if (bInChar)
				{
					AddIssue(TEXT("error"), TEXT("unterminated_char_literal"), TEXT("Patch fragment contains an unterminated character literal."));
				}
				if (bInBlockComment)
				{
					AddIssue(TEXT("error"), TEXT("unterminated_block_comment"), TEXT("Patch fragment contains an unterminated block comment."));
				}
				if (FinalBraceDepth > 0)
				{
					AddIssue(TEXT("error"), TEXT("unbalanced_braces"), TEXT("Patch fragment opens more braces than it closes. This usually means the generated code is incomplete."));
				}
				if (FinalParenDepth > 0)
				{
					AddIssue(TEXT("error"), TEXT("unbalanced_parentheses"), TEXT("Patch fragment opens more parentheses than it closes. This usually means the generated code is incomplete."));
				}
				if (FinalBracketDepth > 0)
				{
					AddIssue(TEXT("error"), TEXT("unbalanced_brackets"), TEXT("Patch fragment opens more brackets than it closes. This usually means the generated code is incomplete."));
				}

				if (!TrimmedSnippet.IsEmpty())
				{
					const TCHAR LastChar = TrimmedSnippet[TrimmedSnippet.Len() - 1];
					if (LastChar == TEXT(',') || LastChar == TEXT('.') || LastChar == TEXT('=') || LastChar == TEXT('+')
						|| LastChar == TEXT('-') || LastChar == TEXT('<') || LastChar == TEXT('>')
						|| LastChar == TEXT(':') || LastChar == TEXT('|') || LastChar == TEXT('&')
						|| LastChar == TEXT('[') || LastChar == TEXT('(') || LastChar == TEXT('{'))
					{
						AddIssue(TEXT("error"), TEXT("truncated_fragment"), TEXT("Patch fragment ends with a dangling token; regenerate or patch the fragment before applying."));
					}
				}
			}

			FString MatchedPattern;
			if (ContainsAnyPattern(SnippetText, {
				TEXT("FPlatformProcess::ExecProcess"),
				TEXT("FPlatformProcess::CreateProc"),
				TEXT(" system("),
				TEXT("\tsystem("),
				TEXT("popen(")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("process_execution"), FString::Printf(TEXT("Snippet contains process execution pattern '%s'."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("IFileManager::Get().Delete"),
				TEXT("DeleteDirectory("),
				TEXT("DeleteDirectoryRecursively"),
				TEXT("FPlatformFileManager::Get().GetPlatformFile().Delete")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("destructive_file_operation"), FString::Printf(TEXT("Snippet contains destructive file operation pattern '%s'."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("FFileHelper::SaveStringToFile"),
				TEXT("FFileHelper::SaveArrayToFile"),
				TEXT("CreateFileWriter("),
				TEXT("std::ofstream")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("file_write_operation"), FString::Printf(TEXT("Snippet contains file write pattern '%s'. Generated tools should route file edits through reviewed MCP utilities."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("/Users/"),
				TEXT("/private/"),
				TEXT("/etc/"),
				TEXT("/tmp/"),
				TEXT("C:\\\\"),
				TEXT("D:\\\\"),
				TEXT("../"),
				TEXT("..\\\\")
			}, MatchedPattern))
			{
				AddIssue(TEXT("warning"), TEXT("external_path_literal"), FString::Printf(TEXT("Snippet contains path-like literal '%s'; verify it cannot write outside the project."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("RunMcpExtensionPipeline("),
				TEXT("TEXT(\"unreal.mcp_extension_pipeline\")"),
				TEXT("ExecuteTool(TEXT(\"unreal.mcp_extension_pipeline\")")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("recursive_pipeline_call"), FString::Printf(TEXT("Snippet contains recursive pipeline call pattern '%s'."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("while (true"),
				TEXT("while(true"),
				TEXT("for (;;"),
				TEXT("for(;;")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("obvious_infinite_loop"), FString::Printf(TEXT("Snippet contains obvious infinite loop pattern '%s'."), *MatchedPattern));
			}

			if (SnippetText.Contains(TEXT("ExecuteTool(ToolName"), ESearchCase::IgnoreCase))
			{
				AddIssue(TEXT("warning"), TEXT("self_dispatch_risk"), TEXT("Snippet forwards ExecuteTool(ToolName, ...); verify this cannot recursively dispatch itself."));
			}
			if (SnippetText.Contains(TEXT("TODO"), ESearchCase::IgnoreCase))
			{
				AddIssue(TEXT("warning"), TEXT("todo_marker"), TEXT("Snippet still contains TODO markers."));
			}
			if (SnippetText.Contains(TEXT("MakeFlexibleObjectProperty"), ESearchCase::IgnoreCase)
				|| SnippetText.Contains(TEXT("additionalProperties"), ESearchCase::IgnoreCase))
			{
				AddIssue(TEXT("warning"), TEXT("schema_flexibility"), TEXT("Snippet may introduce flexible object schema fields; validate OpenAI compatibility before applying."));
			}

			if (CleanSnippetName == TEXT("LegacyExecuteToolHandler.legacy.cpp"))
			{
				if (!SnippetText.Contains(TEXT("return UnrealMcp::MakeExecutionResult"), ESearchCase::CaseSensitive)
					&& !SnippetText.Contains(TEXT("return MakeExecutionResult"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_make_execution_result"), TEXT("Legacy ExecuteTool handler fragment must return UnrealMcp::MakeExecutionResult or MakeExecutionResult."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *CleanToolName), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_tool_name_literal"), TEXT("Legacy ExecuteTool handler fragment does not contain the expected tool name literal."));
				}
			}
			else if (CleanSnippetName == TEXT("LegacyToolDefinition.legacy.cpp"))
			{
				if (!SnippetText.Contains(TEXT("AddToolDefinition"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_add_tool_definition"), TEXT("Legacy tool definition fragment must call UnrealMcp::AddToolDefinition."));
				}
				if (!SnippetText.Contains(TEXT("MakeObjectSchema"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_object_schema"), TEXT("Legacy tool definition fragment should build a fixed object schema with MakeObjectSchema."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *CleanToolName), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_tool_name_literal"), TEXT("Legacy tool definition fragment does not contain the expected tool name literal."));
				}
			}
			else if (CleanSnippetName == TEXT("ToolRegistrar.patch.cpp"))
			{
				if (!SnippetText.Contains(TEXT("FUnrealMcpToolDescriptor"), ESearchCase::CaseSensitive)
					|| !SnippetText.Contains(TEXT("Registrar.Add"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_descriptor_registration"), TEXT("Tool registrar patch must build an FUnrealMcpToolDescriptor and call Registrar.Add."));
				}
				if (!SnippetText.Contains(TEXT("MakeObjectSchema"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_object_schema"), TEXT("Tool registrar patch should build a fixed object schema with MakeObjectSchema."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *CleanToolName), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_tool_name_literal"), TEXT("Tool registrar patch does not contain the expected tool name literal."));
				}
			}
			else if (CleanSnippetName == TEXT("ToolRegistrarCall.patch.cpp"))
			{
				if (!SnippetText.Contains(TEXT("RegisterGenerated"), ESearchCase::CaseSensitive)
					|| !SnippetText.Contains(TEXT("Registrar"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_registrar_call"), TEXT("Tool registrar call patch must call the generated descriptor registration helper."));
				}
			}
			else if (CleanSnippetName == TEXT("CategoryHandlerFunction.patch.cpp"))
			{
				const bool bHasExecutionResult = SnippetText.Contains(TEXT("FUnrealMcpExecutionResult"), ESearchCase::CaseSensitive);
				const bool bHasExecutionReturn =
					SnippetText.Contains(TEXT("return Result"), ESearchCase::CaseSensitive)
					|| SnippetText.Contains(TEXT("return MakeExecutionResult"), ESearchCase::CaseSensitive)
					|| SnippetText.Contains(TEXT("return UnrealMcp::MakeExecutionResult"), ESearchCase::CaseSensitive);
				if (!bHasExecutionResult || !bHasExecutionReturn)
				{
					AddIssue(TEXT("error"), TEXT("missing_execution_result"), TEXT("Category handler function patch must return FUnrealMcpExecutionResult."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *SanitizeMcpToolIdForPath(CleanToolName)), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_action_literal"), TEXT("Category handler function patch does not contain the expected sanitized action literal."));
				}
			}
			else if (CleanSnippetName == TEXT("CategoryDispatcherBranch.patch.cpp"))
			{
				if (!SnippetText.Contains(TEXT("OutResult"), ESearchCase::CaseSensitive)
					|| !SnippetText.Contains(TEXT("return true"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_dispatch_result"), TEXT("Category dispatcher branch patch must assign OutResult and return true."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *CleanToolName), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_tool_name_literal"), TEXT("Category dispatcher branch patch does not contain the expected tool name literal."));
				}
			}
			else if (CleanSnippetName == TEXT("ChatCommand.patch.cpp"))
			{
				if (!SnippetText.Contains(TEXT("ExecuteTool"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_execute_tool"), TEXT("Chat command patch does not call ExecuteTool."));
				}
				if (!SnippetText.Contains(TEXT("return"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_return"), TEXT("Chat command patch does not return a result."));
				}
			}

			TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
			ResultObject->SetStringField(TEXT("snippetName"), CleanSnippetName);
			ResultObject->SetStringField(TEXT("toolName"), CleanToolName);
			ResultObject->SetBoolField(TEXT("safe"), ErrorCount == 0);
			ResultObject->SetNumberField(TEXT("errorCount"), ErrorCount);
			ResultObject->SetNumberField(TEXT("warningCount"), WarningCount);
			ResultObject->SetNumberField(TEXT("characterCount"), SnippetText.Len());
			TSharedPtr<FJsonObject> StructuralObject = MakeShared<FJsonObject>();
			StructuralObject->SetNumberField(TEXT("braceDepth"), FinalBraceDepth);
			StructuralObject->SetNumberField(TEXT("parenDepth"), FinalParenDepth);
			StructuralObject->SetNumberField(TEXT("bracketDepth"), FinalBracketDepth);
			StructuralObject->SetBoolField(TEXT("balanced"), FinalBraceDepth == 0 && FinalParenDepth == 0 && FinalBracketDepth == 0);
			ResultObject->SetObjectField(TEXT("structuralCheck"), StructuralObject);
			ResultObject->SetArrayField(TEXT("issues"), Issues);
			return ResultObject;
		}

		FUnrealMcpExecutionResult ValidateCppSnippet(const FJsonObject& Arguments)
		{
			FString SnippetText;
			FString SnippetName = TEXT("ToolRegistrar.patch.cpp");
			FString ToolName;
			FString ScaffoldDir;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("patchText"), SnippetText);
			Arguments.TryGetStringField(TEXT("snippetText"), SnippetText);
			Arguments.TryGetStringField(TEXT("patchName"), SnippetName);
			Arguments.TryGetStringField(TEXT("snippetName"), SnippetName);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);

			FString CanonicalSnippetName;
			FString FailureReason;
			if (!CanonicalizeScaffoldSnippetName(SnippetName, CanonicalSnippetName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString Source = TEXT("snippetText");
			FString SnippetPath;
			FToolsReadResolution ScaffoldResolution;
			if (SnippetText.TrimStartAndEnd().IsEmpty())
			{
				TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
				ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
				ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
				FString ResolvedScaffoldDir;
				FString ResolvedToolName;
				if (!ResolveMcpScaffoldDirectory(*ResolveArguments, ResolvedScaffoldDir, ResolvedToolName, FailureReason, &ScaffoldResolution))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				if (ToolName.TrimStartAndEnd().IsEmpty())
				{
					ToolName = ResolvedToolName;
				}
				SnippetPath = FPaths::Combine(ResolvedScaffoldDir, CanonicalSnippetName);
				Source = TEXT("scaffoldFile");
				if (!FFileHelper::LoadFileToString(SnippetText, *SnippetPath))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to read patch fragment '%s'."), *SnippetPath), nullptr, true);
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = ValidateCppSnippetText(SnippetText, CanonicalSnippetName, ToolName);
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_validate_cpp_patch"));
			StructuredContent->SetStringField(TEXT("source"), Source);
			if (!SnippetPath.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("patchPath"), SnippetPath);
				StructuredContent->SetStringField(TEXT("snippetPath"), SnippetPath);
				StructuredContent->SetBoolField(TEXT("scaffoldFound"), ScaffoldResolution.bFound);
				StructuredContent->SetStringField(TEXT("scaffoldSourceKind"), LexToString(ScaffoldResolution.SourceKind));
				StructuredContent->SetArrayField(TEXT("scaffoldCandidates"), MakeToolsReadCandidateValues(ScaffoldResolution));
				if (!ScaffoldResolution.Warning.IsEmpty())
				{
					StructuredContent->SetStringField(TEXT("scaffoldResolutionWarning"), ScaffoldResolution.Warning);
				}
			}
			StructuredContent->SetStringField(TEXT("patchName"), CanonicalSnippetName);

			const bool bSafe = StructuredContent->GetBoolField(TEXT("safe"));
			return MakeExecutionResult(
				FString::Printf(TEXT("C++ patch validation for %s safe=%s errors=%d warnings=%d."),
					*CanonicalSnippetName,
					bSafe ? TEXT("true") : TEXT("false"),
					static_cast<int32>(StructuredContent->GetNumberField(TEXT("errorCount"))),
					static_cast<int32>(StructuredContent->GetNumberField(TEXT("warningCount")))),
				StructuredContent,
				!bSafe);
		}

		FUnrealMcpExecutionResult PatchScaffoldSnippet(const FJsonObject& Arguments)
		{
			FString SnippetName;
			Arguments.TryGetStringField(TEXT("patchName"), SnippetName);
			Arguments.TryGetStringField(TEXT("snippetName"), SnippetName);
			if (SnippetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("patchName is required."), nullptr, true);
			}

			FString CanonicalSnippetName;
			FString FailureReason;
			if (!CanonicalizeScaffoldSnippetName(SnippetName, CanonicalSnippetName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ToolName;
			FString ScaffoldDir;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);

			FString ResolvedScaffoldDir;
			FString ResolvedToolName;
			if (!ScaffoldDir.TrimStartAndEnd().IsEmpty())
			{
				if (!ResolveProjectPathInsideProject(ScaffoldDir, ResolvedScaffoldDir, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				ResolvedToolName = ToolName;
			}
			else
			{
				if (ToolName.TrimStartAndEnd().IsEmpty())
				{
					return MakeExecutionResult(TEXT("Provide toolName or scaffoldDir."), nullptr, true);
				}
				FString ResolvedOutputRoot;
				if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				ResolvedScaffoldDir = FPaths::Combine(ResolvedOutputRoot, SanitizeMcpToolIdForPath(ToolName));
				ResolvedToolName = ToolName;
			}
			if (ToolName.TrimStartAndEnd().IsEmpty())
			{
				ToolName = ResolvedToolName;
			}

			const FString SnippetPath = FPaths::Combine(ResolvedScaffoldDir, CanonicalSnippetName);
			FString BeforeText;
			if (!FFileHelper::LoadFileToString(BeforeText, *SnippetPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read patch fragment '%s'."), *SnippetPath), nullptr, true);
			}

			FString Mode;
			FString NewText;
			FString FindText;
			FString ReplaceText;
			FString AppendText;
			FString PrependText;
			bool bDryRun = true;
			bool bCreateBackup = true;
			bool bReplaceAll = false;
			bool bAllowUnsafe = false;
			Arguments.TryGetStringField(TEXT("mode"), Mode);
			Arguments.TryGetStringField(TEXT("newText"), NewText);
			Arguments.TryGetStringField(TEXT("findText"), FindText);
			Arguments.TryGetStringField(TEXT("replaceText"), ReplaceText);
			Arguments.TryGetStringField(TEXT("appendText"), AppendText);
			Arguments.TryGetStringField(TEXT("prependText"), PrependText);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);
			Arguments.TryGetBoolField(TEXT("replaceAll"), bReplaceAll);
			Arguments.TryGetBoolField(TEXT("allowUnsafe"), bAllowUnsafe);
			const int32 DiffPreviewLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("diffPreviewLines"), 120), 1000);

			Mode = Mode.TrimStartAndEnd().ToLower();
			if (Mode.IsEmpty())
			{
				if (!FindText.IsEmpty())
				{
					Mode = TEXT("replace_text");
				}
				else if (!AppendText.IsEmpty())
				{
					Mode = TEXT("append");
				}
				else if (!PrependText.IsEmpty())
				{
					Mode = TEXT("prepend");
				}
				else
				{
					Mode = TEXT("replace_all");
				}
			}

			FString AfterText = BeforeText;
			bool bAlreadyApplied = false;
			if (Mode == TEXT("replace_all"))
			{
				AfterText = NewText;
			}
			else if (Mode == TEXT("replace_text"))
			{
				if (FindText.IsEmpty())
				{
					return MakeExecutionResult(TEXT("findText is required when mode=replace_text."), nullptr, true);
				}
				if (!AfterText.Contains(FindText, ESearchCase::CaseSensitive))
				{
					if (!ReplaceText.IsEmpty() && AfterText.Contains(ReplaceText, ESearchCase::CaseSensitive))
					{
						bAlreadyApplied = true;
					}
					else
					{
						return MakeExecutionResult(TEXT("findText was not found and replaceText does not already appear in the patch fragment."), nullptr, true);
					}
				}
				else if (bReplaceAll)
				{
					AfterText.ReplaceInline(*FindText, *ReplaceText, ESearchCase::CaseSensitive);
				}
				else
				{
					const int32 Index = AfterText.Find(FindText, ESearchCase::CaseSensitive);
					AfterText = AfterText.Left(Index) + ReplaceText + AfterText.Mid(Index + FindText.Len());
				}
			}
			else if (Mode == TEXT("append"))
			{
				if (AppendText.IsEmpty())
				{
					return MakeExecutionResult(TEXT("appendText is required when mode=append."), nullptr, true);
				}
				if (AfterText.Contains(AppendText, ESearchCase::CaseSensitive))
				{
					bAlreadyApplied = true;
				}
				else
				{
					AfterText += (AfterText.EndsWith(TEXT("\n")) ? FString() : FString(TEXT("\n"))) + AppendText;
				}
			}
			else if (Mode == TEXT("prepend"))
			{
				if (PrependText.IsEmpty())
				{
					return MakeExecutionResult(TEXT("prependText is required when mode=prepend."), nullptr, true);
				}
				if (AfterText.Contains(PrependText, ESearchCase::CaseSensitive))
				{
					bAlreadyApplied = true;
				}
				else
				{
					AfterText = PrependText + (PrependText.EndsWith(TEXT("\n")) ? FString() : FString(TEXT("\n"))) + AfterText;
				}
			}
			else
			{
				return MakeExecutionResult(TEXT("mode must be replace_all, replace_text, append, or prepend."), nullptr, true);
			}

			const bool bChanged = BeforeText != AfterText;
			TSharedPtr<FJsonObject> ValidationObject = ValidateCppSnippetText(AfterText, CanonicalSnippetName, ToolName);
			const bool bSafe = ValidationObject->GetBoolField(TEXT("safe"));
			TSharedPtr<FJsonObject> DiffObject = MakeTextDiffObject(BeforeText, AfterText, DiffPreviewLines);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_patch_scaffold_patch"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
			StructuredContent->SetStringField(TEXT("patchName"), CanonicalSnippetName);
			StructuredContent->SetStringField(TEXT("snippetName"), CanonicalSnippetName);
			StructuredContent->SetStringField(TEXT("patchPath"), SnippetPath);
			StructuredContent->SetStringField(TEXT("snippetPath"), SnippetPath);
			StructuredContent->SetStringField(TEXT("mode"), Mode);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("changed"), bChanged);
			StructuredContent->SetBoolField(TEXT("alreadyApplied"), bAlreadyApplied || !bChanged);
			StructuredContent->SetBoolField(TEXT("createBackup"), bCreateBackup);
			StructuredContent->SetBoolField(TEXT("allowUnsafe"), bAllowUnsafe);
			StructuredContent->SetStringField(TEXT("beforeHash"), HashTextForManifest(BeforeText));
			StructuredContent->SetStringField(TEXT("afterHash"), HashTextForManifest(AfterText));
			StructuredContent->SetObjectField(TEXT("validation"), ValidationObject);
			StructuredContent->SetObjectField(TEXT("snippetDiff"), DiffObject);
			TSharedPtr<FJsonObject> RegistrationStateObject = MakeShared<FJsonObject>();
			RegistrationStateObject->SetStringField(TEXT("state"), TEXT("patch_fragment_only"));
			RegistrationStateObject->SetBoolField(TEXT("registeredUsableNow"), false);
			RegistrationStateObject->SetBoolField(TEXT("requiresApply"), true);
			RegistrationStateObject->SetBoolField(TEXT("requiresBuildRestart"), true);
			RegistrationStateObject->SetStringField(TEXT("reason"), TEXT("Editing a scaffold patch only changes the scaffold draft. The tool is not registered until mcp_apply_scaffold applies descriptor, handler, dispatcher, and registry changes, then the editor is rebuilt and restarted."));
			StructuredContent->SetObjectField(TEXT("registrationState"), RegistrationStateObject);

			TArray<TSharedPtr<FJsonValue>> NextSteps;
			if (!bSafe)
			{
				AddSnippetNextStep(NextSteps, TEXT("Fix this patch fragment until validation safe=true."), TEXT("unreal.mcp_patch_scaffold_patch"), TEXT("Unsafe or truncated fragments are blocked from source integration."));
				AddSnippetNextStep(NextSteps, TEXT("Re-run patch validation on the scaffold file."), TEXT("unreal.mcp_validate_cpp_patch"), TEXT("Confirms the fragment is syntactically complete enough for apply."));
			}
			else if (bDryRun)
			{
				AddSnippetNextStep(NextSteps, TEXT("Write the patch edit with dryRun=false after reviewing snippetDiff."), TEXT("unreal.mcp_patch_scaffold_patch"), TEXT("Dry run does not modify the scaffold file."));
			}
			else
			{
				AddSnippetNextStep(NextSteps, TEXT("Preview descriptor-first integration with dryRun=true."), TEXT("unreal.mcp_apply_scaffold"), TEXT("Checks registrar, handler, dispatcher, and registry insertion points."));
				AddSnippetNextStep(NextSteps, TEXT("Apply, build, restart, then run the generated test."), TEXT("unreal.mcp_apply_scaffold"), TEXT("A scaffold patch edit alone never makes a new MCP tool visible."));
			}
			StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);

			if (!bSafe && !bAllowUnsafe)
			{
				return MakeExecutionResult(TEXT("Patched fragment failed static safety validation. Pass allowUnsafe=true only after manual review."), StructuredContent, true);
			}

			if (bDryRun || !bChanged)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("%s patch-fragment edit for %s changed=%s safe=%s."),
						bDryRun ? TEXT("Dry run") : TEXT("No-op"),
						*CanonicalSnippetName,
						bChanged ? TEXT("true") : TEXT("false"),
						bSafe ? TEXT("true") : TEXT("false")),
					StructuredContent,
					false);
			}

			FString BackupDirectory;
			FString BackupBeforePath;
			FString BackupAfterPath;
			if (bCreateBackup)
			{
				const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
				const FString SnippetId = FPaths::GetBaseFilename(CanonicalSnippetName).Replace(TEXT("."), TEXT("_"));
				BackupDirectory = FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("SnippetBackups"), Timestamp + TEXT("_") + SanitizeMcpToolIdForPath(ToolName) + TEXT("_") + SnippetId);
				BackupBeforePath = FPaths::Combine(BackupDirectory, CanonicalSnippetName + TEXT(".before"));
				BackupAfterPath = FPaths::Combine(BackupDirectory, CanonicalSnippetName + TEXT(".after"));
				if (!IFileManager::Get().MakeDirectory(*BackupDirectory, true)
					|| !FFileHelper::SaveStringToFile(BeforeText, *BackupBeforePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					|| !FFileHelper::SaveStringToFile(AfterText, *BackupAfterPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to create patch-fragment backup under '%s'."), *BackupDirectory), StructuredContent, true);
				}
			}

			if (!FFileHelper::SaveStringToFile(AfterText, *SnippetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to write patch fragment '%s'."), *SnippetPath), StructuredContent, true);
			}

			if (bCreateBackup)
			{
				TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
				ManifestObject->SetStringField(TEXT("action"), TEXT("mcp_patch_scaffold_patch"));
				ManifestObject->SetStringField(TEXT("toolName"), ToolName);
				ManifestObject->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
				ManifestObject->SetStringField(TEXT("snippetName"), CanonicalSnippetName);
				ManifestObject->SetStringField(TEXT("snippetPath"), SnippetPath);
				ManifestObject->SetStringField(TEXT("backupDirectory"), BackupDirectory);
				ManifestObject->SetStringField(TEXT("backupBeforePath"), BackupBeforePath);
				ManifestObject->SetStringField(TEXT("backupAfterPath"), BackupAfterPath);
				ManifestObject->SetStringField(TEXT("beforeHash"), HashTextForManifest(BeforeText));
				ManifestObject->SetStringField(TEXT("afterHash"), HashTextForManifest(AfterText));
				ManifestObject->SetStringField(TEXT("patchedAtUtc"), FDateTime::UtcNow().ToIso8601());
				ManifestObject->SetObjectField(TEXT("validation"), ValidationObject);
				FString ManifestFailure;
				const FString ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure))
				{
					return MakeExecutionResult(ManifestFailure, StructuredContent, true);
				}
				StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);
				StructuredContent->SetStringField(TEXT("backupBeforePath"), BackupBeforePath);
				StructuredContent->SetStringField(TEXT("backupAfterPath"), BackupAfterPath);
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Patched %s for %s. Backup: %s"), *CanonicalSnippetName, *ToolName, BackupDirectory.IsEmpty() ? TEXT("<none>") : *BackupDirectory),
				StructuredContent,
				false);
		}



}
