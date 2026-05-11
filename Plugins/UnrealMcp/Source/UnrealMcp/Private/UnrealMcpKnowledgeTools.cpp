#include "UnrealMcpSelfExtensionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealMcpKnowledgeBridge.h"
#include "UnrealMcpToolRegistrar.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values);
	bool TryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues);
	FString LexToString(EToolRiskLevel RiskLevel);

	namespace
	{
		static constexpr int32 DefaultKnowledgeMaxCards = 2000;
		static constexpr int32 DefaultKnowledgeChunkChars = 1800;
		static constexpr int32 DefaultKnowledgeOverlapChars = 160;
		static constexpr int32 DefaultKnowledgeSearchLimit = 8;
		static constexpr int32 DefaultKnowledgeExcerptChars = 420;

		struct FKnowledgeCard
		{
			FString CardId;
			FString SourceId;
			FString Title;
			FString SectionTitle;
			FString SectionPath;
			FString Category;
			TArray<FString> Tags;
			FString SourceKind;
			FString SourcePath;
			FString Url;
			FString Text;
			int32 ChunkIndex = 0;
			int32 TextLength = 0;
			double SourceWeight = 1.0;
			double Confidence = 0.75;
			FString UpdatedAt;
		};

			struct FKnowledgeSection
			{
				FString Title;
				FString Path;
				FString Text;
				int32 SectionIndex = 0;
			};

			struct FKnowledgeCardCacheState
			{
				FString IndexFilePath;
				int64 FileSize = -1;
				FDateTime FileTimestamp;
				TArray<FKnowledgeCard> Cards;
				bool bValid = false;
			};

			struct FActivityLogIndexEvent
			{
				FString SessionId;
				FString TaskLabel;
				FString EventKind;
				FString Summary;
				FString SourcePath;
			};

			FCriticalSection GKnowledgeCardCacheMutex;
			FKnowledgeCardCacheState GKnowledgeCardCache;

		FString GetKnowledgeIndexRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/KnowledgeIndex")));
		}

		FString GetKnowledgeSourceRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/KnowledgeSources")));
		}

		FString NormalizePathForJson(const FString& Path)
		{
			FString Normalized = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(Normalized);
			return Normalized;
		}

		FString ResolveProjectPathForJson(const FString& Path)
		{
			FString Resolved = Path;
			if (FPaths::IsRelative(Resolved))
			{
				Resolved = FPaths::Combine(FPaths::ProjectDir(), Resolved);
			}
			return NormalizePathForJson(Resolved);
		}

		FString MakeProjectRelativePath(const FString& Path)
		{
			FString FullPath = NormalizePathForJson(Path);
			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeDirectoryName(ProjectDir);
			if (!ProjectDir.EndsWith(TEXT("/")))
			{
				ProjectDir += TEXT("/");
			}
			FString Relative = FullPath;
			if (FPaths::MakePathRelativeTo(Relative, *ProjectDir))
			{
				return Relative;
			}
			return FullPath;
		}

		bool LoadJsonObjectFromString(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
		}

		bool WriteJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& Path, FString& OutFailureReason)
		{
			if (!Object.IsValid())
			{
				OutFailureReason = TEXT("Cannot write an invalid JSON object.");
				return false;
			}

			FString JsonText;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
			if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				OutFailureReason = TEXT("Failed to serialize JSON object.");
				return false;
			}

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!FFileHelper::SaveStringToFile(JsonText, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write '%s'."), *Path);
				return false;
			}
			return true;
		}

		FString JsonObjectToCondensedString(const TSharedPtr<FJsonObject>& Object)
		{
			FString JsonText;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
			return JsonText;
		}

		FString SanitizeKnowledgeId(const FString& Value)
		{
			FString Output;
			for (TCHAR Character : Value.ToLower())
			{
				if ((Character >= TEXT('a') && Character <= TEXT('z'))
					|| (Character >= TEXT('0') && Character <= TEXT('9'))
					|| Character == TEXT('_')
					|| Character == TEXT('-')
					|| Character == TEXT('.'))
				{
					Output.AppendChar(Character);
				}
				else
				{
					Output.AppendChar(TEXT('-'));
				}
			}

			while (Output.Contains(TEXT("--")))
			{
				Output.ReplaceInline(TEXT("--"), TEXT("-"));
			}
			Output.TrimStartAndEndInline();
			bool bTrimmedTrailingDash = false;
			Output.TrimCharInline(static_cast<TCHAR>('-'), &bTrimmedTrailingDash);
			return Output.IsEmpty() ? TEXT("knowledge-card") : Output;
		}

		TArray<FString> ExtractSearchTokens(const FString& Text)
		{
			TArray<FString> Tokens;
			FString Current;
			for (TCHAR Character : Text.ToLower())
			{
				if (FChar::IsAlnum(Character) || Character == TEXT('_'))
				{
					Current.AppendChar(Character);
				}
				else
				{
					if (Current.Len() >= 2)
					{
						Tokens.AddUnique(Current);
					}
					Current.Reset();
				}
			}
			if (Current.Len() >= 2)
			{
				Tokens.AddUnique(Current);
			}
			return Tokens;
		}

		void AddSynonymGroup(const FString& QueryLower, const TArray<FString>& Group, TArray<FString>& Tokens)
		{
			bool bMatched = false;
			for (const FString& Term : Group)
			{
				if (QueryLower.Contains(Term.ToLower()))
				{
					bMatched = true;
					break;
				}
			}

			if (!bMatched)
			{
				for (const FString& Token : Tokens)
				{
					for (const FString& Term : Group)
					{
						if (Token.Equals(Term, ESearchCase::IgnoreCase))
						{
							bMatched = true;
							break;
						}
					}
					if (bMatched)
					{
						break;
					}
				}
			}

			if (!bMatched)
			{
				return;
			}

			for (const FString& Term : Group)
			{
				Tokens.AddUnique(Term.ToLower());
			}
		}

		TArray<FString> ExpandSearchTokens(const FString& Query)
		{
			TArray<FString> Tokens = ExtractSearchTokens(Query);
			const FString QueryLower = Query.ToLower();

			const TArray<TArray<FString>> SynonymGroups = {
				{ TEXT("blueprint"), TEXT("bp"), TEXT("蓝图"), TEXT("藍圖"), TEXT("graph"), TEXT("node"), TEXT("pin") },
				{ TEXT("widget"), TEXT("umg"), TEXT("hud"), TEXT("ui"), TEXT("控件"), TEXT("界面"), TEXT("用户界面"), TEXT("使用者介面") },
				{ TEXT("actor"), TEXT("actors"), TEXT("spawn"), TEXT("level"), TEXT("map"), TEXT("world"), TEXT("场景"), TEXT("地圖"), TEXT("地图"), TEXT("生成"), TEXT("放置"), TEXT("摆放") },
				{ TEXT("mcp"), TEXT("tool"), TEXT("tools"), TEXT("工具"), TEXT("自拓展"), TEXT("自扩展"), TEXT("自進化"), TEXT("自进化"), TEXT("scaffold") },
				{ TEXT("rag"), TEXT("knowledge"), TEXT("docs"), TEXT("document"), TEXT("文档"), TEXT("文件"), TEXT("知识库"), TEXT("知識庫"), TEXT("检索"), TEXT("搜尋"), TEXT("搜索") },
				{ TEXT("build"), TEXT("compile"), TEXT("ubt"), TEXT("编译"), TEXT("構建"), TEXT("构建"), TEXT("打包") },
				{ TEXT("test"), TEXT("tests"), TEXT("verify"), TEXT("验证"), TEXT("驗證"), TEXT("测试"), TEXT("測試") },
				{ TEXT("memory"), TEXT("skill"), TEXT("skills"), TEXT("记忆"), TEXT("記憶"), TEXT("技能"), TEXT("经验"), TEXT("經驗") },
				{ TEXT("workflow"), TEXT("recipe"), TEXT("pipeline"), TEXT("流程"), TEXT("工作流"), TEXT("组合"), TEXT("組合") }
			};

			for (const TArray<FString>& Group : SynonymGroups)
			{
				AddSynonymGroup(QueryLower, Group, Tokens);
			}

			return Tokens;
		}

		double SourceWeightForKind(const FString& SourceKind, const FString& Category)
		{
			if (SourceKind.Equals(TEXT("tool-registry"), ESearchCase::IgnoreCase))
			{
				return 2.2;
			}
			if (SourceKind.Equals(TEXT("versioned-doc"), ESearchCase::IgnoreCase))
			{
				return Category.Equals(TEXT("uevolve-docs"), ESearchCase::IgnoreCase) ? 1.7 : 1.45;
			}
			if (SourceKind.Equals(TEXT("skill"), ESearchCase::IgnoreCase))
			{
				return 1.8;
			}
			if (SourceKind.Equals(TEXT("official-docs"), ESearchCase::IgnoreCase))
			{
				return 1.15;
			}
			if (SourceKind.Equals(TEXT("runtime-memory"), ESearchCase::IgnoreCase))
			{
				return 1.25;
			}
			return 1.0;
		}

			double ConfidenceForKind(const FString& SourceKind)
			{
				if (SourceKind.Equals(TEXT("tool-registry"), ESearchCase::IgnoreCase))
				{
					return 0.95;
			}
			if (SourceKind.Equals(TEXT("versioned-doc"), ESearchCase::IgnoreCase))
			{
				return 0.88;
			}
			if (SourceKind.Equals(TEXT("skill"), ESearchCase::IgnoreCase))
			{
				return 0.86;
			}
			if (SourceKind.Equals(TEXT("official-docs"), ESearchCase::IgnoreCase))
			{
				return 0.82;
				}
				return 0.72;
			}

			TArray<FString> GetKnowledgeSourceKindEnumValues()
			{
				return {
					TEXT("tool-registry"),
					TEXT("versioned-doc"),
					TEXT("official-docs"),
					TEXT("skill"),
					TEXT("runtime-memory"),
					TEXT("activity-log"),
					TEXT("test-fixture"),
					TEXT("unknown")
				};
			}

			bool TryCanonicalizeKnowledgeSourceKind(const FString& Value, FString& OutKind)
			{
				const FString Trimmed = Value.TrimStartAndEnd();
				for (const FString& AllowedKind : GetKnowledgeSourceKindEnumValues())
				{
					if (Trimmed.Equals(AllowedKind, ESearchCase::IgnoreCase))
					{
						OutKind = AllowedKind;
						return true;
					}
				}
				return false;
			}

			FString MakeAllowedKnowledgeSourceKindsText()
			{
				const TArray<FString> AllowedKinds = GetKnowledgeSourceKindEnumValues();
				return FString::Printf(TEXT("[%s]"), *FString::Join(AllowedKinds, TEXT(", ")));
			}

			bool ValidateSourceKindFilters(const TArray<FString>& RawFilters, TArray<FString>& OutFilters, FString& OutFailureReason)
			{
				OutFilters.Reset();
				for (const FString& RawFilter : RawFilters)
				{
					FString CanonicalKind;
					if (!TryCanonicalizeKnowledgeSourceKind(RawFilter, CanonicalKind))
					{
						OutFailureReason = FString::Printf(
							TEXT("Invalid sourceKind: %s. Allowed: %s"),
							*RawFilter,
							*MakeAllowedKnowledgeSourceKindsText());
						return false;
					}
					OutFilters.AddUnique(CanonicalKind);
				}
				return true;
			}

			bool SourceKindHasActiveIndexer(const FString& SourceKind)
			{
				return SourceKind.Equals(TEXT("versioned-doc"), ESearchCase::IgnoreCase)
					|| SourceKind.Equals(TEXT("official-docs"), ESearchCase::IgnoreCase)
					|| SourceKind.Equals(TEXT("tool-registry"), ESearchCase::IgnoreCase)
					|| SourceKind.Equals(TEXT("activity-log"), ESearchCase::IgnoreCase)
					|| SourceKind.Equals(TEXT("skill"), ESearchCase::IgnoreCase);
			}

			TSharedPtr<FJsonObject> MakeKindStatusObject(const TArray<FKnowledgeCard>& Cards)
			{
				TMap<FString, int32> CountsByKind;
				const TArray<FString> AllowedKinds = GetKnowledgeSourceKindEnumValues();
				for (const FString& AllowedKind : AllowedKinds)
				{
					CountsByKind.Add(AllowedKind, 0);
				}

				for (const FKnowledgeCard& Card : Cards)
				{
					FString CanonicalKind;
					if (TryCanonicalizeKnowledgeSourceKind(Card.SourceKind, CanonicalKind))
					{
						CountsByKind.FindOrAdd(CanonicalKind)++;
					}
					else if (!Card.SourceKind.TrimStartAndEnd().IsEmpty())
					{
						CountsByKind.FindOrAdd(Card.SourceKind.TrimStartAndEnd())++;
					}
				}

				TSharedPtr<FJsonObject> StatusObject = MakeShared<FJsonObject>();
				for (const FString& AllowedKind : AllowedKinds)
				{
					const int32 Count = CountsByKind.FindRef(AllowedKind);
					if (Count > 0)
					{
						StatusObject->SetStringField(AllowedKind, TEXT("active"));
					}
					else if (SourceKindHasActiveIndexer(AllowedKind))
					{
						StatusObject->SetStringField(AllowedKind, TEXT("active-empty"));
					}
					else
					{
						StatusObject->SetStringField(AllowedKind, TEXT("reserved-not-active"));
					}
				}

				TArray<FString> CountKeys;
				CountsByKind.GetKeys(CountKeys);
				CountKeys.Sort();
				for (const FString& CountKey : CountKeys)
				{
					FString CanonicalKind;
					if (!TryCanonicalizeKnowledgeSourceKind(CountKey, CanonicalKind) && CountsByKind.FindRef(CountKey) > 0)
					{
						StatusObject->SetStringField(CountKey, TEXT("unknown"));
					}
				}
				return StatusObject;
			}

		void FlushKnowledgeSection(
			const FString& BaseTitle,
			const TArray<FString>& HeadingStack,
			const TArray<FString>& Lines,
			TArray<FKnowledgeSection>& OutSections)
		{
			const FString Text = FString::Join(Lines, TEXT("\n")).TrimStartAndEnd();
			if (Text.IsEmpty())
			{
				return;
			}

			FKnowledgeSection Section;
			Section.SectionIndex = OutSections.Num();
			Section.Title = HeadingStack.Num() > 0 ? HeadingStack.Last() : BaseTitle;
			Section.Path = HeadingStack.Num() > 0 ? FString::Join(HeadingStack, TEXT(" > ")) : BaseTitle;
			Section.Text = Text;
			OutSections.Add(MoveTemp(Section));
		}

			TArray<FKnowledgeSection> SplitTextIntoSections(const FString& BaseTitle, const FString& Text)
			{
				TArray<FKnowledgeSection> Sections;
				TArray<FString> Lines;
				Text.ParseIntoArrayLines(Lines, false);

			TArray<FString> HeadingStack;
			TArray<FString> CurrentLines;
			for (const FString& Line : Lines)
			{
				const FString Trimmed = Line.TrimStartAndEnd();
				int32 HashCount = 0;
				while (HashCount < Trimmed.Len() && Trimmed[HashCount] == TEXT('#'))
				{
					HashCount++;
				}

				const bool bMarkdownHeading =
					HashCount > 0
					&& HashCount <= 6
					&& Trimmed.Len() > HashCount
					&& FChar::IsWhitespace(Trimmed[HashCount]);

				if (bMarkdownHeading)
				{
					FlushKnowledgeSection(BaseTitle, HeadingStack, CurrentLines, Sections);
					CurrentLines.Reset();

					FString Heading = Trimmed.Mid(HashCount).TrimStartAndEnd();
					while (HeadingStack.Num() >= HashCount)
					{
						HeadingStack.Pop(EAllowShrinking::No);
					}
					HeadingStack.Add(Heading.IsEmpty() ? BaseTitle : Heading);
					CurrentLines.Add(Trimmed);
					continue;
				}

				CurrentLines.Add(Line);
			}

			FlushKnowledgeSection(BaseTitle, HeadingStack, CurrentLines, Sections);
			if (Sections.IsEmpty() && !Text.TrimStartAndEnd().IsEmpty())
			{
				FKnowledgeSection Section;
				Section.Title = BaseTitle;
				Section.Path = BaseTitle;
				Section.Text = Text.TrimStartAndEnd();
				Sections.Add(MoveTemp(Section));
				}
				return Sections;
			}

			FString ExtractFirstMarkdownHeading(const FString& Text, const FString& FallbackTitle)
			{
				TArray<FString> Lines;
				Text.ParseIntoArrayLines(Lines, false);
				for (const FString& Line : Lines)
				{
					const FString Trimmed = Line.TrimStartAndEnd();
					int32 HashCount = 0;
					while (HashCount < Trimmed.Len() && Trimmed[HashCount] == TEXT('#'))
					{
						HashCount++;
					}

					if (HashCount > 0
						&& HashCount <= 6
						&& Trimmed.Len() > HashCount
						&& FChar::IsWhitespace(Trimmed[HashCount]))
					{
						const FString Heading = Trimmed.Mid(HashCount).TrimStartAndEnd();
						return Heading.IsEmpty() ? FallbackTitle : Heading;
					}
				}
				return FallbackTitle;
			}

		TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Values)
		{
			return MakeJsonStringArray(Values);
		}

		bool TryGetStringArrayFromObject(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<FString>& OutValues)
		{
			OutValues.Reset();
			if (!Object.IsValid())
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Object->TryGetArrayField(FieldName, JsonArray) || !JsonArray)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
			{
				FString StringValue;
				if (Value.IsValid() && Value->TryGetString(StringValue))
				{
					OutValues.Add(StringValue);
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> CardToJsonObject(const FKnowledgeCard& Card)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("cardId"), Card.CardId);
			Object->SetStringField(TEXT("sourceId"), Card.SourceId);
			Object->SetStringField(TEXT("title"), Card.Title);
			Object->SetStringField(TEXT("sectionTitle"), Card.SectionTitle);
			Object->SetStringField(TEXT("sectionPath"), Card.SectionPath);
			Object->SetStringField(TEXT("category"), Card.Category);
			Object->SetArrayField(TEXT("tags"), StringsToJsonArray(Card.Tags));
			Object->SetStringField(TEXT("sourceKind"), Card.SourceKind);
			Object->SetStringField(TEXT("sourcePath"), Card.SourcePath);
			Object->SetStringField(TEXT("url"), Card.Url);
			Object->SetStringField(TEXT("text"), Card.Text);
			Object->SetNumberField(TEXT("chunkIndex"), Card.ChunkIndex);
			Object->SetNumberField(TEXT("textLength"), Card.TextLength);
			Object->SetNumberField(TEXT("sourceWeight"), Card.SourceWeight);
			Object->SetNumberField(TEXT("confidence"), Card.Confidence);
			Object->SetStringField(TEXT("updatedAt"), Card.UpdatedAt);
			return Object;
		}

		bool JsonObjectToCard(const TSharedPtr<FJsonObject>& Object, FKnowledgeCard& OutCard)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			Object->TryGetStringField(TEXT("cardId"), OutCard.CardId);
			Object->TryGetStringField(TEXT("sourceId"), OutCard.SourceId);
			Object->TryGetStringField(TEXT("title"), OutCard.Title);
			Object->TryGetStringField(TEXT("sectionTitle"), OutCard.SectionTitle);
			Object->TryGetStringField(TEXT("sectionPath"), OutCard.SectionPath);
			Object->TryGetStringField(TEXT("category"), OutCard.Category);
			Object->TryGetStringField(TEXT("sourceKind"), OutCard.SourceKind);
			Object->TryGetStringField(TEXT("sourcePath"), OutCard.SourcePath);
			Object->TryGetStringField(TEXT("url"), OutCard.Url);
			Object->TryGetStringField(TEXT("text"), OutCard.Text);
			Object->TryGetStringField(TEXT("updatedAt"), OutCard.UpdatedAt);
			TryGetStringArrayFromObject(Object, TEXT("tags"), OutCard.Tags);

			double NumberValue = 0.0;
			if (Object->TryGetNumberField(TEXT("chunkIndex"), NumberValue))
			{
				OutCard.ChunkIndex = static_cast<int32>(NumberValue);
			}
			if (Object->TryGetNumberField(TEXT("textLength"), NumberValue))
			{
				OutCard.TextLength = static_cast<int32>(NumberValue);
			}
			if (Object->TryGetNumberField(TEXT("sourceWeight"), NumberValue))
			{
				OutCard.SourceWeight = NumberValue;
			}
			if (Object->TryGetNumberField(TEXT("confidence"), NumberValue))
			{
				OutCard.Confidence = NumberValue;
			}
			if (OutCard.SectionTitle.IsEmpty())
			{
				OutCard.SectionTitle = OutCard.Title;
			}
			if (OutCard.SectionPath.IsEmpty())
			{
				OutCard.SectionPath = OutCard.SectionTitle;
			}
			if (OutCard.SourceWeight <= 0.0)
			{
				OutCard.SourceWeight = SourceWeightForKind(OutCard.SourceKind, OutCard.Category);
			}
			if (OutCard.Confidence <= 0.0)
			{
				OutCard.Confidence = ConfidenceForKind(OutCard.SourceKind);
			}

			return !OutCard.CardId.IsEmpty() && !OutCard.Text.IsEmpty();
		}

		void AddCardsFromText(
			const FString& SourceId,
			const FString& Title,
			const FString& Category,
			const TArray<FString>& Tags,
			const FString& SourceKind,
			const FString& SourcePath,
			const FString& Url,
			const FString& Text,
			int32 MaxChunkChars,
			int32 OverlapChars,
			TArray<FKnowledgeCard>& OutCards)
		{
			const FString CleanText = Text.TrimStartAndEnd();
			if (CleanText.IsEmpty())
			{
				return;
			}

			const int32 SafeMaxChunkChars = FMath::Max(400, MaxChunkChars);
			const int32 SafeOverlapChars = FMath::Clamp(OverlapChars, 0, SafeMaxChunkChars / 2);
			const TArray<FKnowledgeSection> Sections = SplitTextIntoSections(Title, CleanText);
			const FString NowIso = FDateTime::UtcNow().ToIso8601();
			const double SourceWeight = SourceWeightForKind(SourceKind, Category);
			const double Confidence = ConfidenceForKind(SourceKind);

			for (const FKnowledgeSection& Section : Sections)
			{
				int32 Offset = 0;
				int32 ChunkIndex = 0;
				const FString SectionText = Section.Text.TrimStartAndEnd();
				while (Offset < SectionText.Len())
				{
					int32 ChunkLen = FMath::Min(SafeMaxChunkChars, SectionText.Len() - Offset);
					FString ChunkText = SectionText.Mid(Offset, ChunkLen).TrimStartAndEnd();
					if (!ChunkText.IsEmpty())
					{
						FKnowledgeCard Card;
						Card.SourceId = SourceId;
						Card.Title = Section.Title.IsEmpty() || Section.Title.Equals(Title, ESearchCase::CaseSensitive)
							? Title
							: FString::Printf(TEXT("%s / %s"), *Title, *Section.Title);
						Card.SectionTitle = Section.Title.IsEmpty() ? Title : Section.Title;
						Card.SectionPath = Section.Path.IsEmpty() ? Card.SectionTitle : Section.Path;
						Card.Category = Category;
						Card.Tags = Tags;
						Card.SourceKind = SourceKind;
						Card.SourcePath = SourcePath;
						Card.Url = Url;
						Card.Text = ChunkText;
						Card.ChunkIndex = ChunkIndex;
						Card.TextLength = ChunkText.Len();
						Card.SourceWeight = SourceWeight;
						Card.Confidence = Confidence;
						Card.UpdatedAt = NowIso;
						Card.CardId = FString::Printf(
							TEXT("%s:%03d:%03d"),
							*SanitizeKnowledgeId(SourceId),
							Section.SectionIndex,
							ChunkIndex);
						OutCards.Add(MoveTemp(Card));
					}

					ChunkIndex++;
					if (Offset + ChunkLen >= SectionText.Len())
					{
						break;
					}
					Offset += FMath::Max(1, ChunkLen - SafeOverlapChars);
				}
			}
		}

		void AddDocumentationJsonlCards(
			const FString& DocumentsJsonlPath,
			int32 MaxChunkChars,
			int32 OverlapChars,
			bool bSkipLowContent,
			TArray<FKnowledgeCard>& OutCards,
			int32& OutSkippedRows)
		{
			TArray<FString> Lines;
			if (!FFileHelper::LoadFileToStringArray(Lines, *DocumentsJsonlPath))
			{
				return;
			}

			const FString RootDir = FPaths::GetPath(DocumentsJsonlPath);
			for (const FString& Line : Lines)
			{
				if (Line.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> Row;
				if (!LoadJsonObjectFromString(Line, Row) || !Row.IsValid())
				{
					OutSkippedRows++;
					continue;
				}

				bool bLowContent = false;
				Row->TryGetBoolField(TEXT("lowContentWarning"), bLowContent);
				if (bSkipLowContent && bLowContent)
				{
					OutSkippedRows++;
					continue;
				}

				FString TextPath;
				if (!Row->TryGetStringField(TEXT("textPath"), TextPath) || TextPath.IsEmpty())
				{
					OutSkippedRows++;
					continue;
				}

				const FString FullTextPath = NormalizePathForJson(FPaths::Combine(RootDir, TextPath));
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *FullTextPath))
				{
					OutSkippedRows++;
					continue;
				}

				FString Id;
				FString Title;
				FString Category;
				FString Url;
				TArray<FString> Tags;
				Row->TryGetStringField(TEXT("id"), Id);
				Row->TryGetStringField(TEXT("title"), Title);
				Row->TryGetStringField(TEXT("category"), Category);
				Row->TryGetStringField(TEXT("url"), Url);
				TryGetStringArrayFromObject(Row, TEXT("tags"), Tags);
				if (Id.IsEmpty())
				{
					Id = FPaths::GetBaseFilename(FullTextPath);
				}
				if (Title.IsEmpty())
				{
					Title = Id;
				}
				if (Category.IsEmpty())
				{
					Category = TEXT("unreal-docs");
				}

				AddCardsFromText(
					Id,
					Title,
					Category,
					Tags,
					TEXT("official-docs"),
					MakeProjectRelativePath(FullTextPath),
					Url,
					Text,
					MaxChunkChars,
					OverlapChars,
					OutCards);
			}
		}

		void AddMarkdownFileCards(
			const FString& FilePath,
			const FString& SourceId,
			const FString& Category,
			const TArray<FString>& Tags,
			int32 MaxChunkChars,
			int32 OverlapChars,
			TArray<FKnowledgeCard>& OutCards)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FilePath))
			{
				return;
			}

			const FString Title = FPaths::GetBaseFilename(FilePath);
			AddCardsFromText(
				SourceId,
				Title,
				Category,
				Tags,
				TEXT("versioned-doc"),
				MakeProjectRelativePath(FilePath),
				FString(),
				Text,
				MaxChunkChars,
				OverlapChars,
				OutCards);
		}

		void AddVersionedDocumentationCards(int32 MaxChunkChars, int32 OverlapChars, TArray<FKnowledgeCard>& OutCards)
		{
			const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			const TArray<FString> RootDocs = {
				FPaths::Combine(ProjectDir, TEXT("README.md")),
				FPaths::Combine(ProjectDir, TEXT("Plugins/UnrealMcp/README.md")),
				FPaths::Combine(ProjectDir, TEXT("Tools/UnrealMcpKnowledge/README.md")),
				FPaths::Combine(ProjectDir, TEXT("Tools/UnrealMcpTests/README.md"))
			};

			for (const FString& FilePath : RootDocs)
			{
				if (FPaths::FileExists(FilePath))
				{
					AddMarkdownFileCards(
						FilePath,
						SanitizeKnowledgeId(MakeProjectRelativePath(FilePath)),
						TEXT("uevolve-docs"),
						{ TEXT("uevolve"), TEXT("docs") },
						MaxChunkChars,
						OverlapChars,
						OutCards);
				}
			}

			TArray<FString> DocFiles;
			IFileManager::Get().FindFilesRecursive(DocFiles, *FPaths::Combine(ProjectDir, TEXT("Docs")), TEXT("*.md"), true, false);
			for (const FString& FilePath : DocFiles)
			{
				AddMarkdownFileCards(
					FilePath,
					SanitizeKnowledgeId(MakeProjectRelativePath(FilePath)),
					TEXT("uevolve-docs"),
					{ TEXT("uevolve"), TEXT("docs") },
					MaxChunkChars,
					OverlapChars,
					OutCards);
			}
		}

			void AddToolRegistryCards(int32 MaxChunkChars, int32 OverlapChars, TArray<FKnowledgeCard>& OutCards)
			{
				for (const FToolRegistryEntry& Entry : GetToolRegistryEntries())
				{
					if (Entry.Exposure == EToolExposure::LegacyHidden)
				{
					continue;
				}

				const FRegisteredUnrealMcpToolDescriptor* Descriptor = FindRegisteredMcpToolDescriptor(Entry.Name);
				FString Description = Descriptor ? Descriptor->Descriptor.Description : Entry.Policy.Reason;
				FString Text = FString::Printf(
					TEXT("Tool: %s\nCategory: %s\nRisk: %s\nRequires write: %s\nRequires build: %s\nRequires external process: %s\nDry-run support: %s\nPreflight support: %s\nPostcheck support: %s\nDescription: %s\nReason: %s\nNotes: %s"),
					*Entry.Name,
					*Entry.Category,
					*LexToString(Entry.Policy.RiskLevel),
					Entry.Policy.bRequiresWrite ? TEXT("true") : TEXT("false"),
					Entry.Policy.bRequiresBuild ? TEXT("true") : TEXT("false"),
					Entry.Policy.bRequiresExternalProcess ? TEXT("true") : TEXT("false"),
					Entry.Policy.bDryRunSupport ? TEXT("true") : TEXT("false"),
					Entry.Policy.bPreflightSupport ? TEXT("true") : TEXT("false"),
					Entry.Policy.bPostcheckSupport ? TEXT("true") : TEXT("false"),
					*Description,
					*Entry.Policy.Reason,
					*Entry.Notes);

				AddCardsFromText(
					Entry.Name,
					Descriptor ? Descriptor->Descriptor.Title : Entry.Name,
					TEXT("mcp-tools"),
					{ Entry.Category, LexToString(Entry.Policy.RiskLevel) },
					TEXT("tool-registry"),
					TEXT("Tools/UnrealMcpToolRegistry/tools.json"),
					FString(),
					Text,
					MaxChunkChars,
					OverlapChars,
					OutCards);
				}
			}

			void AddSkillMarkdownCards(int32 MaxChunkChars, TArray<FKnowledgeCard>& OutCards, int32& OutSkillFileCount)
			{
				const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
				const FString SkillRoot = FPaths::Combine(ProjectDir, TEXT("Tools/UnrealMcpSkills"));
				if (!FPaths::DirectoryExists(SkillRoot))
				{
					return;
				}

				TArray<FString> SkillFiles;
				IFileManager::Get().FindFilesRecursive(SkillFiles, *SkillRoot, TEXT("SKILL.md"), true, false);
				SkillFiles.Sort();

				const FString NowIso = FDateTime::UtcNow().ToIso8601();
				const int32 SkillMaxChars = FMath::Clamp(MaxChunkChars, 400, DefaultKnowledgeChunkChars);
				for (const FString& FilePath : SkillFiles)
				{
					FString Text;
					if (!FFileHelper::LoadFileToString(Text, *FilePath))
					{
						continue;
					}

					const FString CleanText = Text.TrimStartAndEnd();
					if (CleanText.IsEmpty())
					{
						continue;
					}

					OutSkillFileCount++;
					const FString SkillName = FPaths::GetCleanFilename(FPaths::GetPath(FilePath));
					const FString Title = ExtractFirstMarkdownHeading(CleanText, SkillName.IsEmpty() ? TEXT("Skill") : SkillName);
					const FString SourceId = FString::Printf(TEXT("skill_%s"), *SanitizeKnowledgeId(MakeProjectRelativePath(FPaths::GetPath(FilePath))));
					const FString CardText = CleanText.Left(SkillMaxChars).TrimStartAndEnd();
					if (CardText.IsEmpty())
					{
						continue;
					}

					FKnowledgeCard Card;
					Card.CardId = FString::Printf(TEXT("%s:000"), *SanitizeKnowledgeId(SourceId));
					Card.SourceId = SourceId;
					Card.Title = Title;
					Card.SectionTitle = Title;
					Card.SectionPath = Title;
					Card.Category = TEXT("skill");
					Card.Tags.Add(TEXT("skill"));
					if (!SkillName.IsEmpty())
					{
						Card.Tags.Add(SkillName);
					}
					Card.SourceKind = TEXT("skill");
					Card.SourcePath = MakeProjectRelativePath(FilePath);
					Card.Text = CardText;
					Card.ChunkIndex = 0;
					Card.TextLength = Card.Text.Len();
					Card.SourceWeight = 0.9;
					Card.Confidence = 0.85;
					Card.UpdatedAt = NowIso;
					OutCards.Add(MoveTemp(Card));
				}
			}

			FString NormalizeActivityEventKind(const FString& EventKind)
			{
				const FString Trimmed = EventKind.TrimStartAndEnd();
				if (Trimmed.Equals(TEXT("mcp_tool_call"), ESearchCase::IgnoreCase)
					|| Trimmed.Equals(TEXT("mcp_tool_result"), ESearchCase::IgnoreCase))
				{
					return TEXT("tool_call");
				}
				return Trimmed.IsEmpty() ? TEXT("unknown") : Trimmed;
			}

			bool TryParseActivityLogEvent(const FString& Line, const FString& SourcePath, FActivityLogIndexEvent& OutEvent)
			{
				TSharedPtr<FJsonObject> Object;
				if (!LoadJsonObjectFromString(Line, Object) || !Object.IsValid())
				{
					return false;
				}

				FString SessionId;
				if (!Object->TryGetStringField(TEXT("sessionId"), SessionId) || SessionId.TrimStartAndEnd().IsEmpty())
				{
					return false;
				}

				FString EventKind;
				if (!Object->TryGetStringField(TEXT("eventKind"), EventKind) || EventKind.TrimStartAndEnd().IsEmpty())
				{
					Object->TryGetStringField(TEXT("eventType"), EventKind);
				}

				FString TaskLabel;
				if (!Object->TryGetStringField(TEXT("taskLabel"), TaskLabel) || TaskLabel.TrimStartAndEnd().IsEmpty())
				{
					Object->TryGetStringField(TEXT("goal"), TaskLabel);
				}

				FString Summary;
				Object->TryGetStringField(TEXT("summary"), Summary);

				OutEvent.SessionId = SessionId.TrimStartAndEnd();
				OutEvent.TaskLabel = TaskLabel.TrimStartAndEnd();
				OutEvent.EventKind = NormalizeActivityEventKind(EventKind);
				OutEvent.Summary = Summary.TrimStartAndEnd();
				OutEvent.SourcePath = MakeProjectRelativePath(SourcePath);
				return true;
			}

			FString BuildActivitySpanText(const TArray<FActivityLogIndexEvent>& SpanEvents, int32 MaxChars)
			{
				const int32 SafeMaxChars = FMath::Clamp(MaxChars, 400, DefaultKnowledgeChunkChars);
				FString Text;
				for (const FActivityLogIndexEvent& Event : SpanEvents)
				{
					FString Line = Event.Summary.TrimStartAndEnd();
					if (Line.IsEmpty())
					{
						Line = Event.EventKind;
					}
					else if (!Event.EventKind.IsEmpty())
					{
						Line = FString::Printf(TEXT("%s: %s"), *Event.EventKind, *Line);
					}

					if (Line.IsEmpty())
					{
						continue;
					}

					const FString Candidate = Text.IsEmpty() ? Line : Text + TEXT("\n") + Line;
					if (Candidate.Len() <= SafeMaxChars)
					{
						Text = Candidate;
						continue;
					}

					if (Text.IsEmpty())
					{
						Text = Candidate.Left(SafeMaxChars).TrimStartAndEnd();
					}
					break;
				}
				return Text.TrimStartAndEnd();
			}

			TArray<FString> MakeActivitySpanTags(const TArray<FActivityLogIndexEvent>& SpanEvents)
			{
				TArray<FString> Tags;
				for (const FActivityLogIndexEvent& Event : SpanEvents)
				{
					if (!Event.EventKind.TrimStartAndEnd().IsEmpty())
					{
						Tags.AddUnique(Event.EventKind.TrimStartAndEnd());
					}
				}
				Tags.Sort();
				return Tags;
			}

			void EmitActivitySpanCard(
				const FString& SessionId,
				const FString& TaskLabel,
				int32 SpanIndex,
				const TArray<FActivityLogIndexEvent>& SpanEvents,
				int32 MaxChunkChars,
				TArray<FKnowledgeCard>& OutCards)
			{
				if (SpanEvents.IsEmpty())
				{
					return;
				}

				const FString Text = BuildActivitySpanText(SpanEvents, FMath::Min(MaxChunkChars, DefaultKnowledgeChunkChars));
				if (Text.IsEmpty())
				{
					return;
				}

				const bool bHasLabel = !TaskLabel.TrimStartAndEnd().IsEmpty();
				const FString LabelOrSession = bHasLabel ? TaskLabel.TrimStartAndEnd() : TEXT("unlabeled");
				const FString CardBaseId = SanitizeKnowledgeId(FString::Printf(
					TEXT("activity_%s_%s_%d"),
					*SessionId,
					*(bHasLabel ? TaskLabel.TrimStartAndEnd() : TEXT("session")),
					SpanIndex));

				FKnowledgeCard Card;
				Card.CardId = CardBaseId;
				Card.SourceId = CardBaseId;
				Card.Title = bHasLabel
					? FString::Printf(TEXT("Activity: %s"), *TaskLabel.TrimStartAndEnd())
					: FString::Printf(TEXT("Activity session %s"), *SessionId);
				Card.SectionTitle = FString::Printf(TEXT("events × %d"), SpanEvents.Num());
				Card.SectionPath = FString::Printf(TEXT("%s/%s"), *SessionId, *LabelOrSession);
				Card.Category = TEXT("activity");
				Card.Tags = MakeActivitySpanTags(SpanEvents);
				Card.SourceKind = TEXT("activity-log");
				Card.SourcePath = SpanEvents[0].SourcePath;
				Card.Text = Text;
				Card.ChunkIndex = 0;
				Card.TextLength = Card.Text.Len();
				Card.SourceWeight = 0.5;
				Card.Confidence = 0.6;
				Card.UpdatedAt = FDateTime::UtcNow().ToIso8601();
				OutCards.Add(MoveTemp(Card));
			}

			void AddActivityLogCards(int32 MaxChunkChars, TArray<FKnowledgeCard>& OutCards, int32& OutActivityLogFileCount, int32& OutActivityEventCount)
			{
				const FString ActivityRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ActivityLog")));
				if (!FPaths::DirectoryExists(ActivityRoot))
				{
					return;
				}

				TArray<FString> ActivityFiles;
				IFileManager::Get().FindFilesRecursive(ActivityFiles, *ActivityRoot, TEXT("*.jsonl"), true, false);
				ActivityFiles.Sort();
				OutActivityLogFileCount = ActivityFiles.Num();

				TMap<FString, TArray<FActivityLogIndexEvent>> EventsBySession;
				for (const FString& ActivityFile : ActivityFiles)
				{
					TArray<FString> Lines;
					if (!FFileHelper::LoadFileToStringArray(Lines, *ActivityFile))
					{
						continue;
					}

					for (const FString& Line : Lines)
					{
						if (Line.TrimStartAndEnd().IsEmpty())
						{
							continue;
						}

						FActivityLogIndexEvent Event;
						if (TryParseActivityLogEvent(Line, ActivityFile, Event))
						{
							OutActivityEventCount++;
							EventsBySession.FindOrAdd(Event.SessionId).Add(MoveTemp(Event));
						}
					}
				}

				TArray<FString> SessionIds;
				EventsBySession.GetKeys(SessionIds);
				SessionIds.Sort();
				for (const FString& SessionId : SessionIds)
				{
					const TArray<FActivityLogIndexEvent>* SessionEvents = EventsBySession.Find(SessionId);
					if (!SessionEvents || SessionEvents->IsEmpty())
					{
						continue;
					}

					TArray<FActivityLogIndexEvent> SpanEvents;
					FString CurrentLabel;
					int32 SpanIndex = 0;
					auto FlushSpan = [&]()
					{
						EmitActivitySpanCard(SessionId, CurrentLabel, SpanIndex, SpanEvents, MaxChunkChars, OutCards);
						SpanEvents.Reset();
						SpanIndex++;
					};

					for (const FActivityLogIndexEvent& Event : *SessionEvents)
					{
						const FString EventLabel = Event.TaskLabel.TrimStartAndEnd();
						if (SpanEvents.IsEmpty())
						{
							CurrentLabel = EventLabel;
						}
						else if (!CurrentLabel.Equals(EventLabel, ESearchCase::CaseSensitive))
						{
							FlushSpan();
							CurrentLabel = EventLabel;
						}
						SpanEvents.Add(Event);
					}
					FlushSpan();
				}
			}

		bool WriteKnowledgeCardsJsonl(const FString& Path, const TArray<FKnowledgeCard>& Cards, FString& OutFailureReason)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			FString Output;
			for (const FKnowledgeCard& Card : Cards)
			{
				Output += JsonObjectToCondensedString(CardToJsonObject(Card));
				Output += LINE_TERMINATOR;
			}

			if (!FFileHelper::SaveStringToFile(Output, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write knowledge cards '%s'."), *Path);
				return false;
			}
			return true;
		}

		FString MakeKnowledgeDedupKey(const FKnowledgeCard& Card)
		{
			FString TextKey = Card.Text.Left(700).ToLower();
			TextKey.ReplaceInline(TEXT("\r"), TEXT(" "));
			TextKey.ReplaceInline(TEXT("\n"), TEXT(" "));
			while (TextKey.Contains(TEXT("  ")))
			{
				TextKey.ReplaceInline(TEXT("  "), TEXT(" "));
			}
			return FString::Printf(
				TEXT("%s|%s|%s"),
				*Card.SourceId.ToLower(),
				*Card.SectionPath.ToLower(),
				*TextKey.TrimStartAndEnd());
		}

			void DeduplicateKnowledgeCards(TArray<FKnowledgeCard>& Cards)
			{
				TSet<FString> Seen;
				TArray<FKnowledgeCard> UniqueCards;
				UniqueCards.Reserve(Cards.Num());
				for (FKnowledgeCard& Card : Cards)
				{
					const FString Key = MakeKnowledgeDedupKey(Card);
					if (Seen.Contains(Key))
					{
						continue;
					}

					Seen.Add(Key);
					UniqueCards.Add(MoveTemp(Card));
				}
				Cards = MoveTemp(UniqueCards);
			}

			void InvalidateKnowledgeCardCache()
			{
				FScopeLock Lock(&GKnowledgeCardCacheMutex);
				GKnowledgeCardCache = FKnowledgeCardCacheState();
			}

			bool LoadKnowledgeCards(const FString& IndexDir, TArray<FKnowledgeCard>& OutCards, FString& OutFailureReason)
			{
				const FString CardsPath = FPaths::Combine(IndexDir, TEXT("cards.jsonl"));
				const int64 FileSize = IFileManager::Get().FileSize(*CardsPath);
				if (FileSize < 0)
				{
					OutFailureReason = FString::Printf(TEXT("Knowledge index not found at '%s'. Run unreal.knowledge_index_refresh first."), *CardsPath);
					return false;
				}
				const FDateTime FileTimestamp = IFileManager::Get().GetTimeStamp(*CardsPath);

				{
					FScopeLock Lock(&GKnowledgeCardCacheMutex);
					if (GKnowledgeCardCache.bValid
						&& GKnowledgeCardCache.IndexFilePath.Equals(CardsPath, ESearchCase::CaseSensitive)
						&& GKnowledgeCardCache.FileSize == FileSize
						&& GKnowledgeCardCache.FileTimestamp == FileTimestamp)
					{
						OutCards = GKnowledgeCardCache.Cards;
						if (OutCards.IsEmpty())
						{
							OutFailureReason = FString::Printf(TEXT("Knowledge index '%s' contains no cards."), *CardsPath);
							return false;
						}
						return true;
					}
				}

				TArray<FString> Lines;
				if (!FFileHelper::LoadFileToStringArray(Lines, *CardsPath))
				{
					OutFailureReason = FString::Printf(TEXT("Knowledge index not found at '%s'. Run unreal.knowledge_index_refresh first."), *CardsPath);
					return false;
				}

				TArray<FKnowledgeCard> LoadedCards;
				for (const FString& Line : Lines)
				{
					if (Line.TrimStartAndEnd().IsEmpty())
					{
						continue;
					}
					TSharedPtr<FJsonObject> Object;
					if (!LoadJsonObjectFromString(Line, Object))
					{
						continue;
					}
					FKnowledgeCard Card;
					if (JsonObjectToCard(Object, Card))
					{
						LoadedCards.Add(MoveTemp(Card));
					}
				}

				if (LoadedCards.IsEmpty())
				{
					OutFailureReason = FString::Printf(TEXT("Knowledge index '%s' contains no cards."), *CardsPath);
					return false;
				}

				{
					FScopeLock Lock(&GKnowledgeCardCacheMutex);
					GKnowledgeCardCache.IndexFilePath = CardsPath;
					GKnowledgeCardCache.FileSize = FileSize;
					GKnowledgeCardCache.FileTimestamp = FileTimestamp;
					GKnowledgeCardCache.Cards = LoadedCards;
					GKnowledgeCardCache.bValid = true;
				}
				OutCards = MoveTemp(LoadedCards);
				return true;
			}

		double ScoreKnowledgeCard(const FKnowledgeCard& Card, const FString& Query, const TArray<FString>& QueryTokens)
		{
			const FString QueryLower = Query.ToLower().TrimStartAndEnd();
			const FString TitleLower = Card.Title.ToLower();
			const FString SectionLower = (Card.SectionTitle + TEXT(" ") + Card.SectionPath).ToLower();
			const FString CategoryLower = Card.Category.ToLower();
			const FString TextLower = Card.Text.ToLower();
			double Score = 0.0;

			if (!QueryLower.IsEmpty())
			{
				if (TitleLower.Contains(QueryLower))
				{
					Score += 40.0;
				}
				if (SectionLower.Contains(QueryLower))
				{
					Score += 28.0;
				}
				if (CategoryLower.Contains(QueryLower))
				{
					Score += 16.0;
				}
				if (TextLower.Contains(QueryLower))
				{
					Score += 20.0;
				}
			}

			for (const FString& Token : QueryTokens)
			{
				if (TitleLower.Contains(Token))
				{
					Score += 12.0;
				}
				if (SectionLower.Contains(Token))
				{
					Score += 10.0;
				}
				if (CategoryLower.Contains(Token))
				{
					Score += 8.0;
				}
				for (const FString& Tag : Card.Tags)
				{
					if (Tag.ToLower().Contains(Token))
					{
						Score += 8.0;
					}
				}
				if (TextLower.Contains(Token))
				{
					Score += 2.0;
				}
			}
			if (Score <= 0.0)
			{
				return 0.0;
			}

			const double SafeSourceWeight = Card.SourceWeight > 0.0 ? Card.SourceWeight : SourceWeightForKind(Card.SourceKind, Card.Category);
			const double SafeConfidence = Card.Confidence > 0.0 ? Card.Confidence : ConfidenceForKind(Card.SourceKind);
			return Score * SafeSourceWeight * FMath::Clamp(SafeConfidence, 0.35, 1.0);
		}

			FString MakeExcerpt(const FString& Text, const FString& Query, const TArray<FString>& QueryTokens, int32 MaxChars)
			{
				const int32 SafeMaxChars = FMath::Clamp(MaxChars, 80, 2400);
				if (Text.Len() <= SafeMaxChars)
				{
				return Text;
			}

			const FString LowerText = Text.ToLower();
			int32 HitIndex = INDEX_NONE;
			if (!Query.TrimStartAndEnd().IsEmpty())
			{
				HitIndex = LowerText.Find(Query.ToLower().TrimStartAndEnd());
			}
			if (HitIndex == INDEX_NONE)
			{
				for (const FString& Token : QueryTokens)
				{
					HitIndex = LowerText.Find(Token);
					if (HitIndex != INDEX_NONE)
					{
						break;
					}
				}
			}

				const int32 Start = HitIndex == INDEX_NONE ? 0 : FMath::Max(0, HitIndex - SafeMaxChars / 3);
				return Text.Mid(Start, SafeMaxChars).TrimStartAndEnd();
			}

			TSharedPtr<FJsonObject> MakeKnowledgeSearchResultObject(
				const FKnowledgeCard& Card,
				double Score,
				const FString& Query,
				const TArray<FString>& QueryTokens,
				int32 MaxExcerptChars,
				bool bIncludeText)
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("cardId"), Card.CardId);
				Result->SetStringField(TEXT("title"), Card.Title);
				Result->SetStringField(TEXT("sectionTitle"), Card.SectionTitle);
				Result->SetStringField(TEXT("sectionPath"), Card.SectionPath);
				Result->SetStringField(TEXT("category"), Card.Category);
				Result->SetStringField(TEXT("sourceKind"), Card.SourceKind);
				Result->SetStringField(TEXT("sourcePath"), Card.SourcePath);
				Result->SetStringField(TEXT("url"), Card.Url);
				Result->SetArrayField(TEXT("tags"), StringsToJsonArray(Card.Tags));
				Result->SetNumberField(TEXT("score"), Score);
				Result->SetNumberField(TEXT("sourceWeight"), Card.SourceWeight);
				Result->SetNumberField(TEXT("confidence"), Card.Confidence);
				Result->SetStringField(TEXT("excerpt"), MakeExcerpt(Card.Text, Query, QueryTokens, MaxExcerptChars));
				if (bIncludeText)
				{
					Result->SetStringField(TEXT("text"), Card.Text);
				}
				return Result;
			}

			bool CategoryAllowed(const FString& Category, const TArray<FString>& Filters)
			{
				if (Filters.IsEmpty())
				{
				return true;
			}
			for (const FString& Filter : Filters)
			{
				if (Category.Equals(Filter, ESearchCase::IgnoreCase))
				{
					return true;
				}
				}
				return false;
			}

			bool SourceKindAllowed(const FString& SourceKind, const TArray<FString>& Filters)
			{
				if (Filters.IsEmpty())
				{
					return true;
				}
				for (const FString& Filter : Filters)
				{
					if (SourceKind.Equals(Filter, ESearchCase::IgnoreCase))
					{
						return true;
					}
				}
				return false;
			}

		int32 RiskRank(EToolRiskLevel Risk)
		{
			switch (Risk)
			{
			case EToolRiskLevel::ReadOnly:
				return 0;
			case EToolRiskLevel::Low:
				return 1;
			case EToolRiskLevel::Medium:
				return 2;
			case EToolRiskLevel::High:
				return 3;
			case EToolRiskLevel::Critical:
				return 4;
			default:
				return 2;
			}
		}

		int32 RiskRankFromString(const FString& RiskMax)
		{
			if (RiskMax.Equals(TEXT("read_only"), ESearchCase::IgnoreCase) || RiskMax.Equals(TEXT("readonly"), ESearchCase::IgnoreCase))
			{
				return 0;
			}
			if (RiskMax.Equals(TEXT("low"), ESearchCase::IgnoreCase))
			{
				return 1;
			}
			if (RiskMax.Equals(TEXT("medium"), ESearchCase::IgnoreCase))
			{
				return 2;
			}
			if (RiskMax.Equals(TEXT("high"), ESearchCase::IgnoreCase))
			{
				return 3;
			}
			if (RiskMax.Equals(TEXT("critical"), ESearchCase::IgnoreCase))
			{
				return 4;
			}
			return 4;
		}

		double ScoreToolForTask(const FToolRegistryEntry& Entry, const FString& Task, const TArray<FString>& Tokens)
		{
			const FRegisteredUnrealMcpToolDescriptor* Descriptor = FindRegisteredMcpToolDescriptor(Entry.Name);
			FString Haystack = Entry.Name + TEXT(" ") + Entry.Category + TEXT(" ") + Entry.Policy.Reason + TEXT(" ") + Entry.Notes;
			if (Descriptor)
			{
				Haystack += TEXT(" ") + Descriptor->Descriptor.Title + TEXT(" ") + Descriptor->Descriptor.Description;
			}
			Haystack = Haystack.ToLower();

			const FString TaskLower = Task.ToLower();
			double Score = 0.0;
			if (!TaskLower.IsEmpty() && Haystack.Contains(TaskLower))
			{
				Score += 40.0;
			}

			for (const FString& Token : Tokens)
			{
				if (Entry.Name.ToLower().Contains(Token))
				{
					Score += 16.0;
				}
				if (Entry.Category.ToLower().Contains(Token))
				{
					Score += 8.0;
				}
				if (Haystack.Contains(Token))
				{
					Score += 4.0;
				}
			}

			if (TaskLower.Contains(TEXT("blueprint")) || TaskLower.Contains(TEXT("蓝图")))
			{
				Score += Entry.Category == TEXT("blueprint") ? 24.0 : 0.0;
			}
			if (TaskLower.Contains(TEXT("widget")) || TaskLower.Contains(TEXT("umg")) || TaskLower.Contains(TEXT("hud")) || TaskLower.Contains(TEXT("ui")))
			{
				Score += Entry.Category == TEXT("widget") ? 24.0 : 0.0;
			}
			if (TaskLower.Contains(TEXT("actor")) || TaskLower.Contains(TEXT("spawn")) || TaskLower.Contains(TEXT("level")) || TaskLower.Contains(TEXT("场景")))
			{
				Score += Entry.Category == TEXT("actors") || Entry.Category == TEXT("editor") ? 14.0 : 0.0;
			}
			if (TaskLower.Contains(TEXT("mcp")) || TaskLower.Contains(TEXT("tool")) || TaskLower.Contains(TEXT("工具")) || TaskLower.Contains(TEXT("自拓展")))
			{
				Score += Entry.Category == TEXT("self-extension") || Entry.Category == TEXT("scaffold") ? 18.0 : 0.0;
			}
			if (TaskLower.Contains(TEXT("rag")) || TaskLower.Contains(TEXT("knowledge")) || TaskLower.Contains(TEXT("知识库")))
			{
				Score += Entry.Name.Contains(TEXT("knowledge")) || Entry.Name == TEXT("unreal.tool_recommend") ? 30.0 : 0.0;
			}

			if (Entry.Policy.bPostcheckSupport)
			{
				Score += 2.0;
			}
			if (Entry.Policy.bDryRunSupport)
			{
				Score += 2.0;
			}
			return Score;
		}

		TSharedPtr<FJsonObject> MakeToolRecommendationObject(const FToolRegistryEntry& Entry, double Score)
		{
			const FRegisteredUnrealMcpToolDescriptor* Descriptor = FindRegisteredMcpToolDescriptor(Entry.Name);
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("toolName"), Entry.Name);
			Object->SetStringField(TEXT("handlerName"), Entry.HandlerName.IsEmpty() ? Entry.Name : Entry.HandlerName);
			Object->SetStringField(TEXT("category"), Entry.Category);
			Object->SetStringField(TEXT("title"), Descriptor ? Descriptor->Descriptor.Title : Entry.Name);
			Object->SetStringField(TEXT("description"), Descriptor ? Descriptor->Descriptor.Description : Entry.Policy.Reason);
			Object->SetStringField(TEXT("riskLevel"), LexToString(Entry.Policy.RiskLevel));
			Object->SetBoolField(TEXT("requiresWrite"), Entry.Policy.bRequiresWrite);
			Object->SetBoolField(TEXT("dryRunSupport"), Entry.Policy.bDryRunSupport);
			Object->SetBoolField(TEXT("preflightSupport"), Entry.Policy.bPreflightSupport);
			Object->SetBoolField(TEXT("postcheckSupport"), Entry.Policy.bPostcheckSupport);
			Object->SetNumberField(TEXT("score"), Score);
			return Object;
		}

		struct FToolRecommendationCandidate
		{
			const FToolRegistryEntry* Entry = nullptr;
			double Score = 0.0;
		};

		TArray<FToolRecommendationCandidate> FindToolRecommendations(const FString& Task, const FString& RiskMax)
		{
			const int32 MaxRiskRank = RiskRankFromString(RiskMax);
			const TArray<FString> Tokens = ExpandSearchTokens(Task);
			TArray<FToolRecommendationCandidate> ScoredTools;
			for (const FToolRegistryEntry& Entry : GetToolRegistryEntries())
			{
				if (Entry.Exposure == EToolExposure::LegacyHidden)
				{
					continue;
				}
				if (RiskRank(Entry.Policy.RiskLevel) > MaxRiskRank)
				{
					continue;
				}

				const double Score = ScoreToolForTask(Entry, Task, Tokens);
				if (Score > 0.0)
				{
					FToolRecommendationCandidate Candidate;
					Candidate.Entry = &Entry;
					Candidate.Score = Score;
					ScoredTools.Add(Candidate);
				}
			}

			ScoredTools.Sort([](const FToolRecommendationCandidate& Left, const FToolRecommendationCandidate& Right)
			{
				if (!FMath::IsNearlyEqual(Left.Score, Right.Score))
				{
					return Left.Score > Right.Score;
				}
				return Left.Entry && Right.Entry ? Left.Entry->Name < Right.Entry->Name : false;
			});

			return ScoredTools;
		}

		TArray<TSharedPtr<FJsonValue>> MakeRecommendationValues(const TArray<FToolRecommendationCandidate>& ScoredTools, int32 Limit)
		{
			TArray<TSharedPtr<FJsonValue>> RecommendationValues;
			for (int32 Index = 0; Index < FMath::Min(Limit, ScoredTools.Num()); ++Index)
			{
				if (ScoredTools[Index].Entry)
				{
					RecommendationValues.Add(MakeShared<FJsonValueObject>(MakeToolRecommendationObject(*ScoredTools[Index].Entry, ScoredTools[Index].Score)));
				}
			}
			return RecommendationValues;
		}

		TSharedPtr<FJsonObject> MakeWorkflowStepObject(
			const FString& ToolName,
			const FString& Purpose,
			const TSharedPtr<FJsonObject>& Arguments,
			bool bSkip = false)
		{
			TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("tool"), ToolName);
			Step->SetStringField(TEXT("purpose"), Purpose);
			if (Arguments.IsValid())
			{
				Step->SetObjectField(TEXT("arguments"), Arguments);
			}
			if (bSkip)
			{
				Step->SetBoolField(TEXT("skip"), true);
			}
			return Step;
		}

	}

	FUnrealMcpExecutionResult KnowledgeIndexRefresh(const FJsonObject& Arguments)
	{
		FString SourceRoot = GetKnowledgeSourceRoot();
		FString IndexRoot = GetKnowledgeIndexRoot();
		Arguments.TryGetStringField(TEXT("sourceRoot"), SourceRoot);
		Arguments.TryGetStringField(TEXT("indexRoot"), IndexRoot);
		SourceRoot = ResolveProjectPathForJson(SourceRoot);
		IndexRoot = ResolveProjectPathForJson(IndexRoot);

			bool bIncludeOfficialDocs = true;
			bool bIncludeVersionedDocs = true;
			bool bIncludeToolRegistry = true;
			bool bIncludeActivityLog = true;
			bool bIncludeSkills = true;
			bool bSkipLowContent = true;
			bool bDryRun = false;
			Arguments.TryGetBoolField(TEXT("includeOfficialDocs"), bIncludeOfficialDocs);
			Arguments.TryGetBoolField(TEXT("includeVersionedDocs"), bIncludeVersionedDocs);
			Arguments.TryGetBoolField(TEXT("includeToolRegistry"), bIncludeToolRegistry);
			Arguments.TryGetBoolField(TEXT("includeActivityLog"), bIncludeActivityLog);
			Arguments.TryGetBoolField(TEXT("includeSkills"), bIncludeSkills);
			Arguments.TryGetBoolField(TEXT("skipLowContent"), bSkipLowContent);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

		const int32 MaxCards = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("maxCards"), DefaultKnowledgeMaxCards), 1, 20000);
		const int32 MaxChunkChars = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("maxChunkChars"), DefaultKnowledgeChunkChars), 400, 12000);
		const int32 OverlapChars = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("chunkOverlapChars"), DefaultKnowledgeOverlapChars), 0, MaxChunkChars / 2);

			TArray<FKnowledgeCard> Cards;
			int32 SkippedRows = 0;
			int32 ActivityLogFileCount = 0;
			int32 ActivityEventCount = 0;
			int32 SkillFileCount = 0;
			TArray<FString> SourceFiles;

		if (bIncludeOfficialDocs && FPaths::DirectoryExists(SourceRoot))
		{
			IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceRoot, TEXT("documents.jsonl"), true, false);
			for (const FString& DocumentsJsonlPath : SourceFiles)
			{
				AddDocumentationJsonlCards(DocumentsJsonlPath, MaxChunkChars, OverlapChars, bSkipLowContent, Cards, SkippedRows);
				if (Cards.Num() >= MaxCards * 4)
				{
					break;
				}
			}
		}

		if (bIncludeVersionedDocs)
		{
			AddVersionedDocumentationCards(MaxChunkChars, OverlapChars, Cards);
		}

		if (bIncludeToolRegistry)
		{
			AddToolRegistryCards(MaxChunkChars, OverlapChars, Cards);
		}

		if (bIncludeSkills)
		{
			AddSkillMarkdownCards(MaxChunkChars, Cards, SkillFileCount);
		}

		if (bIncludeActivityLog)
		{
			AddActivityLogCards(MaxChunkChars, Cards, ActivityLogFileCount, ActivityEventCount);
		}

		DeduplicateKnowledgeCards(Cards);
		Cards.Sort([](const FKnowledgeCard& Left, const FKnowledgeCard& Right)
		{
			const double LeftRank = Left.SourceWeight * Left.Confidence;
			const double RightRank = Right.SourceWeight * Right.Confidence;
			if (!FMath::IsNearlyEqual(LeftRank, RightRank))
			{
				return LeftRank > RightRank;
			}
			return Left.Title < Right.Title;
		});
		if (Cards.Num() > MaxCards)
		{
			Cards.SetNum(MaxCards);
		}

		TArray<TSharedPtr<FJsonValue>> SourceFileValues;
		for (const FString& SourceFile : SourceFiles)
		{
			SourceFileValues.Add(MakeShared<FJsonValueString>(MakeProjectRelativePath(SourceFile)));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("knowledge_index_refresh"));
		StructuredContent->SetStringField(TEXT("indexRoot"), MakeProjectRelativePath(IndexRoot));
		StructuredContent->SetStringField(TEXT("sourceRoot"), MakeProjectRelativePath(SourceRoot));
		StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetNumberField(TEXT("cardCount"), Cards.Num());
			StructuredContent->SetNumberField(TEXT("sourceDocumentsJsonlCount"), SourceFiles.Num());
			StructuredContent->SetNumberField(TEXT("skippedRows"), SkippedRows);
			StructuredContent->SetNumberField(TEXT("activityLogFileCount"), ActivityLogFileCount);
			StructuredContent->SetNumberField(TEXT("activityEventCount"), ActivityEventCount);
			StructuredContent->SetNumberField(TEXT("skillFileCount"), SkillFileCount);
			StructuredContent->SetArrayField(TEXT("sourceDocumentsJsonl"), SourceFileValues);

		if (bDryRun)
		{
			return MakeExecutionResult(
				FString::Printf(TEXT("Knowledge index dry run: would write %d KnowledgeCards from %d source documents.jsonl files."), Cards.Num(), SourceFiles.Num()),
				StructuredContent,
				false);
		}

		const FString CardsPath = FPaths::Combine(IndexRoot, TEXT("cards.jsonl"));
		const FString ManifestPath = FPaths::Combine(IndexRoot, TEXT("index.json"));
		FString FailureReason;
		if (!WriteKnowledgeCardsJsonl(CardsPath, Cards, FailureReason))
		{
			return MakeExecutionResult(FailureReason, StructuredContent, true);
		}
		InvalidateKnowledgeCardCache();

		TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema"), TEXT("UEvolve.KnowledgeIndex.v1"));
		Manifest->SetStringField(TEXT("indexRoot"), MakeProjectRelativePath(IndexRoot));
		Manifest->SetStringField(TEXT("sourceRoot"), MakeProjectRelativePath(SourceRoot));
		Manifest->SetStringField(TEXT("cardsPath"), MakeProjectRelativePath(CardsPath));
			Manifest->SetNumberField(TEXT("cardCount"), Cards.Num());
			Manifest->SetNumberField(TEXT("sourceDocumentsJsonlCount"), SourceFiles.Num());
			Manifest->SetNumberField(TEXT("skippedRows"), SkippedRows);
			Manifest->SetNumberField(TEXT("activityLogFileCount"), ActivityLogFileCount);
			Manifest->SetNumberField(TEXT("activityEventCount"), ActivityEventCount);
			Manifest->SetNumberField(TEXT("skillFileCount"), SkillFileCount);
			Manifest->SetBoolField(TEXT("includeOfficialDocs"), bIncludeOfficialDocs);
			Manifest->SetBoolField(TEXT("includeVersionedDocs"), bIncludeVersionedDocs);
			Manifest->SetBoolField(TEXT("includeToolRegistry"), bIncludeToolRegistry);
			Manifest->SetBoolField(TEXT("includeActivityLog"), bIncludeActivityLog);
			Manifest->SetBoolField(TEXT("includeSkills"), bIncludeSkills);
		Manifest->SetArrayField(TEXT("sourceDocumentsJsonl"), SourceFileValues);
		if (!WriteJsonObjectToFile(Manifest, ManifestPath, FailureReason))
		{
			return MakeExecutionResult(FailureReason, StructuredContent, true);
		}

		StructuredContent->SetStringField(TEXT("cardsPath"), MakeProjectRelativePath(CardsPath));
		StructuredContent->SetStringField(TEXT("manifestPath"), MakeProjectRelativePath(ManifestPath));
		return MakeExecutionResult(
			FString::Printf(TEXT("Knowledge index refreshed: %d KnowledgeCards written to %s."), Cards.Num(), *MakeProjectRelativePath(CardsPath)),
			StructuredContent,
			false);
	}

	FUnrealMcpExecutionResult KnowledgeSearch(const FJsonObject& Arguments)
	{
		FString Query;
		if (!Arguments.TryGetStringField(TEXT("query"), Query) || Query.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Missing required field 'query'."), nullptr, true);
		}

		FString IndexRoot = GetKnowledgeIndexRoot();
		Arguments.TryGetStringField(TEXT("indexRoot"), IndexRoot);
		IndexRoot = ResolveProjectPathForJson(IndexRoot);

		TArray<FString> Categories;
		TryGetStringArrayField(Arguments, TEXT("categories"), Categories);
		TArray<FString> RawSourceKindFilters;
		TryGetStringArrayField(Arguments, TEXT("sourceKinds"), RawSourceKindFilters);
		TArray<FString> SourceKindFilters;
		FString SourceKindFailureReason;
		if (!ValidateSourceKindFilters(RawSourceKindFilters, SourceKindFilters, SourceKindFailureReason))
		{
			return MakeExecutionResult(SourceKindFailureReason, nullptr, true);
		}

		bool bGroupByKind = false;
		Arguments.TryGetBoolField(TEXT("groupByKind"), bGroupByKind);
		bool bIncludeText = false;
		Arguments.TryGetBoolField(TEXT("includeText"), bIncludeText);
		const int32 Limit = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("limit"), DefaultKnowledgeSearchLimit), 1, 50);
		const int32 MaxExcerptChars = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("maxExcerptChars"), DefaultKnowledgeExcerptChars), 80, 2400);

		TArray<FKnowledgeCard> Cards;
		FString FailureReason;
		if (!LoadKnowledgeCards(IndexRoot, Cards, FailureReason))
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("knowledge_search"));
			StructuredContent->SetStringField(TEXT("indexRoot"), MakeProjectRelativePath(IndexRoot));
			StructuredContent->SetStringField(TEXT("recommendedNextTool"), TEXT("unreal.knowledge_index_refresh"));
			return MakeExecutionResult(FailureReason, StructuredContent, true);
		}

		const TArray<FString> QueryTokens = ExpandSearchTokens(Query);
		struct FScoredCard
		{
			FKnowledgeCard Card;
			double Score = 0.0;
		};
		TArray<FScoredCard> ScoredCards;
		for (const FKnowledgeCard& Card : Cards)
		{
			if (!CategoryAllowed(Card.Category, Categories))
			{
				continue;
			}
			if (!SourceKindAllowed(Card.SourceKind, SourceKindFilters))
			{
				continue;
			}
			const double Score = ScoreKnowledgeCard(Card, Query, QueryTokens);
			if (Score > 0.0)
			{
				FScoredCard Scored;
				Scored.Card = Card;
				Scored.Score = Score;
				ScoredCards.Add(MoveTemp(Scored));
			}
		}

		ScoredCards.Sort([](const FScoredCard& Left, const FScoredCard& Right)
		{
			if (!FMath::IsNearlyEqual(Left.Score, Right.Score))
			{
				return Left.Score > Right.Score;
			}
			return Left.Card.Title < Right.Card.Title;
		});

		TArray<TSharedPtr<FJsonValue>> ResultValues;
		TMap<FString, TArray<TSharedPtr<FJsonValue>>> ResultValuesByKind;
		TSet<FString> AddedSourceGroups;
		int32 ResultCount = 0;
		for (int32 Index = 0; Index < ScoredCards.Num() && ResultCount < Limit; ++Index)
		{
			const FScoredCard& Scored = ScoredCards[Index];
			const FString SourceGroup = FString::Printf(TEXT("%s|%s"), *Scored.Card.SourceId, *Scored.Card.SectionPath);
			if (AddedSourceGroups.Contains(SourceGroup))
			{
				continue;
			}
			AddedSourceGroups.Add(SourceGroup);

			TSharedPtr<FJsonObject> Result = MakeKnowledgeSearchResultObject(
				Scored.Card,
				Scored.Score,
				Query,
				QueryTokens,
				MaxExcerptChars,
				bIncludeText);
			if (bGroupByKind)
			{
				ResultValuesByKind.FindOrAdd(Scored.Card.SourceKind).Add(MakeShared<FJsonValueObject>(Result));
			}
			else
			{
				ResultValues.Add(MakeShared<FJsonValueObject>(Result));
			}
			ResultCount++;
		}

		TSharedPtr<FJsonObject> ByKindObject = MakeShared<FJsonObject>();
		if (bGroupByKind)
		{
			TArray<FString> KindKeys;
			ResultValuesByKind.GetKeys(KindKeys);
			KindKeys.Sort();
			for (const FString& KindKey : KindKeys)
			{
				ByKindObject->SetArrayField(KindKey, ResultValuesByKind.FindRef(KindKey));
			}
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("knowledge_search"));
		StructuredContent->SetStringField(TEXT("query"), Query);
		StructuredContent->SetStringField(TEXT("indexRoot"), MakeProjectRelativePath(IndexRoot));
		StructuredContent->SetNumberField(TEXT("cardCount"), Cards.Num());
		StructuredContent->SetNumberField(TEXT("matchCount"), ScoredCards.Num());
		StructuredContent->SetNumberField(TEXT("resultCount"), ResultCount);
		StructuredContent->SetBoolField(TEXT("groupByKind"), bGroupByKind);
		StructuredContent->SetArrayField(TEXT("sourceKinds"), MakeJsonStringArray(SourceKindFilters));
		StructuredContent->SetObjectField(TEXT("kindStatus"), MakeKindStatusObject(Cards));
		if (bGroupByKind)
		{
			StructuredContent->SetObjectField(TEXT("byKind"), ByKindObject);
		}
		else
		{
			StructuredContent->SetArrayField(TEXT("results"), ResultValues);
		}
		return MakeExecutionResult(
			FString::Printf(TEXT("Knowledge search returned %d result(s) for '%s'."), ResultCount, *Query),
			StructuredContent,
			false);
	}

	TArray<TSharedPtr<FJsonObject>> BuildEvidenceForTask(
		const FString& TaskQuery,
		int32 TopN,
		int32 MaxExcerptChars)
	{
		TArray<TSharedPtr<FJsonObject>> Evidence;
		const FString Query = TaskQuery.TrimStartAndEnd();
		if (Query.IsEmpty() || TopN <= 0)
		{
			return Evidence;
		}

		TArray<FKnowledgeCard> Cards;
		FString FailureReason;
		if (!LoadKnowledgeCards(GetKnowledgeIndexRoot(), Cards, FailureReason))
		{
			return Evidence;
		}

		const TArray<FString> QueryTokens = ExpandSearchTokens(Query);
		struct FScoredEvidenceCard
		{
			FKnowledgeCard Card;
			double Score = 0.0;
		};
		TArray<FScoredEvidenceCard> ScoredCards;
		for (const FKnowledgeCard& Card : Cards)
		{
			const double Score = ScoreKnowledgeCard(Card, Query, QueryTokens);
			if (Score > 0.0)
			{
				FScoredEvidenceCard Scored;
				Scored.Card = Card;
				Scored.Score = Score;
				ScoredCards.Add(MoveTemp(Scored));
			}
		}

		ScoredCards.Sort([](const FScoredEvidenceCard& Left, const FScoredEvidenceCard& Right)
		{
			if (!FMath::IsNearlyEqual(Left.Score, Right.Score))
			{
				return Left.Score > Right.Score;
			}
			return Left.Card.Title < Right.Card.Title;
		});

		TSet<FString> AddedSourceGroups;
		const int32 SafeTopN = FMath::Clamp(TopN, 1, 20);
		const int32 SafeMaxExcerptChars = FMath::Clamp(MaxExcerptChars, 80, 600);
		for (int32 Index = 0; Index < ScoredCards.Num() && Evidence.Num() < SafeTopN; ++Index)
		{
			const FScoredEvidenceCard& Scored = ScoredCards[Index];
			const FString SourceGroup = FString::Printf(TEXT("%s|%s"), *Scored.Card.SourceId, *Scored.Card.SectionPath);
			if (AddedSourceGroups.Contains(SourceGroup))
			{
				continue;
			}
			AddedSourceGroups.Add(SourceGroup);

			TSharedPtr<FJsonObject> EvidenceObject = MakeShared<FJsonObject>();
			EvidenceObject->SetStringField(TEXT("cardId"), Scored.Card.CardId);
			EvidenceObject->SetStringField(TEXT("sourcePath"), Scored.Card.SourcePath);
			EvidenceObject->SetStringField(TEXT("sourceKind"), Scored.Card.SourceKind);
			EvidenceObject->SetStringField(TEXT("excerpt"), MakeExcerpt(Scored.Card.Text, Query, QueryTokens, SafeMaxExcerptChars));
			EvidenceObject->SetNumberField(TEXT("score"), Scored.Score);
			EvidenceObject->SetStringField(TEXT("queryUsed"), Query);
			Evidence.Add(MoveTemp(EvidenceObject));
		}
		return Evidence;
	}

	bool WriteOutcomeKnowledgeCard(
		const FString& ManifestSessionId,
		const FString& Title,
		const FString& Text,
		const FString& SourcePath,
		const TArray<FString>& Tags,
		FString& OutFailureReason)
	{
		const FString CleanSessionId = ManifestSessionId.TrimStartAndEnd();
		if (CleanSessionId.IsEmpty())
		{
			OutFailureReason = TEXT("Cannot write outcome card without a manifest sessionId.");
			return false;
		}

		const FString SourceId = SanitizeKnowledgeId(FString::Printf(TEXT("outcome_%s"), *CleanSessionId));
		const FString CardsPath = FPaths::Combine(GetKnowledgeIndexRoot(), TEXT("cards.jsonl"));
		TArray<FKnowledgeCard> Cards;
		if (IFileManager::Get().FileSize(*CardsPath) >= 0)
		{
			TArray<FString> Lines;
			if (!FFileHelper::LoadFileToStringArray(Lines, *CardsPath))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to read knowledge cards '%s'."), *CardsPath);
				return false;
			}
			for (const FString& Line : Lines)
			{
				if (Line.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}
				TSharedPtr<FJsonObject> Object;
				FKnowledgeCard ExistingCard;
				if (LoadJsonObjectFromString(Line, Object) && JsonObjectToCard(Object, ExistingCard))
				{
					if (ExistingCard.SourceId.Equals(SourceId, ESearchCase::CaseSensitive))
					{
						return true;
					}
					Cards.Add(MoveTemp(ExistingCard));
				}
			}
		}

		FKnowledgeCard Card;
		Card.CardId = SourceId;
		Card.SourceId = SourceId;
		Card.Title = Title.TrimStartAndEnd().IsEmpty()
			? FString::Printf(TEXT("Outcome: %s"), *CleanSessionId)
			: Title.TrimStartAndEnd();
		Card.SectionTitle = Card.Title;
		Card.SectionPath = Card.Title;
		Card.Category = TEXT("outcome");
		for (const FString& Tag : Tags)
		{
			const FString CleanTag = Tag.TrimStartAndEnd();
			if (!CleanTag.IsEmpty())
			{
				Card.Tags.AddUnique(CleanTag);
			}
		}
		Card.SourceKind = TEXT("activity-log");
		Card.SourcePath = SourcePath.TrimStartAndEnd().IsEmpty() ? TEXT("Saved/UnrealMcp/LastExtensionApply.json") : SourcePath.TrimStartAndEnd();
		Card.Text = Text.Left(1800).TrimStartAndEnd();
		if (Card.Text.IsEmpty())
		{
			Card.Text = TEXT("Outcome verified.");
		}
		Card.ChunkIndex = 0;
		Card.TextLength = Card.Text.Len();
		Card.SourceWeight = 0.6;
		Card.Confidence = 0.7;
		Card.UpdatedAt = FDateTime::UtcNow().ToIso8601();
		Cards.Add(MoveTemp(Card));

		if (!WriteKnowledgeCardsJsonl(CardsPath, Cards, OutFailureReason))
		{
			return false;
		}
		InvalidateKnowledgeCardCache();
		return true;
	}

	FUnrealMcpExecutionResult ToolRecommend(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		FString Task;
		if (!Arguments.TryGetStringField(TEXT("task"), Task) || Task.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Missing required field 'task'."), nullptr, true);
		}

		FString RiskMax = TEXT("critical");
		Arguments.TryGetStringField(TEXT("riskMax"), RiskMax);
		const int32 Limit = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("limit"), 8), 1, 30);
		bool bIncludeKnowledge = true;
		bool bIncludeWorkflowDraft = true;
		Arguments.TryGetBoolField(TEXT("includeKnowledge"), bIncludeKnowledge);
		Arguments.TryGetBoolField(TEXT("includeWorkflowDraft"), bIncludeWorkflowDraft);

		const TArray<FToolRecommendationCandidate> ScoredTools = FindToolRecommendations(Task, RiskMax);
		TArray<TSharedPtr<FJsonValue>> RecommendationValues = MakeRecommendationValues(ScoredTools, Limit);

		TArray<TSharedPtr<FJsonValue>> KnowledgeValues;
		FString KnowledgeNote;
		if (bIncludeKnowledge)
		{
			TArray<FKnowledgeCard> Cards;
			FString FailureReason;
			if (LoadKnowledgeCards(GetKnowledgeIndexRoot(), Cards, FailureReason))
			{
				const TArray<FString> QueryTokens = ExpandSearchTokens(Task);
				struct FScoredCard
				{
					FKnowledgeCard Card;
					double Score = 0.0;
				};
				TArray<FScoredCard> ScoredCards;
				for (const FKnowledgeCard& Card : Cards)
				{
					const double Score = ScoreKnowledgeCard(Card, Task, QueryTokens);
					if (Score > 0.0)
					{
						FScoredCard Scored;
						Scored.Card = Card;
						Scored.Score = Score;
						ScoredCards.Add(MoveTemp(Scored));
					}
				}
				ScoredCards.Sort([](const FScoredCard& Left, const FScoredCard& Right)
				{
					return Left.Score > Right.Score;
				});
				TSet<FString> AddedKnowledgeGroups;
				for (int32 Index = 0; Index < ScoredCards.Num() && KnowledgeValues.Num() < 3; ++Index)
				{
					const FString SourceGroup = FString::Printf(TEXT("%s|%s"), *ScoredCards[Index].Card.SourceId, *ScoredCards[Index].Card.SectionPath);
					if (AddedKnowledgeGroups.Contains(SourceGroup))
					{
						continue;
					}
					AddedKnowledgeGroups.Add(SourceGroup);

					TSharedPtr<FJsonObject> CardObject = MakeShared<FJsonObject>();
					CardObject->SetStringField(TEXT("cardId"), ScoredCards[Index].Card.CardId);
					CardObject->SetStringField(TEXT("title"), ScoredCards[Index].Card.Title);
					CardObject->SetStringField(TEXT("sectionTitle"), ScoredCards[Index].Card.SectionTitle);
					CardObject->SetStringField(TEXT("category"), ScoredCards[Index].Card.Category);
					CardObject->SetStringField(TEXT("sourcePath"), ScoredCards[Index].Card.SourcePath);
					CardObject->SetStringField(TEXT("excerpt"), MakeExcerpt(ScoredCards[Index].Card.Text, Task, QueryTokens, 320));
					CardObject->SetNumberField(TEXT("score"), ScoredCards[Index].Score);
					KnowledgeValues.Add(MakeShared<FJsonValueObject>(CardObject));
				}
			}
			else
			{
				KnowledgeNote = FailureReason;
			}
		}

		TArray<TSharedPtr<FJsonValue>> WorkflowSteps;
		if (bIncludeWorkflowDraft)
		{
			auto AddStep = [&WorkflowSteps](const FString& ToolName, const FString& Purpose)
			{
				TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("tool"), ToolName);
				Step->SetStringField(TEXT("purpose"), Purpose);
				WorkflowSteps.Add(MakeShared<FJsonValueObject>(Step));
			};

			AddStep(TEXT("unreal.preview_change_plan"), TEXT("Turn the task into a bounded plan with risk and verification gates."));
			if (!KnowledgeNote.IsEmpty())
			{
				AddStep(TEXT("unreal.knowledge_index_refresh"), TEXT("Refresh the local knowledge index before searching docs and tool cards."));
			}
			AddStep(TEXT("unreal.knowledge_search"), TEXT("Retrieve relevant docs, tool cards, tests, and workflow notes."));
			if (RecommendationValues.Num() > 0)
			{
				const TSharedPtr<FJsonObject> FirstTool = RecommendationValues[0]->AsObject();
				if (FirstTool.IsValid())
				{
					AddStep(FirstTool->GetStringField(TEXT("toolName")), TEXT("Run the top recommended task-specific tool, preferably dry-run first when supported."));
				}
			}
			AddStep(TEXT("unreal.verify_task_outcome"), TEXT("Verify the task outcome with evidence rather than relying on a prose summary."));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("tool_recommend"));
		StructuredContent->SetStringField(TEXT("task"), Task);
		StructuredContent->SetStringField(TEXT("riskMax"), RiskMax);
		StructuredContent->SetNumberField(TEXT("visibleToolDefinitionCount"), ToolsArray.Num());
		StructuredContent->SetArrayField(TEXT("recommendations"), RecommendationValues);
		StructuredContent->SetArrayField(TEXT("knowledgeCards"), KnowledgeValues);
		StructuredContent->SetArrayField(TEXT("workflowDraft"), WorkflowSteps);
		if (!KnowledgeNote.IsEmpty())
		{
			StructuredContent->SetStringField(TEXT("knowledgeNote"), KnowledgeNote);
		}
		return MakeExecutionResult(
			FString::Printf(TEXT("Recommended %d tool(s) for this task."), RecommendationValues.Num()),
			StructuredContent,
			false);
	}

	FUnrealMcpExecutionResult ToolGapAnalyze(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		FString Task;
		if (!Arguments.TryGetStringField(TEXT("task"), Task) || Task.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Missing required field 'task'."), nullptr, true);
		}

		FString RiskMax = TEXT("critical");
		Arguments.TryGetStringField(TEXT("riskMax"), RiskMax);
		const int32 Limit = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("limit"), 6), 1, 20);

		const FString TaskLower = Task.ToLower();
		const bool bExplicitNewTool =
			TaskLower.Contains(TEXT("new tool"))
			|| TaskLower.Contains(TEXT("add tool"))
			|| TaskLower.Contains(TEXT("新增工具"))
			|| TaskLower.Contains(TEXT("新工具"))
			|| TaskLower.Contains(TEXT("扩展工具"))
			|| TaskLower.Contains(TEXT("自拓展"))
			|| TaskLower.Contains(TEXT("自扩展"));

		const TArray<FToolRecommendationCandidate> ScoredTools = FindToolRecommendations(Task, RiskMax);
		const double TopScore = ScoredTools.Num() > 0 ? ScoredTools[0].Score : 0.0;
		int32 StrongToolCount = 0;
		for (const FToolRecommendationCandidate& Candidate : ScoredTools)
		{
			if (Candidate.Score >= 42.0)
			{
				StrongToolCount++;
			}
		}

		FString Decision;
		FString Reason;
		double Confidence = 0.6;
		if (bExplicitNewTool && TopScore < 75.0)
		{
			Decision = TEXT("scaffold_new_tool");
			Reason = TEXT("The task explicitly asks for new tool capability and no existing tool is a strong enough match.");
			Confidence = 0.78;
		}
		else if (TopScore >= 68.0 && StrongToolCount <= 1)
		{
			Decision = TEXT("use_existing_tool");
			Reason = TEXT("A single existing MCP tool has a strong semantic and policy match.");
			Confidence = 0.84;
		}
		else if (TopScore >= 35.0 || StrongToolCount >= 2)
		{
			Decision = TEXT("compose_existing_tools");
			Reason = TEXT("Existing MCP tools cover meaningful parts of the task; use a bounded workflow before scaffolding anything new.");
			Confidence = 0.76;
		}
		else
		{
			Decision = TEXT("scaffold_new_tool");
			Reason = TEXT("No existing visible MCP tool has enough overlap with the task; scaffold a descriptor-first tool behind the self-extension gate.");
			Confidence = 0.68;
		}

		FString SuggestedCategory = TEXT("self-extension");
		if (TaskLower.Contains(TEXT("blueprint")) || TaskLower.Contains(TEXT("蓝图")))
		{
			SuggestedCategory = TEXT("blueprint");
		}
		else if (TaskLower.Contains(TEXT("widget")) || TaskLower.Contains(TEXT("umg")) || TaskLower.Contains(TEXT("ui")) || TaskLower.Contains(TEXT("界面")))
		{
			SuggestedCategory = TEXT("widget");
		}
		else if (TaskLower.Contains(TEXT("actor")) || TaskLower.Contains(TEXT("level")) || TaskLower.Contains(TEXT("spawn")) || TaskLower.Contains(TEXT("场景")))
		{
			SuggestedCategory = TEXT("actors");
		}

		TSharedPtr<FJsonObject> ScaffoldHints = MakeShared<FJsonObject>();
		ScaffoldHints->SetStringField(TEXT("suggestedCategory"), SuggestedCategory);
		ScaffoldHints->SetStringField(TEXT("suggestedRisk"), SuggestedCategory == TEXT("self-extension") ? TEXT("high") : TEXT("medium"));
		ScaffoldHints->SetArrayField(TEXT("schemaHints"), MakeJsonStringArray(TArray<FString>{
			TEXT("task-specific required fields should be explicit and typed"),
			TEXT("include dryRun when the tool can mutate project assets or source"),
			TEXT("return structuredContent with action, changed paths, warnings, and verification evidence")
		}));
		ScaffoldHints->SetArrayField(TEXT("testIdeas"), MakeJsonStringArray(TArray<FString>{
			TEXT("happy path with disposable sandbox data"),
			TEXT("missing required field"),
			TEXT("invalid path or unsupported type"),
			TEXT("dryRun does not mutate project state")
		}));
		ScaffoldHints->SetArrayField(TEXT("selfExtensionGate"), MakeJsonStringArray(TArray<FString>{
			TEXT("unreal.preview_change_plan"),
			TEXT("unreal.scaffold_mcp_tool"),
			TEXT("unreal.mcp_validate_tool_schema"),
			TEXT("unreal.mcp_apply_scaffold dryRun"),
			TEXT("unreal.mcp_apply_scaffold"),
			TEXT("unreal.mcp_build_editor"),
			TEXT("unreal.mcp_run_tool_test or unreal.mcp_run_test_suite"),
			TEXT("unreal.verify_task_outcome")
		}));

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("tool_gap_analyze"));
		StructuredContent->SetStringField(TEXT("task"), Task);
		StructuredContent->SetStringField(TEXT("decision"), Decision);
		StructuredContent->SetStringField(TEXT("reason"), Reason);
		StructuredContent->SetNumberField(TEXT("confidence"), Confidence);
		StructuredContent->SetNumberField(TEXT("topScore"), TopScore);
		StructuredContent->SetNumberField(TEXT("strongToolCount"), StrongToolCount);
		StructuredContent->SetNumberField(TEXT("visibleToolDefinitionCount"), ToolsArray.Num());
		StructuredContent->SetArrayField(TEXT("recommendedExistingTools"), MakeRecommendationValues(ScoredTools, Limit));
		StructuredContent->SetObjectField(TEXT("scaffoldHints"), ScaffoldHints);

		return MakeExecutionResult(
			FString::Printf(TEXT("Tool gap analysis decision: %s."), *Decision),
			StructuredContent,
			false);
	}

	FUnrealMcpExecutionResult WorkflowRecommend(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		FString Task;
		if (!Arguments.TryGetStringField(TEXT("task"), Task) || Task.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Missing required field 'task'."), nullptr, true);
		}

		FString RiskMax = TEXT("critical");
		Arguments.TryGetStringField(TEXT("riskMax"), RiskMax);
		const int32 Limit = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("limit"), 5), 1, 12);
		bool bIncludeKnowledge = true;
		bool bDryRun = true;
		Arguments.TryGetBoolField(TEXT("includeKnowledge"), bIncludeKnowledge);
		Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

		const TArray<FToolRecommendationCandidate> ScoredTools = FindToolRecommendations(Task, RiskMax);
		bool bAnyWriteTool = false;
		for (int32 Index = 0; Index < FMath::Min(Limit, ScoredTools.Num()); ++Index)
		{
			if (ScoredTools[Index].Entry && ScoredTools[Index].Entry->Policy.bRequiresWrite)
			{
				bAnyWriteTool = true;
				break;
			}
		}

		TArray<TSharedPtr<FJsonValue>> Steps;
		TSharedPtr<FJsonObject> PreviewArgs = MakeShared<FJsonObject>();
		PreviewArgs->SetStringField(TEXT("task"), Task);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStepObject(
			TEXT("unreal.preview_change_plan"),
			TEXT("Convert the user request into a bounded plan with risk and verification gates."),
			PreviewArgs)));

		if (bIncludeKnowledge)
		{
			TSharedPtr<FJsonObject> SearchArgs = MakeShared<FJsonObject>();
			SearchArgs->SetStringField(TEXT("query"), Task);
			SearchArgs->SetNumberField(TEXT("limit"), 5.0);
			SearchArgs->SetBoolField(TEXT("includeText"), false);
			Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStepObject(
				TEXT("unreal.knowledge_search"),
				TEXT("Retrieve relevant KnowledgeCards before selecting exact tool arguments."),
				SearchArgs)));
		}

		TSharedPtr<FJsonObject> GapArgs = MakeShared<FJsonObject>();
		GapArgs->SetStringField(TEXT("task"), Task);
		GapArgs->SetStringField(TEXT("riskMax"), RiskMax);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStepObject(
			TEXT("unreal.tool_gap_analyze"),
			TEXT("Decide whether existing tools are enough or a new MCP tool should be scaffolded."),
			GapArgs)));

		if (bAnyWriteTool)
		{
			TSharedPtr<FJsonObject> SnapshotArgs = MakeShared<FJsonObject>();
			SnapshotArgs->SetStringField(TEXT("snapshotName"), TEXT("before_workflow"));
			Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStepObject(
				TEXT("unreal.capture_project_snapshot"),
				TEXT("Capture objective before-state evidence before write tools run."),
				SnapshotArgs)));
		}

		for (int32 Index = 0; Index < FMath::Min(Limit, ScoredTools.Num()); ++Index)
		{
			if (!ScoredTools[Index].Entry)
			{
				continue;
			}

			TSharedPtr<FJsonObject> PlaceholderArgs = MakeShared<FJsonObject>();
			PlaceholderArgs->SetStringField(TEXT("_todo"), TEXT("Fill exact arguments from the task and prior step evidence before executing this step."));
			PlaceholderArgs->SetBoolField(TEXT("dryRun"), ScoredTools[Index].Entry->Policy.bDryRunSupport);
			Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStepObject(
				ScoredTools[Index].Entry->Name,
				TEXT("Task-specific recommended tool. Keep skipped until exact arguments are known."),
				PlaceholderArgs,
				true)));
		}

		TSharedPtr<FJsonObject> VerifyArgs = MakeShared<FJsonObject>();
		VerifyArgs->SetStringField(TEXT("task"), Task);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStepObject(
			TEXT("unreal.verify_task_outcome"),
			TEXT("Verify the final result with evidence instead of relying on a prose summary."),
			VerifyArgs)));

		TSharedPtr<FJsonObject> WorkflowRunDraft = MakeShared<FJsonObject>();
		WorkflowRunDraft->SetStringField(TEXT("workflowName"), TEXT("rag_recommended_workflow"));
		WorkflowRunDraft->SetBoolField(TEXT("dryRun"), bDryRun);
		WorkflowRunDraft->SetBoolField(TEXT("stopOnFailure"), true);
		WorkflowRunDraft->SetBoolField(TEXT("writeMemory"), true);
		WorkflowRunDraft->SetStringField(TEXT("memoryKey"), TEXT("chat.active_task"));
		WorkflowRunDraft->SetArrayField(TEXT("steps"), Steps);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("workflow_recommend"));
		StructuredContent->SetStringField(TEXT("task"), Task);
		StructuredContent->SetStringField(TEXT("riskMax"), RiskMax);
		StructuredContent->SetNumberField(TEXT("visibleToolDefinitionCount"), ToolsArray.Num());
		StructuredContent->SetArrayField(TEXT("recommendedTools"), MakeRecommendationValues(ScoredTools, Limit));
		StructuredContent->SetObjectField(TEXT("workflowRunDraft"), WorkflowRunDraft);
		StructuredContent->SetStringField(TEXT("note"), TEXT("Generated task-specific steps are intentionally skipped until exact arguments are filled from retrieved evidence."));

		return MakeExecutionResult(
			FString::Printf(TEXT("Workflow recommendation created with %d step(s)."), Steps.Num()),
			StructuredContent,
			false);
	}

	FUnrealMcpExecutionResult KnowledgeEvalRun(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		FString EvalPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpKnowledge/Evals"));
		Arguments.TryGetStringField(TEXT("evalPath"), EvalPath);
		EvalPath = ResolveProjectPathForJson(EvalPath);

		bool bRefreshIndex = false;
		bool bIncludeDetails = true;
		Arguments.TryGetBoolField(TEXT("refreshIndex"), bRefreshIndex);
		Arguments.TryGetBoolField(TEXT("includeDetails"), bIncludeDetails);
		const int32 Limit = FMath::Clamp(GetPositiveIntArgument(Arguments, TEXT("limit"), 6), 1, 30);

		TArray<FString> EvalFiles;
		if (FPaths::DirectoryExists(EvalPath))
		{
			IFileManager::Get().FindFilesRecursive(EvalFiles, *EvalPath, TEXT("*.json"), true, false);
			EvalFiles.Sort();
		}
		else if (FPaths::FileExists(EvalPath))
		{
			EvalFiles.Add(EvalPath);
		}
		else
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("knowledge_eval_run"));
			StructuredContent->SetStringField(TEXT("evalPath"), MakeProjectRelativePath(EvalPath));
			return MakeExecutionResult(FString::Printf(TEXT("Knowledge eval path was not found: %s"), *EvalPath), StructuredContent, true);
		}

		if (bRefreshIndex)
		{
			TSharedPtr<FJsonObject> RefreshArguments = MakeShared<FJsonObject>();
			RefreshArguments->SetBoolField(TEXT("includeOfficialDocs"), true);
			RefreshArguments->SetBoolField(TEXT("includeVersionedDocs"), true);
			RefreshArguments->SetBoolField(TEXT("includeToolRegistry"), true);
			RefreshArguments->SetBoolField(TEXT("skipLowContent"), true);
			const FUnrealMcpExecutionResult RefreshResult = KnowledgeIndexRefresh(*RefreshArguments);
			if (RefreshResult.bIsError)
			{
				return RefreshResult;
			}
		}

		auto TryGetStringArrayOrSingle = [](const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<FString>& OutValues)
		{
			OutValues.Reset();
			if (!Object.IsValid())
			{
				return false;
			}
			if (TryGetStringArrayFromObject(Object, FieldName, OutValues))
			{
				return true;
			}
			FString SingleValue;
			if (Object->TryGetStringField(FieldName, SingleValue) && !SingleValue.IsEmpty())
			{
				OutValues.Add(SingleValue);
				return true;
			}
			return false;
		};

		auto JsonArrayHasStringField = [](const TArray<TSharedPtr<FJsonValue>>* Values, const FString& FieldName, const FString& Expected)
		{
			if (!Values)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid() || !Value->AsObject().IsValid())
				{
					continue;
				}
				FString Actual;
				if (Value->AsObject()->TryGetStringField(FieldName, Actual) && Actual.Equals(Expected, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		};

		auto JsonArrayHasAnyStringField = [&JsonArrayHasStringField](const TArray<TSharedPtr<FJsonValue>>* Values, const FString& FieldName, const TArray<FString>& ExpectedValues)
		{
			for (const FString& Expected : ExpectedValues)
			{
				if (JsonArrayHasStringField(Values, FieldName, Expected))
				{
					return true;
				}
			}
			return ExpectedValues.IsEmpty();
		};

		auto WorkflowStepsContainTool = [](const TSharedPtr<FJsonObject>& Root, const FString& ExpectedTool)
		{
			if (!Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* WorkflowObject = nullptr;
			if (!Root->TryGetObjectField(TEXT("workflowRunDraft"), WorkflowObject) || !WorkflowObject || !(*WorkflowObject).IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
			if (!(*WorkflowObject)->TryGetArrayField(TEXT("steps"), Steps) || !Steps)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& StepValue : *Steps)
			{
				if (!StepValue.IsValid() || !StepValue->AsObject().IsValid())
				{
					continue;
				}
				FString ToolName;
				if (StepValue->AsObject()->TryGetStringField(TEXT("tool"), ToolName) && ToolName.Equals(ExpectedTool, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		};

		TArray<TSharedPtr<FJsonValue>> CaseResultValues;
		int32 CaseCount = 0;
		int32 PassedCount = 0;
		int32 FailedCount = 0;
		int32 FileCount = 0;

		auto EvaluateCase = [&](const TSharedPtr<FJsonObject>& CaseObject, const FString& SourceFile)
		{
			if (!CaseObject.IsValid())
			{
				return;
			}

			CaseCount++;
			FString Name;
			FString Type;
			CaseObject->TryGetStringField(TEXT("name"), Name);
			CaseObject->TryGetStringField(TEXT("type"), Type);
			if (Name.IsEmpty())
			{
				Name = FString::Printf(TEXT("case_%d"), CaseCount);
			}
			if (Type.IsEmpty())
			{
				Type = TEXT("search");
			}

			TArray<FString> Failures;
			FUnrealMcpExecutionResult Result;
			if (Type.Equals(TEXT("search"), ESearchCase::IgnoreCase))
			{
				FString Query;
				CaseObject->TryGetStringField(TEXT("query"), Query);
				TSharedPtr<FJsonObject> SearchArgs = MakeShared<FJsonObject>();
				SearchArgs->SetStringField(TEXT("query"), Query);
				SearchArgs->SetNumberField(TEXT("limit"), static_cast<double>(Limit));
				SearchArgs->SetBoolField(TEXT("includeText"), false);
				TArray<FString> SearchSourceKinds;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("sourceKinds"), SearchSourceKinds))
				{
					SearchArgs->SetArrayField(TEXT("sourceKinds"), MakeJsonStringArray(SearchSourceKinds));
				}
				Result = KnowledgeSearch(*SearchArgs);

				const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
				if (!Result.StructuredContent.IsValid() || !Result.StructuredContent->TryGetArrayField(TEXT("results"), Results) || !Results || Results->IsEmpty())
				{
					Failures.Add(TEXT("Expected at least one knowledge_search result."));
				}

				TArray<FString> ExpectedCategories;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("expectCategories"), ExpectedCategories)
					&& !JsonArrayHasAnyStringField(Results, TEXT("category"), ExpectedCategories))
				{
					Failures.Add(TEXT("Expected at least one matching result category."));
				}

				TArray<FString> ExpectedSourceKinds;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("expectSourceKinds"), ExpectedSourceKinds)
					&& !JsonArrayHasAnyStringField(Results, TEXT("sourceKind"), ExpectedSourceKinds))
				{
					Failures.Add(TEXT("Expected at least one matching sourceKind."));
				}

				// Search eval extension keys:
				// - sourceKinds: forwards the same filter accepted by knowledge_search.
				// - expectKindStatusContains: requires at least one kindStatus value to match each listed status.
				TArray<FString> ExpectedKindStatuses;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("expectKindStatusContains"), ExpectedKindStatuses))
				{
					const TSharedPtr<FJsonObject>* KindStatus = nullptr;
					if (!Result.StructuredContent.IsValid()
						|| !Result.StructuredContent->TryGetObjectField(TEXT("kindStatus"), KindStatus)
						|| !KindStatus
						|| !(*KindStatus).IsValid())
					{
						Failures.Add(TEXT("Expected knowledge_search kindStatus object."));
					}
					else
					{
						for (const FString& ExpectedStatus : ExpectedKindStatuses)
						{
							bool bFoundStatus = false;
							for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*KindStatus)->Values)
							{
								if (Pair.Value.IsValid()
									&& Pair.Value->Type == EJson::String
									&& Pair.Value->AsString().Equals(ExpectedStatus, ESearchCase::IgnoreCase))
								{
									bFoundStatus = true;
									break;
								}
							}
							if (!bFoundStatus)
							{
								Failures.Add(FString::Printf(TEXT("Expected kindStatus to contain status '%s'."), *ExpectedStatus));
							}
						}
					}
				}
			}
			else if (Type.Equals(TEXT("tool_recommend"), ESearchCase::IgnoreCase))
			{
				FString Task;
				FString RiskMax = TEXT("critical");
				CaseObject->TryGetStringField(TEXT("task"), Task);
				CaseObject->TryGetStringField(TEXT("riskMax"), RiskMax);
				TSharedPtr<FJsonObject> RecommendArgs = MakeShared<FJsonObject>();
				RecommendArgs->SetStringField(TEXT("task"), Task);
				RecommendArgs->SetStringField(TEXT("riskMax"), RiskMax);
				RecommendArgs->SetNumberField(TEXT("limit"), static_cast<double>(Limit));
				RecommendArgs->SetBoolField(TEXT("includeKnowledge"), true);
				RecommendArgs->SetBoolField(TEXT("includeWorkflowDraft"), true);
				Result = ToolRecommend(*RecommendArgs, ToolsArray);

				const TArray<TSharedPtr<FJsonValue>>* Recommendations = nullptr;
				if (!Result.StructuredContent.IsValid() || !Result.StructuredContent->TryGetArrayField(TEXT("recommendations"), Recommendations) || !Recommendations || Recommendations->IsEmpty())
				{
					Failures.Add(TEXT("Expected at least one tool recommendation."));
				}

				TArray<FString> ExpectedAnyTools;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("expectAnyTools"), ExpectedAnyTools)
					&& !JsonArrayHasAnyStringField(Recommendations, TEXT("toolName"), ExpectedAnyTools))
				{
					Failures.Add(TEXT("Expected at least one specific recommended tool."));
				}
			}
			else if (Type.Equals(TEXT("tool_gap_analyze"), ESearchCase::IgnoreCase))
			{
				FString Task;
				FString RiskMax = TEXT("critical");
				CaseObject->TryGetStringField(TEXT("task"), Task);
				CaseObject->TryGetStringField(TEXT("riskMax"), RiskMax);
				TSharedPtr<FJsonObject> GapArgs = MakeShared<FJsonObject>();
				GapArgs->SetStringField(TEXT("task"), Task);
				GapArgs->SetStringField(TEXT("riskMax"), RiskMax);
				GapArgs->SetNumberField(TEXT("limit"), static_cast<double>(Limit));
				Result = ToolGapAnalyze(*GapArgs, ToolsArray);

				FString Decision;
				if (!Result.StructuredContent.IsValid() || !Result.StructuredContent->TryGetStringField(TEXT("decision"), Decision))
				{
					Failures.Add(TEXT("Expected a tool gap decision."));
				}
				TArray<FString> ExpectedDecisions;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("expectDecisionIn"), ExpectedDecisions)
					&& !ExpectedDecisions.ContainsByPredicate([&Decision](const FString& Expected)
					{
						return Decision.Equals(Expected, ESearchCase::IgnoreCase);
					}))
				{
					Failures.Add(TEXT("Gap decision was outside the expected set."));
				}
			}
			else if (Type.Equals(TEXT("workflow_recommend"), ESearchCase::IgnoreCase))
			{
				FString Task;
				FString RiskMax = TEXT("critical");
				CaseObject->TryGetStringField(TEXT("task"), Task);
				CaseObject->TryGetStringField(TEXT("riskMax"), RiskMax);
				TSharedPtr<FJsonObject> WorkflowArgs = MakeShared<FJsonObject>();
				WorkflowArgs->SetStringField(TEXT("task"), Task);
				WorkflowArgs->SetStringField(TEXT("riskMax"), RiskMax);
				WorkflowArgs->SetNumberField(TEXT("limit"), static_cast<double>(Limit));
				WorkflowArgs->SetBoolField(TEXT("includeKnowledge"), true);
				WorkflowArgs->SetBoolField(TEXT("dryRun"), true);
				Result = WorkflowRecommend(*WorkflowArgs, ToolsArray);

				TArray<FString> ExpectedWorkflowTools;
				if (TryGetStringArrayOrSingle(CaseObject, TEXT("expectWorkflowTools"), ExpectedWorkflowTools))
				{
					for (const FString& ExpectedTool : ExpectedWorkflowTools)
					{
						if (!WorkflowStepsContainTool(Result.StructuredContent, ExpectedTool))
						{
							Failures.Add(FString::Printf(TEXT("Workflow draft did not include expected tool '%s'."), *ExpectedTool));
						}
					}
				}
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("Unsupported eval case type '%s'."), *Type));
			}

			if (Result.bIsError)
			{
				Failures.Add(Result.Text);
			}

			const bool bPassed = Failures.IsEmpty();
			if (bPassed)
			{
				PassedCount++;
			}
			else
			{
				FailedCount++;
			}

			TSharedPtr<FJsonObject> CaseResult = MakeShared<FJsonObject>();
			CaseResult->SetStringField(TEXT("name"), Name);
			CaseResult->SetStringField(TEXT("type"), Type);
			CaseResult->SetStringField(TEXT("sourceFile"), MakeProjectRelativePath(SourceFile));
			CaseResult->SetBoolField(TEXT("passed"), bPassed);
			CaseResult->SetArrayField(TEXT("failures"), MakeJsonStringArray(Failures));
			if (bIncludeDetails && Result.StructuredContent.IsValid())
			{
				CaseResult->SetObjectField(TEXT("structuredContent"), Result.StructuredContent);
			}
			CaseResultValues.Add(MakeShared<FJsonValueObject>(CaseResult));
		};

		for (const FString& EvalFile : EvalFiles)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *EvalFile))
			{
				continue;
			}

			TSharedPtr<FJsonObject> RootObject;
			if (!LoadJsonObjectFromString(JsonText, RootObject) || !RootObject.IsValid())
			{
				continue;
			}
			FileCount++;

			const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
			if (RootObject->TryGetArrayField(TEXT("cases"), Cases) && Cases)
			{
				for (const TSharedPtr<FJsonValue>& CaseValue : *Cases)
				{
					if (CaseValue.IsValid() && CaseValue->AsObject().IsValid())
					{
						EvaluateCase(CaseValue->AsObject(), EvalFile);
					}
				}
			}
			else
			{
				EvaluateCase(RootObject, EvalFile);
			}
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("knowledge_eval_run"));
		StructuredContent->SetStringField(TEXT("evalPath"), MakeProjectRelativePath(EvalPath));
		StructuredContent->SetNumberField(TEXT("fileCount"), FileCount);
		StructuredContent->SetNumberField(TEXT("caseCount"), CaseCount);
		StructuredContent->SetNumberField(TEXT("passedCount"), PassedCount);
		StructuredContent->SetNumberField(TEXT("failedCount"), FailedCount);
		StructuredContent->SetNumberField(TEXT("passRate"), CaseCount > 0 ? static_cast<double>(PassedCount) / static_cast<double>(CaseCount) : 0.0);
		StructuredContent->SetArrayField(TEXT("cases"), CaseResultValues);

		const bool bIsError = CaseCount == 0 || FailedCount > 0;
		return MakeExecutionResult(
			FString::Printf(TEXT("Knowledge eval run: %d/%d case(s) passed."), PassedCount, CaseCount),
			StructuredContent,
			bIsError);
	}
}
