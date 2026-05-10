#include "UnrealMcpChatPanel.h"

#include "Algo/Reverse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HttpModule.h"
#include "ISettingsModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "UnrealMcpModule.h"
#include "UnrealMcpSettings.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UnrealMcpChatPanel"

namespace UnrealMcpChat
{
	static constexpr int32 AssistantHistoryMaxEntries = 16;
	static constexpr int32 AssistantHistoryMaxChars = 7000;
	static constexpr int32 AssistantHistoryMaxCharsPerEntry = 650;
	static constexpr int32 AssistantToolResultMaxChars = 420;
	static constexpr int32 AssistantToolArgumentsMaxChars = 180;
	static constexpr int32 AssistantActiveTaskMaxChars = 2200;
	static constexpr int32 AiTestResponsePreviewMaxChars = 3000;

	const FString& SkillApplyModeReadOnly()
	{
		static const FString Value = TEXT("Read Only");
		return Value;
	}

	const FString& SkillApplyModeApplyToMemory()
	{
		static const FString Value = TEXT("Apply to Memory");
		return Value;
	}

	const FString& SkillApplyModeInsertPrompt()
	{
		static const FString Value = TEXT("Insert Prompt");
		return Value;
	}

	const FString& SkillApplyModeAskNow()
	{
		static const FString Value = TEXT("Ask Now");
		return Value;
	}

	FString GetHistoryFilePath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp"), TEXT("ChatHistory.json"));
	}

	FString GetChatPanelStateFilePath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp"), TEXT("chat_panel.json"));
	}

	const FAiProviderConfig* FindProviderById(const UUnrealMcpSettings& Settings, const FString& ProviderId)
	{
		for (const FAiProviderConfig& Provider : Settings.Providers)
		{ if (Provider.Id.Equals(ProviderId, ESearchCase::CaseSensitive)) { return &Provider; } }
		return nullptr;
	}

	FAiProviderConfig* FindMutableProviderById(UUnrealMcpSettings& Settings, const FString& ProviderId)
	{
		for (FAiProviderConfig& Provider : Settings.Providers)
		{ if (Provider.Id.Equals(ProviderId, ESearchCase::CaseSensitive)) { return &Provider; } }
		return nullptr;
	}

	FString ProviderKindShortName(EAiProviderKind Kind)
	{
		switch (Kind)
		{ case EAiProviderKind::OpenAiResponses: return TEXT("OpenAI"); case EAiProviderKind::OpenAiChatCompat: return TEXT("OpenAI-compat"); case EAiProviderKind::AnthropicMessages: return TEXT("Anthropic"); case EAiProviderKind::Codex: return TEXT("Codex"); case EAiProviderKind::CodexAppServer: return TEXT("CodexDesktop"); default: return TEXT("Unknown"); }
	}

	FString FormatProviderLabel(const FAiProviderConfig& Provider)
	{
		const FString DisplayName = Provider.DisplayName.TrimStartAndEnd().IsEmpty() ? Provider.Id : Provider.DisplayName.TrimStartAndEnd();
		return FString::Printf(TEXT("%s (%s)"), *DisplayName, *ProviderKindShortName(Provider.Kind));
	}

	FLinearColor GetBorderColor(const FUnrealMcpChatEntry& Entry)
	{
		switch (Entry.Type)
		{
		case EUnrealMcpChatEntryType::User:
			return FLinearColor(0.22f, 0.46f, 0.80f, 1.0f);
		case EUnrealMcpChatEntryType::Assistant:
			return Entry.bIsError ? FLinearColor(0.76f, 0.24f, 0.24f, 1.0f) : FLinearColor(0.20f, 0.58f, 0.36f, 1.0f);
		case EUnrealMcpChatEntryType::Tool:
			return Entry.bIsError ? FLinearColor(0.76f, 0.34f, 0.18f, 1.0f) : FLinearColor(0.82f, 0.60f, 0.20f, 1.0f);
		case EUnrealMcpChatEntryType::System:
		default:
			return FLinearColor(0.42f, 0.46f, 0.55f, 1.0f);
		}
	}

	FLinearColor GetBackgroundColor(const FUnrealMcpChatEntry& Entry)
	{
		if (Entry.bIsError)
		{
			return FLinearColor(0.40f, 0.10f, 0.10f, 0.40f);
		}

		switch (Entry.Type)
		{
		case EUnrealMcpChatEntryType::User:
			return FLinearColor(0.08f, 0.12f, 0.18f, 0.95f);
		case EUnrealMcpChatEntryType::Assistant:
			return FLinearColor(0.08f, 0.16f, 0.10f, 0.95f);
		case EUnrealMcpChatEntryType::Tool:
			return FLinearColor(0.18f, 0.14f, 0.07f, 0.95f);
		case EUnrealMcpChatEntryType::System:
		default:
			return FLinearColor(0.11f, 0.12f, 0.14f, 0.95f);
		}
	}

	FString EntryTypeToString(EUnrealMcpChatEntryType Type)
	{
		switch (Type)
		{
		case EUnrealMcpChatEntryType::User:
			return TEXT("user");
		case EUnrealMcpChatEntryType::Assistant:
			return TEXT("assistant");
		case EUnrealMcpChatEntryType::Tool:
			return TEXT("tool");
		case EUnrealMcpChatEntryType::System:
		default:
			return TEXT("system");
		}
	}

	EUnrealMcpChatEntryType EntryTypeFromString(const FString& TypeString)
	{
		if (TypeString.Equals(TEXT("user"), ESearchCase::IgnoreCase))
		{
			return EUnrealMcpChatEntryType::User;
		}

		if (TypeString.Equals(TEXT("assistant"), ESearchCase::IgnoreCase))
		{
			return EUnrealMcpChatEntryType::Assistant;
		}

		if (TypeString.Equals(TEXT("tool"), ESearchCase::IgnoreCase))
		{
			return EUnrealMcpChatEntryType::Tool;
		}

		return EUnrealMcpChatEntryType::System;
	}

	FString BuildEntryClipboardText(const FUnrealMcpChatEntry& Entry)
	{
		TArray<FString> Lines;

		FString Header = Entry.Speaker;
		if (Entry.Type == EUnrealMcpChatEntryType::Tool)
		{
			const FString Status = Entry.bIsPending ? TEXT("running") : (Entry.bIsError ? TEXT("error") : TEXT("done"));
			Header = FString::Printf(TEXT("Tool: %s (%s)"), *Entry.Title, *Status);
		}
		else if (!Entry.Title.IsEmpty() && !Entry.Title.Equals(Entry.Speaker, ESearchCase::CaseSensitive))
		{
			Header = FString::Printf(TEXT("%s: %s"), *Entry.Speaker, *Entry.Title);
		}

		Lines.Add(Header);

		if (!Entry.Body.IsEmpty())
		{
			Lines.Add(Entry.Body);
		}

		if (!Entry.Details.IsEmpty())
		{
			Lines.Add(TEXT("Details:"));
			Lines.Add(Entry.Details);
		}

		return FString::Join(Lines, TEXT("\n"));
	}

	FString GetToolStatusIcon(const FUnrealMcpChatEntry& Entry)
	{
		if (Entry.bIsPending)
		{
			return TEXT("...");
		}

		return Entry.bIsError ? TEXT("✗") : TEXT("✓");
	}

	FString BuildEntryTitleText(const FUnrealMcpChatEntry& Entry)
	{
		const FString ErrorPrefix = Entry.bIsError ? TEXT("⚠ ") : FString();
		if (Entry.Type == EUnrealMcpChatEntryType::Tool)
		{
			const FString Status = Entry.bIsPending ? TEXT("running") : (Entry.bIsError ? TEXT("error") : TEXT("done"));
			return FString::Printf(TEXT("%s%s Tool • %s (%s)"), *ErrorPrefix, *GetToolStatusIcon(Entry), *Entry.Title, *Status);
		}

		return ErrorPrefix + Entry.Speaker;
	}

	FSlateColor GetEntryTitleColor(const FUnrealMcpChatEntry& Entry)
	{
		return Entry.bIsError ? FSlateColor(FLinearColor(1.0f, 0.60f, 0.60f, 1.0f)) : FSlateColor::UseForeground();
	}

	bool IsScrollBoxNearBottom(const TSharedPtr<SScrollBox>& ScrollBox)
	{
		if (!ScrollBox.IsValid())
		{
			return true;
		}

		if (ScrollBox->GetViewOffsetFraction() > 0.95f)
		{
			return true;
		}

		const float ScrollOffset = ScrollBox->GetScrollOffset();
		const float EndOffset = ScrollBox->GetScrollOffsetOfEnd();
		return EndOffset <= 0.0f || (EndOffset - ScrollOffset) <= 50.0f;
	}

	TSharedRef<SWidget> MakeSelectableReadOnlyText(
		TAttribute<FText> Text,
		const FSlateFontInfo& Font,
		const FMargin& Margin = FMargin(0.0f))
	{
		return SNew(SMultiLineEditableText)
			.Text(MoveTemp(Text))
			.IsReadOnly(true)
			.AutoWrapText(true)
			.AllowContextMenu(true)
			.SelectWordOnMouseDoubleClick(true)
			.ClearTextSelectionOnFocusLoss(false)
			.Margin(Margin)
			.Font(Font);
	}

	FString JsonObjectToPrettyString(const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return TEXT("{}");
		}

		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return Output;
	}

	FString ClampForAssistantContext(const FString& Text, int32 MaxChars)
	{
		if (Text.Len() <= MaxChars)
		{
			return Text;
		}

		return Text.Left(MaxChars) + TEXT("\n...[truncated]");
	}

	bool ShouldSkipForAssistantContext(const FUnrealMcpChatEntry& Entry)
	{
		if (Entry.bIsPending)
		{
			return true;
		}

		const FString Body = Entry.Body.TrimStartAndEnd();
		if (Body.IsEmpty() && Entry.Details.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}

		if (Entry.Type == EUnrealMcpChatEntryType::System)
		{
			if (!Entry.bIsError)
			{
				return true;
			}

			if (Body.StartsWith(TEXT("Ready. Type plain text or /ask"), ESearchCase::CaseSensitive)
				|| Body.Equals(TEXT("AI is incorporating the tool results."), ESearchCase::CaseSensitive)
				|| Body.Equals(TEXT("AI conversation state reset."), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		return false;
	}

	FString BuildAssistantContextBlock(const FUnrealMcpChatEntry& Entry)
	{
		FString Prefix = TEXT("System");
		switch (Entry.Type)
		{
		case EUnrealMcpChatEntryType::User:
			Prefix = TEXT("User");
			break;
		case EUnrealMcpChatEntryType::Assistant:
			Prefix = TEXT("Assistant");
			break;
		case EUnrealMcpChatEntryType::Tool:
			Prefix = FString::Printf(
				TEXT("Tool %s [%s]"),
				*Entry.Title,
				Entry.bIsError ? TEXT("error") : TEXT("done"));
			break;
		case EUnrealMcpChatEntryType::System:
		default:
			break;
		}

		FString Block = Prefix + TEXT(":\n");
		if (Entry.Type == EUnrealMcpChatEntryType::Tool)
		{
			const FString Body = Entry.Body.TrimStartAndEnd();
			const FString Details = Entry.Details.TrimStartAndEnd();
			if (!Body.IsEmpty())
			{
				Block += TEXT("Result: ");
				Block += ClampForAssistantContext(Body, AssistantToolResultMaxChars);
			}
			if (!Details.IsEmpty())
			{
				if (!Body.IsEmpty())
				{
					Block += TEXT("\n");
				}
				Block += TEXT("Arguments: ");
				Block += ClampForAssistantContext(Details, AssistantToolArgumentsMaxChars);
			}
		}
		else
		{
			Block += Entry.Body.TrimStartAndEnd();
		}

		return ClampForAssistantContext(Block, AssistantHistoryMaxCharsPerEntry);
	}

	bool LoadProjectMemoryEntry(const FString& Key, TSharedPtr<FJsonObject>& OutEntry)
	{
		const FString MemoryPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp"), TEXT("ProjectMemory.json"));
		FString MemoryText;
		if (!FFileHelper::LoadFileToString(MemoryText, *MemoryPath))
		{
			return false;
		}

		TSharedPtr<FJsonObject> MemoryObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MemoryText);
		if (!FJsonSerializer::Deserialize(Reader, MemoryObject) || !MemoryObject.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
		if (!MemoryObject->TryGetArrayField(TEXT("entries"), EntriesArray) || !EntriesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
		{
			if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
			{
				continue;
			}

			TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
			FString EntryKey;
			if (EntryObject->TryGetStringField(TEXT("key"), EntryKey) && EntryKey.Equals(Key, ESearchCase::CaseSensitive))
			{
				OutEntry = EntryObject;
				return true;
			}
		}

		return false;
	}

	void AddOptionalMemoryLine(TArray<FString>& Lines, const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Label, int32 MaxChars)
	{
		if (!Object.IsValid())
		{
			return;
		}

		FString Value;
		if (Object->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("- %s: %s"), *Label, *ClampForAssistantContext(Value.TrimStartAndEnd(), MaxChars)));
		}
	}

	FString BuildActiveTaskMemoryContextBlock()
	{
		TSharedPtr<FJsonObject> EntryObject;
		if (!LoadProjectMemoryEntry(TEXT("chat.active_task"), EntryObject) || !EntryObject.IsValid())
		{
			return FString();
		}

		TArray<FString> Lines;
		Lines.Add(TEXT("Active task memory (chat.active_task):"));
		AddOptionalMemoryLine(Lines, EntryObject, TEXT("summary"), TEXT("summary"), 320);
		AddOptionalMemoryLine(Lines, EntryObject, TEXT("status"), TEXT("status"), 120);
		AddOptionalMemoryLine(Lines, EntryObject, TEXT("nextStep"), TEXT("next step"), 420);
		AddOptionalMemoryLine(Lines, EntryObject, TEXT("updatedAtUtc"), TEXT("updated at UTC"), 120);

		const TSharedPtr<FJsonObject>* ContentObject = nullptr;
		if (EntryObject->TryGetObjectField(TEXT("content"), ContentObject) && ContentObject && (*ContentObject).IsValid())
		{
			AddOptionalMemoryLine(Lines, *ContentObject, TEXT("trigger"), TEXT("trigger"), 160);
			AddOptionalMemoryLine(Lines, *ContentObject, TEXT("userPrompt"), TEXT("original user prompt"), 420);
			AddOptionalMemoryLine(Lines, *ContentObject, TEXT("assistantDraft"), TEXT("assistant draft"), 520);

			bool bHadToolError = false;
			if ((*ContentObject)->TryGetBoolField(TEXT("hadToolError"), bHadToolError))
			{
				Lines.Add(FString::Printf(TEXT("- had tool error: %s"), bHadToolError ? TEXT("true") : TEXT("false")));
			}

			double ToolRoundCount = 0.0;
			if ((*ContentObject)->TryGetNumberField(TEXT("toolRoundCount"), ToolRoundCount))
			{
				Lines.Add(FString::Printf(TEXT("- tool rounds used: %d"), static_cast<int32>(ToolRoundCount)));
			}

			const TArray<TSharedPtr<FJsonValue>>* RecentToolSummaries = nullptr;
			if ((*ContentObject)->TryGetArrayField(TEXT("recentToolSummaries"), RecentToolSummaries) && RecentToolSummaries)
			{
				TArray<FString> SummaryLines;
				const int32 StartIndex = FMath::Max(0, RecentToolSummaries->Num() - 6);
				for (int32 Index = StartIndex; Index < RecentToolSummaries->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& SummaryValue = (*RecentToolSummaries)[Index];
					if (SummaryValue.IsValid() && SummaryValue->Type == EJson::String)
					{
						SummaryLines.Add(FString::Printf(TEXT("  - %s"), *ClampForAssistantContext(SummaryValue->AsString(), 360)));
					}
				}

				if (SummaryLines.Num() > 0)
				{
					Lines.Add(TEXT("- recent tool summaries:"));
					Lines.Append(SummaryLines);
				}
			}
		}

		Lines.Add(TEXT("- resume rule: continue from the smallest verified next step; inspect before mutating; verify after tool use."));
		return ClampForAssistantContext(FString::Join(Lines, TEXT("\n")), AssistantActiveTaskMaxChars);
	}

	struct FToolOverviewItem
	{
		FString Name;
		FString Title;
		FString Description;
		FString Category;
		FString HandlerName;
		FString HandlerSourceFile;
		FString RiskLevel;
		FString TestCoverage;
		FString Exposure;
		bool bRequiresWrite = false;
		bool bRequiresBuild = false;
		bool bRequiresExternalProcess = false;
		bool bRequiresRestart = false;
		bool bDescriptorBacked = false;
		bool bLegacyHidden = false;
	};

	FString GetJsonStringField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Fallback = FString())
	{
		FString Value;
		if (Object.IsValid() && Object->TryGetStringField(FieldName, Value))
		{
			return Value;
		}
		return Fallback;
	}

	bool GetJsonBoolField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool bFallback = false)
	{
		bool bValue = bFallback;
		if (Object.IsValid() && Object->TryGetBoolField(FieldName, bValue))
		{
			return bValue;
		}
		return bFallback;
	}

	int32 GetJsonIntField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		double Value = 0.0;
		if (Object.IsValid() && Object->TryGetNumberField(FieldName, Value))
		{
			return static_cast<int32>(Value);
		}
		return 0;
	}

	FString OneLineToolText(FString Text, int32 MaxChars)
	{
		Text.ReplaceInline(TEXT("\r"), TEXT(" "));
		Text.ReplaceInline(TEXT("\n"), TEXT(" "));
		Text = Text.TrimStartAndEnd();
		while (Text.Contains(TEXT("  ")))
		{
			Text.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		if (Text.Len() > MaxChars)
		{
			Text = Text.Left(MaxChars - 3) + TEXT("...");
		}
		return Text;
	}

	FToolOverviewItem ToolOverviewItemFromVisibleTool(const TSharedPtr<FJsonObject>& ToolObject)
	{
		FToolOverviewItem Item;
		Item.Name = GetJsonStringField(ToolObject, TEXT("name"));
		Item.Title = GetJsonStringField(ToolObject, TEXT("title"));
		Item.Description = GetJsonStringField(ToolObject, TEXT("description"));
		Item.Category = GetJsonStringField(ToolObject, TEXT("registryCategory"));
		Item.HandlerName = GetJsonStringField(ToolObject, TEXT("handlerName"), Item.Name);
		Item.HandlerSourceFile = GetJsonStringField(ToolObject, TEXT("handlerSourceFile"));
		Item.Exposure = TEXT("visible");

		const TSharedPtr<FJsonObject>* PolicyObject = nullptr;
		if (ToolObject.IsValid() && ToolObject->TryGetObjectField(TEXT("policy"), PolicyObject) && PolicyObject && (*PolicyObject).IsValid())
		{
			Item.RiskLevel = GetJsonStringField(*PolicyObject, TEXT("riskLevel"), TEXT("unknown"));
			Item.TestCoverage = GetJsonStringField(*PolicyObject, TEXT("testCoverage"), TEXT("missing"));
			Item.bRequiresWrite = GetJsonBoolField(*PolicyObject, TEXT("requiresWrite"));
			Item.bRequiresBuild = GetJsonBoolField(*PolicyObject, TEXT("requiresBuild"));
			Item.bRequiresExternalProcess = GetJsonBoolField(*PolicyObject, TEXT("requiresExternalProcess"));
			Item.bRequiresRestart = GetJsonBoolField(*PolicyObject, TEXT("requiresRestart"));
			Item.bDescriptorBacked = GetJsonBoolField(*PolicyObject, TEXT("descriptorBacked"));
		}

		return Item;
	}

	FToolOverviewItem ToolOverviewItemFromRegistryEntry(const TSharedPtr<FJsonObject>& EntryObject)
	{
		FToolOverviewItem Item;
		Item.Name = GetJsonStringField(EntryObject, TEXT("name"));
		Item.Category = GetJsonStringField(EntryObject, TEXT("category"));
		Item.HandlerName = GetJsonStringField(EntryObject, TEXT("handlerName"), Item.Name);
		Item.Exposure = GetJsonStringField(EntryObject, TEXT("exposure"), TEXT("visible"));
		Item.bLegacyHidden = Item.Exposure.Equals(TEXT("legacy_hidden"), ESearchCase::IgnoreCase);
		Item.Description = GetJsonStringField(EntryObject, TEXT("notes"));
		Item.Title = Item.Name;

		const TSharedPtr<FJsonObject>* PolicyObject = nullptr;
		if (EntryObject.IsValid() && EntryObject->TryGetObjectField(TEXT("policy"), PolicyObject) && PolicyObject && (*PolicyObject).IsValid())
		{
			Item.RiskLevel = GetJsonStringField(*PolicyObject, TEXT("riskLevel"), TEXT("unknown"));
			Item.TestCoverage = GetJsonStringField(*PolicyObject, TEXT("testCoverage"), TEXT("missing"));
			Item.bRequiresWrite = GetJsonBoolField(*PolicyObject, TEXT("requiresWrite"));
			Item.bRequiresBuild = GetJsonBoolField(*PolicyObject, TEXT("requiresBuild"));
			Item.bRequiresExternalProcess = GetJsonBoolField(*PolicyObject, TEXT("requiresExternalProcess"));
			Item.bRequiresRestart = GetJsonBoolField(*PolicyObject, TEXT("requiresRestart"));
			Item.bDescriptorBacked = GetJsonBoolField(*PolicyObject, TEXT("descriptorBacked"));
		}

		return Item;
	}

	bool IsSelfExtensionTool(const FToolOverviewItem& Item)
	{
		return Item.Category.Equals(TEXT("self-extension"), ESearchCase::IgnoreCase)
			|| Item.Name.StartsWith(TEXT("unreal.mcp_"), ESearchCase::IgnoreCase)
			|| Item.Name.StartsWith(TEXT("unreal.knowledge_"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.tool_recommend"), ESearchCase::IgnoreCase)
			|| Item.Name.StartsWith(TEXT("unreal.workflow_"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.preview_change_plan"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.capture_project_snapshot"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.diff_project_snapshot"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.verify_task_outcome"), ESearchCase::IgnoreCase);
	}

	bool IsCliOrDynamicTool(const FToolOverviewItem& Item)
	{
		return Item.bRequiresExternalProcess
			|| Item.bRequiresBuild
			|| Item.Name.Contains(TEXT("execute_python"), ESearchCase::IgnoreCase)
			|| Item.Name.Contains(TEXT("execute_console_command"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.mcp_build_editor"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.mcp_supervisor_install"), ESearchCase::IgnoreCase)
			|| Item.Name.Equals(TEXT("unreal.mcp_extension_pipeline"), ESearchCase::IgnoreCase);
	}

	FString FormatToolOverviewLine(const FToolOverviewItem& Item)
	{
		TArray<FString> Tags;
		Tags.Add(Item.RiskLevel.IsEmpty() ? TEXT("unknown") : Item.RiskLevel);
		if (Item.bDescriptorBacked)
		{
			Tags.Add(TEXT("descriptor"));
		}
		if (Item.bRequiresWrite)
		{
			Tags.Add(TEXT("write"));
		}
		if (Item.bRequiresBuild)
		{
			Tags.Add(TEXT("build"));
		}
		if (Item.bRequiresExternalProcess)
		{
			Tags.Add(TEXT("external"));
		}
		if (Item.bRequiresRestart)
		{
			Tags.Add(TEXT("restart"));
		}
		if (!Item.TestCoverage.IsEmpty() && !Item.TestCoverage.Equals(TEXT("missing"), ESearchCase::IgnoreCase))
		{
			Tags.Add(Item.TestCoverage);
		}
		if (Item.bLegacyHidden)
		{
			Tags.Add(TEXT("legacy_hidden"));
		}

		const FString DisplayTitle = Item.Title.IsEmpty() || Item.Title.Equals(Item.Name, ESearchCase::CaseSensitive)
			? FString()
			: FString::Printf(TEXT(" - %s"), *OneLineToolText(Item.Title, 72));
		const FString Description = OneLineToolText(Item.Description, 110);
		const FString DescriptionSuffix = Description.IsEmpty() ? FString() : FString::Printf(TEXT(" | %s"), *Description);
		const FString HandlerSuffix = Item.HandlerName.IsEmpty() || Item.HandlerName.Equals(Item.Name, ESearchCase::CaseSensitive)
			? FString()
			: FString::Printf(TEXT(" -> %s"), *Item.HandlerName);

		return FString::Printf(
			TEXT("- %s%s [%s]%s%s"),
			*Item.Name,
			*HandlerSuffix,
			*FString::Join(Tags, TEXT(", ")),
			*DisplayTitle,
			*DescriptionSuffix);
	}

	void AppendToolGroup(TArray<FString>& Lines, const FString& Heading, TArray<FString>& GroupLines)
	{
		GroupLines.Sort();
		Lines.Add(FString::Printf(TEXT("\n%s (%d)"), *Heading, GroupLines.Num()));
		if (GroupLines.Num() == 0)
		{
			Lines.Add(TEXT("- none"));
			return;
		}
		Lines.Append(GroupLines);
	}
}

void SUnrealMcpChatPanel::Construct(const FArguments& InArgs, FUnrealMcpModule* InOwnerModule)
{
	OwnerModule = InOwnerModule;
	SkillApplyModes =
	{
		MakeShared<FString>(UnrealMcpChat::SkillApplyModeReadOnly()),
		MakeShared<FString>(UnrealMcpChat::SkillApplyModeApplyToMemory()),
		MakeShared<FString>(UnrealMcpChat::SkillApplyModeInsertPrompt()),
		MakeShared<FString>(UnrealMcpChat::SkillApplyModeAskNow())
	};
	SelectedSkillApplyMode = SkillApplyModes.IsValidIndex(1) ? SkillApplyModes[1] : nullptr;
	LoadRecentModelsFromDisk();
	RefreshProviderOptions();
	RefreshModelOptionsForActiveProvider();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "Unreal MCP Chat"))
				.Font(FAppStyle::GetFontStyle("HeadingMedium"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("Subtitle", "Use the panel as either a command surface or an AI copilot. Conversation stays in the main pane, while tool calls and logs stream into the separate Tool Log pane."))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 6.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 6.0f))
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("HelpPreset", "Help"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/help"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("StatusPreset", "Status"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/status"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ToolsPreset", "Tools"))
					.ToolTipText(LOCTEXT("ToolsPresetTooltip", "Show MCP tools grouped by self-extension, legacy, AI/CLI dynamic, and built-in tools."))
					.OnClicked(this, &SUnrealMcpChatPanel::HandleToolsOverviewClicked)
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("MapsPreset", "Maps"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/maps"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("AssetsPreset", "Selected Assets"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/selected assets"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ActorsPreset", "Selected Actors"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/selected actors"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("StartPiePreset", "Start PIE"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/pie"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("StopPiePreset", "Stop PIE"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/stop_pie"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("TailLogPreset", "Tail Log"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/log 80"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("MapCheckPreset", "Map Check"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/map_check"));
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("SavePreset", "Save"))
					.OnClicked_Lambda([this]()
					{
						HandlePresetClicked(TEXT("/save"));
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AiSkillBarTitle", "AI Settings / Project Skills"))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SHorizontalBox)
							.Visibility_Lambda([this]()
							{
								return ProviderOptionIds.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
							})
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(STextBlock)
								.Font(FAppStyle::GetFontStyle("SmallFont"))
								.Text(LOCTEXT("NoAiProvidersConfigured", "No AI providers configured. Open AI Settings to add one."))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(LOCTEXT("NoAiProvidersSettingsButton", "AI Settings"))
								.OnClicked(this, &SUnrealMcpChatPanel::HandleOpenAiSettingsClicked)
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(200.0f)
							.Visibility_Lambda([this]()
							{
								return ProviderOptionIds.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
							})
							[
								SAssignNew(ProviderComboBox, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&ProviderOptionIds)
								.InitiallySelectedItem(SelectedProviderId)
								.OnGenerateWidget_Lambda([this](TSharedPtr<FString> ProviderId)
								{
									FString Label = ProviderId.IsValid() ? *ProviderId : TEXT("<provider>");
									if (const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>())
									{
										if (ProviderId.IsValid())
										{
											if (const FAiProviderConfig* Provider = UnrealMcpChat::FindProviderById(*Settings, *ProviderId))
											{
												Label = UnrealMcpChat::FormatProviderLabel(*Provider);
											}
										}
									}
									return SNew(STextBlock).Text(FText::FromString(Label));
								})
								.OnSelectionChanged(this, &SUnrealMcpChatPanel::HandleProviderSelectionChanged)
								.ToolTipText_Lambda([this]()
								{
									return bAssistantRequestInFlight
										? LOCTEXT("ProviderInFlightTooltip", "Changes apply to the next request - current run continues.")
										: LOCTEXT("ProviderSelectorTooltip", "Select the AI provider used for the next request.");
								})
								[
									SNew(STextBlock)
									.Text_Lambda([this]()
									{
										return FText::FromString(TEXT("Provider: ") + GetCurrentProviderDisplayText().ToString());
									})
								]
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(200.0f)
							.Visibility_Lambda([this]()
							{
								return ProviderOptionIds.Num() > 0 && !IsActiveProviderModelLocked() ? EVisibility::Visible : EVisibility::Collapsed;
							})
							[
								SAssignNew(ModelComboBox, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&ModelOptions)
								.InitiallySelectedItem(SelectedModel)
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> Model)
								{
									return SNew(STextBlock).Text(FText::FromString(Model.IsValid() ? *Model : TEXT("<model>")));
								})
								.OnSelectionChanged(this, &SUnrealMcpChatPanel::HandleModelSelectionChanged)
								.ToolTipText_Lambda([this]()
								{
									return bAssistantRequestInFlight
										? LOCTEXT("ModelInFlightTooltip", "Changes apply to the next request - current run continues.")
										: LOCTEXT("ModelSelectorTooltip", "Edit the active provider model, or pick a recent model.");
								})
								[
									SAssignNew(ModelEditableTextBox, SEditableTextBox)
									.Text(GetCurrentModelDisplayText())
									.HintText(LOCTEXT("ModelSelectorLabel", "Model"))
									.SelectAllTextWhenFocused(true)
									.OnTextCommitted(this, &SUnrealMcpChatPanel::HandleModelTextCommitted)
									.ToolTipText_Lambda([this]()
									{
										return bAssistantRequestInFlight
											? LOCTEXT("ModelTextInFlightTooltip", "Changes apply to the next request - current run continues.")
											: LOCTEXT("ModelTextTooltip", "Type a model id and press Enter, or leave the field to save it.");
									})
								]
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.MinDesiredWidth(200.0f)
							.Visibility_Lambda([this]()
							{
								return ProviderOptionIds.Num() > 0 && IsActiveProviderModelLocked() ? EVisibility::Visible : EVisibility::Collapsed;
							})
							.ToolTipText(LOCTEXT("CodexLockedModelTooltip", "Codex variants are hard-locked to gpt-5.5 with xhigh reasoning per project policy."))
							[
								SNew(STextBlock)
								.Text(this, &SUnrealMcpChatPanel::GetCurrentModelDisplayText)
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("AiSettingsButton", "AI Settings"))
							.ToolTipText(LOCTEXT("AiSettingsTooltip", "Open Project Settings > Plugins > Unreal MCP, where the OpenAI API key and model are configured."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleOpenAiSettingsClicked)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("TestAiConnectionButton", "Test AI"))
							.ToolTipText(LOCTEXT("TestAiConnectionTooltip", "Send a minimal Responses API request using the configured endpoint, model, and API key."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleTestAiConnectionClicked)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("RefreshSkillsButton", "Refresh Skills"))
							.ToolTipText(LOCTEXT("RefreshSkillsTooltip", "Call unreal.skill_list and refresh the skill selector."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleRefreshSkillsClicked)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(0.30f)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SAssignNew(SkillComboBox, SComboBox<TSharedPtr<FUnrealMcpSkillOption>>)
							.OptionsSource(&SkillOptions)
							.OnGenerateWidget(this, &SUnrealMcpChatPanel::MakeSkillComboOption)
							.OnSelectionChanged(this, &SUnrealMcpChatPanel::HandleSkillSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SUnrealMcpChatPanel::GetSelectedSkillText)
							]
						]
						+ SHorizontalBox::Slot()
						.FillWidth(0.34f)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SAssignNew(SkillTaskTextBox, SEditableTextBox)
							.HintText(LOCTEXT("SkillTaskHint", "Task for Apply Skill, e.g. extend MCP safely"))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(0.24f)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SAssignNew(SkillApplyModeComboBox, SComboBox<TSharedPtr<FString>>)
							.OptionsSource(&SkillApplyModes)
							.OnGenerateWidget(this, &SUnrealMcpChatPanel::MakeSkillApplyModeComboOption)
							.OnSelectionChanged(this, &SUnrealMcpChatPanel::HandleSkillApplyModeChanged)
							[
								SNew(STextBlock)
								.Text(this, &SUnrealMcpChatPanel::GetSelectedSkillApplyModeText)
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("ReadSkillButton", "Read Skill"))
							.ToolTipText(LOCTEXT("ReadSkillTooltip", "Call unreal.skill_read for the selected skill."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleReadSelectedSkillClicked)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("ApplySkillButton", "Apply Skill"))
							.ToolTipText(LOCTEXT("ApplySkillTooltip", "Apply the selected skill using the chosen mode: read only, memory write, insert prompt, or ask now."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleApplySelectedSkillClicked)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(6.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("ReadMemoryButton", "Read Memory"))
							.ToolTipText(LOCTEXT("ReadMemoryTooltip", "Call unreal.project_memory_view for recent project memory entries."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleReadMemoryClicked)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("WriteTaskMemoryButton", "Write Task Memory"))
							.ToolTipText(LOCTEXT("WriteTaskMemoryTooltip", "Save the current task text and selected skill as a project memory entry."))
							.OnClicked(this, &SUnrealMcpChatPanel::HandleWriteCurrentTaskMemoryClicked)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SAssignNew(SkillDescriptionText, STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.72f, 0.75f, 0.80f, 1.0f))
						.Text(this, &SUnrealMcpChatPanel::GetSelectedSkillDescriptionText)
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				+ SSplitter::Slot()
				.Value(0.68f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(8.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 6.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ConversationPaneTitle", "Conversation"))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SAssignNew(TranscriptScrollBox, SScrollBox)
							+ SScrollBox::Slot()
							[
								SAssignNew(TranscriptEntriesBox, SVerticalBox)
							]
						]
					]
				]
				+ SSplitter::Slot()
				.Value(0.32f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(8.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 6.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ToolLogPaneTitle", "Tool Log"))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SAssignNew(ToolLogScrollBox, SScrollBox)
							+ SScrollBox::Slot()
							[
								SAssignNew(ToolLogEntriesBox, SVerticalBox)
							]
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SAssignNew(InputTextBox, SEditableTextBox)
					.HintText(LOCTEXT("InputHint", "Try build the current map, /ask inspect the TwinStick map, /steer keep it simple, /stop_ai, or /tool unreal.spawn_actor {...}"))
					.OnTextCommitted(this, &SUnrealMcpChatPanel::HandleInputCommitted)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Send", "Send"))
					.OnClicked(this, &SUnrealMcpChatPanel::HandleSendClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this]()
					{
						return bAssistantRequestInFlight ? EVisibility::Visible : EVisibility::Collapsed;
					})
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SThrobber)
						.Visibility_Lambda([this]()
						{
							return bAssistantRequestInFlight ? EVisibility::Visible : EVisibility::Collapsed;
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.Text(this, &SUnrealMcpChatPanel::GetActiveRequestProgressText)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Stop", "Stop"))
					.IsEnabled_Lambda([this]()
					{
						return bAssistantRequestInFlight;
					})
					.OnClicked(this, &SUnrealMcpChatPanel::HandleStopClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyChat", "Copy Chat"))
					.ToolTipText(LOCTEXT("CopyChatTooltip", "Copy only the visible conversation pane: user, assistant, and system messages."))
					.IsEnabled_Lambda([this]()
					{
						return HasTranscriptEntries();
					})
					.OnClicked(this, &SUnrealMcpChatPanel::HandleCopyChatClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyToolLog", "Copy Tool Log"))
					.ToolTipText(LOCTEXT("CopyToolLogTooltip", "Copy the visible Tool Log pane, including tool names, status, text, and details."))
					.IsEnabled_Lambda([this]()
					{
						return HasToolLogEntries();
					})
					.OnClicked(this, &SUnrealMcpChatPanel::HandleCopyToolLogClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Clear", "Clear"))
					.OnClicked(this, &SUnrealMcpChatPanel::HandleClearClicked)
				]
			]
		]
	];

	LoadHistory();
	if (Entries.Num() == 0)
	{
		ResetHistory(true);
	}
	RefreshSkillOptions(false);
}

FReply SUnrealMcpChatPanel::HandleSendClicked()
{
	SendCurrentInput();
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleStopClicked()
{
	StopAssistantRequest();
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleClearClicked()
{
	StopAssistantRequest();
	ResetHistory(true);
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleCopyChatClicked()
{
	const FString TranscriptText = BuildTranscriptText();
	if (!TranscriptText.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*TranscriptText);
	}
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleCopyToolLogClicked()
{
	const FString ToolLogText = BuildToolLogText();
	if (!ToolLogText.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*ToolLogText);
	}
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleEntryCopyClicked(TSharedPtr<FUnrealMcpChatEntry> Entry)
{
	if (Entry.IsValid())
	{
		const FString EntryText = BuildEntryCopyText(*Entry);
		if (!EntryText.IsEmpty())
		{
			FPlatformApplicationMisc::ClipboardCopy(*EntryText);
		}
	}

	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleToolsOverviewClicked()
{
	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("The chat panel is not connected to the module."), true);
		return FReply::Handled();
	}

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.mcp_workbench_status"), *Arguments);
	if (Result.bIsError || !Result.StructuredContent.IsValid())
	{
		AppendToolExecutionResult(TEXT("unreal.mcp_workbench_status"), *Arguments, Result);
		return FReply::Handled();
	}

	AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Tools"), BuildToolsOverviewText(Result));
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleOpenAiSettingsClicked()
{
	ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>(TEXT("Settings"));
	if (!SettingsModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("Unable to load the Settings module."), true);
		return FReply::Handled();
	}

	SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("UnrealMcp"));
	AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("Opened Project Settings > Plugins > Unreal MCP. Configure the OpenAI API key, model, and endpoint there."));
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleTestAiConnectionClicked()
{
	StartAiConnectionTest();
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleRefreshSkillsClicked()
{
	RefreshSkillOptions(true);
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleReadSelectedSkillClicked()
{
	const FString SkillName = GetSelectedSkillName();
	if (SkillName.IsEmpty())
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("Select a project skill before reading it."), true);
		return FReply::Handled();
	}

	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("The chat panel is not connected to the module."), true);
		return FReply::Handled();
	}

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("skillName"), SkillName);
	Arguments->SetBoolField(TEXT("includeText"), true);
	Arguments->SetNumberField(TEXT("maxPreviewChars"), 8000.0);
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.skill_read"), *Arguments);
	AppendToolExecutionResult(TEXT("unreal.skill_read"), *Arguments, Result);
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleApplySelectedSkillClicked()
{
	const FString SkillName = GetSelectedSkillName();
	if (SkillName.IsEmpty())
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("Select a project skill before applying it."), true);
		return FReply::Handled();
	}

	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("The chat panel is not connected to the module."), true);
		return FReply::Handled();
	}

	const FString Task = GetSkillTaskOrFallback();
	const FString ApplyMode = GetSelectedSkillApplyMode();

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("skillName"), SkillName);
	Arguments->SetStringField(TEXT("task"), Task);

	if (ApplyMode.Equals(UnrealMcpChat::SkillApplyModeReadOnly(), ESearchCase::CaseSensitive))
	{
		Arguments->SetBoolField(TEXT("includeText"), true);
		Arguments->SetNumberField(TEXT("maxPreviewChars"), 12000.0);
		const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.skill_read"), *Arguments);
		AppendToolExecutionResult(TEXT("unreal.skill_read"), *Arguments, Result);
		return FReply::Handled();
	}

	const bool bWriteMemory = ApplyMode.Equals(UnrealMcpChat::SkillApplyModeApplyToMemory(), ESearchCase::CaseSensitive)
		|| ApplyMode.Equals(UnrealMcpChat::SkillApplyModeAskNow(), ESearchCase::CaseSensitive);
	const bool bNeedsPrompt = ApplyMode.Equals(UnrealMcpChat::SkillApplyModeInsertPrompt(), ESearchCase::CaseSensitive)
		|| ApplyMode.Equals(UnrealMcpChat::SkillApplyModeAskNow(), ESearchCase::CaseSensitive);

	Arguments->SetBoolField(TEXT("writeMemory"), bWriteMemory);
	Arguments->SetBoolField(TEXT("includeFullText"), bNeedsPrompt);
	Arguments->SetStringField(TEXT("chatApplyMode"), ApplyMode);
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.skill_apply"), *Arguments);
	AppendToolExecutionResult(TEXT("unreal.skill_apply"), *Arguments, Result);

	if (Result.bIsError || !bNeedsPrompt)
	{
		return FReply::Handled();
	}

	const FString AskPrompt = BuildSkillAskPrompt(SkillName, Task);
	if (ApplyMode.Equals(UnrealMcpChat::SkillApplyModeAskNow(), ESearchCase::CaseSensitive))
	{
		SendCommand(AskPrompt);
		return FReply::Handled();
	}

	if (InputTextBox.IsValid())
	{
		InputTextBox->SetText(FText::FromString(AskPrompt));
	}
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleReadMemoryClicked()
{
	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("The chat panel is not connected to the module."), true);
		return FReply::Handled();
	}

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetBoolField(TEXT("includeContent"), false);
	Arguments->SetNumberField(TEXT("maxEntries"), 10.0);
	Arguments->SetBoolField(TEXT("sortDescending"), true);
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.project_memory_view"), *Arguments);
	AppendToolExecutionResult(TEXT("unreal.project_memory_view"), *Arguments, Result);
	return FReply::Handled();
}

FReply SUnrealMcpChatPanel::HandleWriteCurrentTaskMemoryClicked()
{
	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("The chat panel is not connected to the module."), true);
		return FReply::Handled();
	}

	FString Task = SkillTaskTextBox.IsValid() ? SkillTaskTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (Task.IsEmpty() && InputTextBox.IsValid())
	{
		Task = InputTextBox->GetText().ToString().TrimStartAndEnd();
	}
	if (Task.IsEmpty())
	{
		Task = TEXT("Continue the current Unreal MCP chat task.");
	}

	TSharedPtr<FJsonObject> ContentObject = MakeShared<FJsonObject>();
	ContentObject->SetStringField(TEXT("task"), Task);
	ContentObject->SetStringField(TEXT("selectedSkill"), GetSelectedSkillName());
	ContentObject->SetStringField(TEXT("skillApplyMode"), GetSelectedSkillApplyMode());
	ContentObject->SetStringField(TEXT("capturedAtUtc"), FDateTime::UtcNow().ToIso8601());

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("key"), TEXT("chat.current_task"));
	Arguments->SetStringField(TEXT("summary"), TEXT("Current Chat toolbar task."));
	Arguments->SetStringField(TEXT("status"), TEXT("in_progress"));
	Arguments->SetStringField(TEXT("nextStep"), TEXT("Resume from the saved Chat toolbar task."));
	Arguments->SetStringField(TEXT("contentJson"), UnrealMcpChat::JsonObjectToPrettyString(ContentObject));

	TArray<TSharedPtr<FJsonValue>> Tags;
	Tags.Add(MakeShared<FJsonValueString>(TEXT("chat")));
	Tags.Add(MakeShared<FJsonValueString>(TEXT("toolbar")));
	if (!GetSelectedSkillName().IsEmpty())
	{
		Tags.Add(MakeShared<FJsonValueString>(GetSelectedSkillName()));
	}
	Arguments->SetArrayField(TEXT("tags"), Tags);

	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.project_memory_write"), *Arguments);
	AppendToolExecutionResult(TEXT("unreal.project_memory_write"), *Arguments, Result);
	return FReply::Handled();
}

void SUnrealMcpChatPanel::HandleInputCommitted(const FText& InText, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter)
	{
		SendCurrentInput();
	}
}

void SUnrealMcpChatPanel::HandlePresetClicked(FString CommandText)
{
	SendCommand(CommandText);
}

void SUnrealMcpChatPanel::HandleProviderSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid()) { return; }
	SelectedProviderId = NewSelection;
	if (SelectInfo != ESelectInfo::Direct) { SetActiveProviderById(*NewSelection); }
}

void SUnrealMcpChatPanel::HandleModelSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid() || IsActiveProviderModelLocked()) { return; }
	SelectedModel = NewSelection;
	if (ModelEditableTextBox.IsValid()) { ModelEditableTextBox->SetText(FText::FromString(*NewSelection)); }
	if (SelectInfo != ESelectInfo::Direct) { HandleModelTextCommitted(FText::FromString(*NewSelection), ETextCommit::OnEnter); }
}

void SUnrealMcpChatPanel::HandleModelTextCommitted(const FText& InText, ETextCommit::Type CommitType)
{
	if (IsActiveProviderModelLocked() || (CommitType != ETextCommit::OnEnter && CommitType != ETextCommit::OnUserMovedFocus)) { return; }
	const FString Model = InText.ToString().TrimStartAndEnd();
	if (Model.IsEmpty()) { return; }
	UUnrealMcpSettings* Settings = GetMutableDefault<UUnrealMcpSettings>();
	if (!Settings) { return; }
	FAiProviderConfig* Provider = Settings->FindActiveProvider() ? UnrealMcpChat::FindMutableProviderById(*Settings, Settings->ActiveProviderId) : nullptr;
	if (!Provider) { return; }
	const FString ProviderId = Provider->Id;
	if (!Provider->Model.Equals(Model, ESearchCase::CaseSensitive)) { Provider->Model = Model; Settings->SaveConfig(); }
	RememberRecentModel(ProviderId, Model);
	RefreshModelOptionsForActiveProvider();
}

void SUnrealMcpChatPanel::RefreshProviderOptions()
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	ProviderOptionIds.Reset();
	SelectedProviderId.Reset();
	if (!Settings) { return; }

	TArray<const FAiProviderConfig*> SortedProviders;
	for (const FAiProviderConfig& Provider : Settings->Providers) { SortedProviders.Add(&Provider); }

	SortedProviders.Sort([](const FAiProviderConfig& A, const FAiProviderConfig& B)
	{
		const FString ADisplay = A.DisplayName.TrimStartAndEnd().IsEmpty() ? A.Id : A.DisplayName.TrimStartAndEnd();
		const FString BDisplay = B.DisplayName.TrimStartAndEnd().IsEmpty() ? B.Id : B.DisplayName.TrimStartAndEnd();
		const int32 DisplayCompare = ADisplay.Compare(BDisplay, ESearchCase::IgnoreCase);
		if (DisplayCompare != 0) { return DisplayCompare < 0; }
		const FString AId = A.Id;
		const FString BId = B.Id;
		return AId.Compare(BId, ESearchCase::IgnoreCase) < 0;
	});

	for (const FAiProviderConfig* Provider : SortedProviders)
	{
		if (!Provider || Provider->Id.TrimStartAndEnd().IsEmpty()) { continue; }
		TSharedPtr<FString> OptionId = MakeShared<FString>(Provider->Id);
		ProviderOptionIds.Add(OptionId);
		if (Provider->Id.Equals(Settings->ActiveProviderId, ESearchCase::CaseSensitive)) { SelectedProviderId = OptionId; }
	}

	if (!SelectedProviderId.IsValid() && ProviderOptionIds.Num() > 0) { SelectedProviderId = ProviderOptionIds[0]; }
	if (ProviderComboBox.IsValid())
	{ ProviderComboBox->RefreshOptions(); ProviderComboBox->SetSelectedItem(SelectedProviderId); }
}

void SUnrealMcpChatPanel::RefreshModelOptionsForActiveProvider()
{
	ModelOptions.Reset();
	SelectedModel.Reset();

	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	const FAiProviderConfig* Provider = Settings ? Settings->FindActiveProvider() : nullptr;
	if (!Provider)
	{
		if (ModelComboBox.IsValid()) { ModelComboBox->RefreshOptions(); ModelComboBox->ClearSelection(); }
		if (ModelEditableTextBox.IsValid()) { ModelEditableTextBox->SetText(FText::GetEmpty()); }
		return;
	}

	const FString CurrentModel = IsActiveProviderModelLocked() ? TEXT("gpt-5.5") : Provider->Model.TrimStartAndEnd();
	TSet<FString> SeenModels;
	if (!CurrentModel.IsEmpty()) { SelectedModel = MakeShared<FString>(CurrentModel); ModelOptions.Add(SelectedModel); SeenModels.Add(CurrentModel); }

	const TArray<FString>* RecentModels = RecentModelsByProvider.Find(Provider->Id);
	int32 RecentAdded = 0;
	if (RecentModels)
	{
		for (const FString& RecentModel : *RecentModels)
		{
			const FString TrimmedRecentModel = RecentModel.TrimStartAndEnd();
			if (TrimmedRecentModel.IsEmpty() || SeenModels.Contains(TrimmedRecentModel)) { continue; }
			ModelOptions.Add(MakeShared<FString>(TrimmedRecentModel));
			SeenModels.Add(TrimmedRecentModel);
			if (++RecentAdded >= 3) { break; }
		}
	}

	if (!SelectedModel.IsValid()) { SelectedModel = MakeShared<FString>(FString()); }
	if (ModelComboBox.IsValid())
	{ ModelComboBox->RefreshOptions(); ModelComboBox->SetSelectedItem(ModelOptions.Num() > 0 ? SelectedModel : TSharedPtr<FString>()); }
	if (ModelEditableTextBox.IsValid()) { ModelEditableTextBox->SetText(GetCurrentModelDisplayText()); }
}

void SUnrealMcpChatPanel::LoadRecentModelsFromDisk()
{
	RecentModelsByProvider.Reset();

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *UnrealMcpChat::GetChatPanelStateFilePath())) { return; }

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{ UE_LOG(LogTemp, Verbose, TEXT("[UnrealMcp] Ignoring corrupt chat panel state file.")); return; }

	const TSharedPtr<FJsonObject>* RecentModelsObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("recentModels"), RecentModelsObject) || !RecentModelsObject || !(*RecentModelsObject).IsValid()) { return; }

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*RecentModelsObject)->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Array) { continue; }

		TArray<FString> Models;
		for (const TSharedPtr<FJsonValue>& ModelValue : Pair.Value->AsArray())
		{
			if (!ModelValue.IsValid() || ModelValue->Type != EJson::String) { continue; }
			const FString Model = ModelValue->AsString().TrimStartAndEnd();
			if (!Model.IsEmpty() && !Models.Contains(Model)) { Models.Add(Model); }
			if (Models.Num() >= 3) { break; }
		}
		if (Models.Num() > 0) { RecentModelsByProvider.Add(Pair.Key, Models); }
	}
}

void SUnrealMcpChatPanel::SaveRecentModelsToDisk() const
{
	const FString StatePath = UnrealMcpChat::GetChatPanelStateFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(StatePath), true);

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> RecentModelsObject = MakeShared<FJsonObject>();
	for (const TPair<FString, TArray<FString>>& Pair : RecentModelsByProvider)
	{
		TArray<TSharedPtr<FJsonValue>> ModelValues;
		for (const FString& Model : Pair.Value)
		{
			const FString TrimmedModel = Model.TrimStartAndEnd();
			if (!TrimmedModel.IsEmpty()) { ModelValues.Add(MakeShared<FJsonValueString>(TrimmedModel)); }
		}
		RecentModelsObject->SetArrayField(Pair.Key, ModelValues);
	}
	RootObject->SetObjectField(TEXT("recentModels"), RecentModelsObject);

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);
	if (!FFileHelper::SaveStringToFile(JsonText, *StatePath))
	{ UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to save chat panel state file: %s"), *StatePath); }
}

void SUnrealMcpChatPanel::SetActiveProviderById(const FString& NewId)
{
	UUnrealMcpSettings* Settings = GetMutableDefault<UUnrealMcpSettings>();
	if (!Settings || NewId.TrimStartAndEnd().IsEmpty()) { return; }

	FAiProviderConfig* Provider = UnrealMcpChat::FindMutableProviderById(*Settings, NewId);
	if (!Provider) { return; }

	Settings->ActiveProviderId = NewId;
	Settings->SaveConfig();
	SelectedProviderId = MakeShared<FString>(NewId);
	RefreshProviderOptions();
	RefreshModelOptionsForActiveProvider();

	const FString DisplayName = Provider->DisplayName.TrimStartAndEnd().IsEmpty() ? Provider->Id : Provider->DisplayName.TrimStartAndEnd();
	AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), FString::Printf(TEXT("Provider switched to %s. The change applies to the next request."), *DisplayName));
}

void SUnrealMcpChatPanel::RememberRecentModel(const FString& ProviderId, const FString& Model)
{
	const FString TrimmedProviderId = ProviderId.TrimStartAndEnd();
	const FString TrimmedModel = Model.TrimStartAndEnd();
	if (TrimmedProviderId.IsEmpty() || TrimmedModel.IsEmpty()) { return; }

	TArray<FString>& Models = RecentModelsByProvider.FindOrAdd(TrimmedProviderId);
	const TArray<FString> PreviousModels = Models;
	Models.RemoveAll([&TrimmedModel](const FString& Existing)
	{
		return Existing.Equals(TrimmedModel, ESearchCase::CaseSensitive);
	});
	Models.Insert(TrimmedModel, 0);
	while (Models.Num() > 3) { Models.RemoveAt(Models.Num() - 1); }
	if (Models != PreviousModels) { SaveRecentModelsToDisk(); }
}

FText SUnrealMcpChatPanel::GetCurrentProviderDisplayText() const
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (!Settings || !SelectedProviderId.IsValid()) { return FText::FromString(TEXT("No provider")); }

	const FAiProviderConfig* Provider = UnrealMcpChat::FindProviderById(*Settings, *SelectedProviderId);
	return FText::FromString(Provider ? UnrealMcpChat::FormatProviderLabel(*Provider) : *SelectedProviderId);
}

FText SUnrealMcpChatPanel::GetCurrentModelDisplayText() const
{
	if (IsActiveProviderModelLocked()) { return FText::FromString(TEXT("gpt-5.5 (locked)")); }
	return FText::FromString(SelectedModel.IsValid() ? *SelectedModel : FString());
}

bool SUnrealMcpChatPanel::IsActiveProviderModelLocked() const
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (!Settings) { return false; }
	const FString ProviderId = SelectedProviderId.IsValid() ? *SelectedProviderId : Settings->ActiveProviderId;
	const FAiProviderConfig* Provider = UnrealMcpChat::FindProviderById(*Settings, ProviderId);
	return Provider && (Provider->Kind == EAiProviderKind::Codex || Provider->Kind == EAiProviderKind::CodexAppServer);
}

FString SUnrealMcpChatPanel::KindShortName(EAiProviderKind Kind)
{
	return UnrealMcpChat::ProviderKindShortName(Kind);
}

void SUnrealMcpChatPanel::HandleSkillSelectionChanged(TSharedPtr<FUnrealMcpSkillOption> NewSelection, ESelectInfo::Type SelectInfo)
{
	SelectedSkill = NewSelection;
}

void SUnrealMcpChatPanel::HandleSkillApplyModeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	SelectedSkillApplyMode = NewSelection;
}

void SUnrealMcpChatPanel::StartAiConnectionTest()
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (!Settings)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("Unable to load Unreal MCP settings."), true);
		return;
	}

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("url"), Settings->OpenAIResponsesUrl);
	Arguments->SetStringField(TEXT("model"), Settings->OpenAIModel);
	Arguments->SetBoolField(TEXT("apiKeyConfigured"), !Settings->OpenAIApiKey.TrimStartAndEnd().IsEmpty());
	Arguments->SetBoolField(TEXT("aiAssistantEnabled"), Settings->bEnableAiAssistant);
	Arguments->SetNumberField(TEXT("timeoutSeconds"), Settings->AiRequestTimeoutSeconds);
	const FString ArgumentsJson = UnrealMcpChat::JsonObjectToPrettyString(Arguments);
	TSharedPtr<FUnrealMcpChatEntry> Entry = AppendToolCard(TEXT("unreal.ai_test_connection"), FString(), ArgumentsJson);

	auto FinishAiTest = [this, Entry, ArgumentsJson](const FString& Message, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError)
	{
		FUnrealMcpExecutionResult Result;
		Result.Text = Message;
		Result.StructuredContent = StructuredContent;
		Result.bIsError = bIsError;
		UpdateToolEntryWithResult(Entry, ArgumentsJson, Result);
	};

	const FString ApiKey = Settings->OpenAIApiKey.TrimStartAndEnd();
	const FString Model = Settings->OpenAIModel.TrimStartAndEnd();
	const FString Url = Settings->OpenAIResponsesUrl.TrimStartAndEnd();

	if (ApiKey.IsEmpty() || Model.IsEmpty() || Url.IsEmpty())
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("ai_test_connection"));
		StructuredContent->SetStringField(TEXT("url"), Url);
		StructuredContent->SetStringField(TEXT("model"), Model);
		StructuredContent->SetBoolField(TEXT("apiKeyConfigured"), !ApiKey.IsEmpty());
		StructuredContent->SetBoolField(TEXT("requestSent"), false);
		FinishAiTest(TEXT("AI connection test was not sent. Configure OpenAI API key, model, and Responses URL first."), StructuredContent, true);
		return;
	}

	TSharedPtr<FJsonObject> TextObject = MakeShared<FJsonObject>();
	TextObject->SetStringField(TEXT("type"), TEXT("input_text"));
	TextObject->SetStringField(TEXT("text"), TEXT("Return exactly: OK"));
	TArray<TSharedPtr<FJsonValue>> ContentArray;
	ContentArray.Add(MakeShared<FJsonValueObject>(TextObject));

	TSharedPtr<FJsonObject> MessageObject = MakeShared<FJsonObject>();
	MessageObject->SetStringField(TEXT("role"), TEXT("user"));
	MessageObject->SetArrayField(TEXT("content"), ContentArray);
	TArray<TSharedPtr<FJsonValue>> InputArray;
	InputArray.Add(MakeShared<FJsonValueObject>(MessageObject));

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("model"), Model);
	Payload->SetArrayField(TEXT("input"), InputArray);
	Payload->SetNumberField(TEXT("max_output_tokens"), 16.0);
	Payload->SetBoolField(TEXT("stream"), false);
	Payload->SetStringField(TEXT("truncation"), TEXT("auto"));

	FString PayloadString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
	FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	Request->SetTimeout(Settings->AiRequestTimeoutSeconds);
	Request->SetActivityTimeout(Settings->AiRequestActivityTimeoutSeconds);
	Request->SetContentAsString(PayloadString);

	TWeakPtr<SUnrealMcpChatPanel> WeakPanel = SharedThis(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakPanel, Entry, ArgumentsJson, Url, Model](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
		{
			if (const TSharedPtr<SUnrealMcpChatPanel> PinnedThis = WeakPanel.Pin())
			{
				const int32 ResponseCode = HttpResponse.IsValid() ? HttpResponse->GetResponseCode() : 0;
				const FString ResponseText = HttpResponse.IsValid() ? HttpResponse->GetContentAsString() : FString();
				const bool bHttpOk = bSucceeded && ResponseCode >= 200 && ResponseCode < 300;

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("ai_test_connection"));
				StructuredContent->SetStringField(TEXT("url"), Url);
				StructuredContent->SetStringField(TEXT("model"), Model);
				StructuredContent->SetBoolField(TEXT("requestSucceeded"), bSucceeded);
				StructuredContent->SetNumberField(TEXT("httpStatus"), ResponseCode);
				StructuredContent->SetStringField(TEXT("responsePreview"), ResponseText.Left(UnrealMcpChat::AiTestResponsePreviewMaxChars));

				FUnrealMcpExecutionResult Result;
				Result.StructuredContent = StructuredContent;
				Result.bIsError = !bHttpOk;
				Result.Text = bHttpOk
					? FString::Printf(TEXT("AI connection test succeeded. HTTP %d using model '%s'."), ResponseCode, *Model)
					: FString::Printf(TEXT("AI connection test failed. HTTP %d. %s"), ResponseCode, *ResponseText.Left(700));
				PinnedThis->UpdateToolEntryWithResult(Entry, ArgumentsJson, Result);
				PinnedThis->ActiveAiTestRequest.Reset();
			}
		});

	ActiveAiTestRequest = Request;
	if (!Request->ProcessRequest())
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("ai_test_connection"));
		StructuredContent->SetStringField(TEXT("url"), Url);
		StructuredContent->SetStringField(TEXT("model"), Model);
		StructuredContent->SetBoolField(TEXT("requestSent"), false);
		FinishAiTest(TEXT("AI connection test failed before the HTTP request could be started."), StructuredContent, true);
		ActiveAiTestRequest.Reset();
	}
}

void SUnrealMcpChatPanel::RefreshSkillOptions(bool bAppendResult)
{
	if (!OwnerModule)
	{
		if (bAppendResult)
		{
			AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("The chat panel is not connected to the module."), true);
		}
		return;
	}

	const FString PreviousSelection = GetSelectedSkillName();
	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetBoolField(TEXT("includeText"), false);
	Arguments->SetNumberField(TEXT("maxPreviewChars"), 1000.0);
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.skill_list"), *Arguments);

	SkillOptions.Reset();
	TSharedPtr<FUnrealMcpSkillOption> PreservedSelection;
	if (!Result.bIsError && Result.StructuredContent.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Skills = nullptr;
		if (Result.StructuredContent->TryGetArrayField(TEXT("skills"), Skills) && Skills)
		{
			for (const TSharedPtr<FJsonValue>& SkillValue : *Skills)
			{
				const TSharedPtr<FJsonObject> SkillObject = SkillValue.IsValid() ? SkillValue->AsObject() : nullptr;
				FString SkillName;
				if (SkillObject.IsValid() && SkillObject->TryGetStringField(TEXT("name"), SkillName) && !SkillName.IsEmpty())
				{
					TSharedPtr<FUnrealMcpSkillOption> Option = MakeShared<FUnrealMcpSkillOption>();
					Option->Name = SkillName;
					SkillObject->TryGetStringField(TEXT("title"), Option->Title);
					SkillObject->TryGetStringField(TEXT("description"), Option->Description);
					SkillObject->TryGetStringField(TEXT("path"), Option->Path);
					SkillOptions.Add(Option);
					if (!PreviousSelection.IsEmpty() && SkillName.Equals(PreviousSelection, ESearchCase::CaseSensitive))
					{
						PreservedSelection = Option;
					}
				}
			}
		}
	}

	SelectedSkill = PreservedSelection.IsValid() ? PreservedSelection : (SkillOptions.Num() > 0 ? SkillOptions[0] : nullptr);
	if (SkillComboBox.IsValid())
	{
		SkillComboBox->RefreshOptions();
		SkillComboBox->SetSelectedItem(SelectedSkill);
	}

	if (bAppendResult)
	{
		AppendToolExecutionResult(TEXT("unreal.skill_list"), *Arguments, Result);
	}
}

TSharedRef<SWidget> SUnrealMcpChatPanel::MakeSkillComboOption(TSharedPtr<FUnrealMcpSkillOption> SkillOption) const
{
	const FString Name = SkillOption.IsValid() ? SkillOption->Name : TEXT("<none>");
	const FString Title = SkillOption.IsValid() && !SkillOption->Title.IsEmpty() ? SkillOption->Title : Name;
	const FString Description = SkillOption.IsValid() ? SkillOption->Description : FString();
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(FText::FromString(FString::Printf(TEXT("%s - %s"), *Name, *Title)))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.Text(FText::FromString(Description.Left(160)))
		];
}

TSharedRef<SWidget> SUnrealMcpChatPanel::MakeSkillApplyModeComboOption(TSharedPtr<FString> ApplyMode) const
{
	return SNew(STextBlock)
		.Text(FText::FromString(ApplyMode.IsValid() ? *ApplyMode : TEXT("<mode>")));
}

FText SUnrealMcpChatPanel::GetSelectedSkillText() const
{
	if (!SelectedSkill.IsValid())
	{
		return FText::FromString(TEXT("No skill selected"));
	}
	return FText::FromString(SelectedSkill->Title.IsEmpty() || SelectedSkill->Title.Equals(SelectedSkill->Name, ESearchCase::CaseSensitive)
		? SelectedSkill->Name
		: FString::Printf(TEXT("%s - %s"), *SelectedSkill->Name, *SelectedSkill->Title));
}

FText SUnrealMcpChatPanel::GetSelectedSkillDescriptionText() const
{
	if (!SelectedSkill.IsValid())
	{
		return FText::FromString(TEXT("No project skill loaded. Use Refresh Skills to scan Tools/UnrealMcpSkills."));
	}

	const FString Description = SelectedSkill->Description.TrimStartAndEnd();
	const FString PathSuffix = SelectedSkill->Path.IsEmpty() ? FString() : FString::Printf(TEXT("  [%s]"), *SelectedSkill->Path);
	return FText::FromString(Description.IsEmpty()
		? FString::Printf(TEXT("%s%s"), *SelectedSkill->Name, *PathSuffix)
		: FString::Printf(TEXT("%s%s"), *Description, *PathSuffix));
}

FText SUnrealMcpChatPanel::GetSelectedSkillApplyModeText() const
{
	return FText::FromString(SelectedSkillApplyMode.IsValid() ? *SelectedSkillApplyMode : UnrealMcpChat::SkillApplyModeApplyToMemory());
}

FString SUnrealMcpChatPanel::GetSelectedSkillName() const
{
	return SelectedSkill.IsValid() ? SelectedSkill->Name : FString();
}

FString SUnrealMcpChatPanel::GetSelectedSkillApplyMode() const
{
	return SelectedSkillApplyMode.IsValid() ? *SelectedSkillApplyMode : UnrealMcpChat::SkillApplyModeApplyToMemory();
}

FString SUnrealMcpChatPanel::GetSkillTaskOrFallback() const
{
	const FString Task = SkillTaskTextBox.IsValid() ? SkillTaskTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	return Task.IsEmpty() ? TEXT("Apply this project skill to the next chat task.") : Task;
}

FString SUnrealMcpChatPanel::BuildSkillAskPrompt(const FString& SkillName, const FString& Task) const
{
	return FString::Printf(
		TEXT("/ask Use project skill '%s' for this task: %s\nIf you need the skill instructions, call unreal.skill_read or unreal.skill_apply first, then continue with the task."),
		*SkillName,
		*Task);
}

void SUnrealMcpChatPanel::AppendToolExecutionResult(const FString& ToolName, const FJsonObject& Arguments, const FUnrealMcpExecutionResult& Result)
{
	TSharedPtr<FJsonObject> ArgumentsObject = MakeShared<FJsonObject>();
	ArgumentsObject->Values = Arguments.Values;
	const FString ArgumentsJson = UnrealMcpChat::JsonObjectToPrettyString(ArgumentsObject);

	TSharedPtr<FUnrealMcpChatEntry> Entry = AppendToolCard(ToolName, FString(), ArgumentsJson);
	if (!Entry.IsValid())
	{
		return;
	}

	UpdateToolEntryWithResult(Entry, ArgumentsJson, Result);
}

void SUnrealMcpChatPanel::UpdateToolEntryWithResult(
	const TSharedPtr<FUnrealMcpChatEntry>& Entry,
	const FString& ArgumentsJson,
	const FUnrealMcpExecutionResult& Result)
{
	if (!Entry.IsValid())
	{
		return;
	}

	Entry->bIsPending = false;
	Entry->bIsError = Result.bIsError;
	Entry->Body = Result.Text;

	if (Result.StructuredContent.IsValid())
	{
		FString ApplicationPrompt;
		if (Result.StructuredContent->TryGetStringField(TEXT("applicationPrompt"), ApplicationPrompt) && !ApplicationPrompt.IsEmpty())
		{
			Entry->Body += TEXT("\n\nApplication prompt is available in structured content.");
		}

		Entry->Details = FString::Printf(
			TEXT("Arguments:\n%s\n\nStructured content:\n%s"),
			*ArgumentsJson,
			*UnrealMcpChat::JsonObjectToPrettyString(Result.StructuredContent));
	}

	InvalidateEntryWidgets();
	if (Entry->Type == EUnrealMcpChatEntryType::Tool)
	{
		ScrollToolLogToEnd();
	}
	else
	{
		ScrollTranscriptToEnd();
	}
	SaveHistory();
}

void SUnrealMcpChatPanel::SendCurrentInput()
{
	if (!InputTextBox.IsValid())
	{
		return;
	}

	const FString CommandText = InputTextBox->GetText().ToString().TrimStartAndEnd();
	InputTextBox->SetText(FText::GetEmpty());
	SendCommand(CommandText);
}

void SUnrealMcpChatPanel::SendCommand(const FString& CommandText)
{
	if (CommandText.IsEmpty())
	{
		return;
	}

	const FString TrimmedCommand = CommandText.TrimStartAndEnd();
	if (TrimmedCommand.Equals(TEXT("/stop_ai"), ESearchCase::IgnoreCase) || TrimmedCommand.Equals(TEXT("/stop ai"), ESearchCase::IgnoreCase))
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		StopAssistantRequest();
		return;
	}

	const bool bIsSteerCommand = TrimmedCommand.Equals(TEXT("/steer"), ESearchCase::IgnoreCase)
		|| TrimmedCommand.StartsWith(TEXT("/steer "), ESearchCase::IgnoreCase);
	if (bIsSteerCommand)
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		const FString SteeringInstruction = TrimmedCommand.Equals(TEXT("/steer"), ESearchCase::IgnoreCase)
			? FString()
			: TrimmedCommand.Mid(6).TrimStartAndEnd();

		if (SteeringInstruction.IsEmpty())
		{
			AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("Usage: /steer <guidance while AI is running>"), true);
			return;
		}

		if (!bAssistantRequestInFlight || !ActiveAssistantHandle.IsValid())
		{
			AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("/steer can only be used while an AI request is running. Use plain text or /ask to start a new AI turn."), true);
			return;
		}

		if (!ActiveAssistantHandle->Steer(SteeringInstruction))
		{
			AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), TEXT("Unable to queue steer guidance for the active AI request."), true);
		}
		return;
	}

	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("The chat panel is not connected to the module."), true);
		return;
	}

	if (bAssistantRequestInFlight)
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("An AI request is already in progress. Wait for it to finish, press Stop, or use /steer <guidance> to guide the active run."), true);
		return;
	}

	const bool bIsAskCommand = TrimmedCommand.Equals(TEXT("/ask"), ESearchCase::IgnoreCase)
		|| TrimmedCommand.StartsWith(TEXT("/ask "), ESearchCase::IgnoreCase);

	if (TrimmedCommand.Equals(TEXT("/reset_ai"), ESearchCase::IgnoreCase) || TrimmedCommand.Equals(TEXT("/reset ai"), ESearchCase::IgnoreCase))
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		LastAssistantResponseId.Reset();
		bHasInjectedPersistedContextThisSession = false;
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("AI conversation state reset."));
		return;
	}

	if (bIsAskCommand || !TrimmedCommand.StartsWith(TEXT("/")))
	{
		const FString UserPrompt = bIsAskCommand ? TrimmedCommand.Mid(4).TrimStartAndEnd() : TrimmedCommand;
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), UserPrompt.IsEmpty() ? TrimmedCommand : UserPrompt);

		if (UserPrompt.IsEmpty())
		{
			AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("Usage: /ask <prompt>"), true);
			return;
		}

		StartAssistantRequest(UserPrompt);
		return;
	}

	AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteChatCommand(CommandText);
	if (!Result.bIsError
		&& (TrimmedCommand.StartsWith(TEXT("/log"), ESearchCase::IgnoreCase)
			|| TrimmedCommand.StartsWith(TEXT("/tool unreal.tail_log"), ESearchCase::IgnoreCase)))
	{
		LastLogText = Result.Text;
	}

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("command"), TrimmedCommand);
	AppendToolExecutionResult(TEXT("chat.command"), *Arguments, Result);
}

void SUnrealMcpChatPanel::StopAssistantRequest()
{
	if (!bAssistantRequestInFlight)
	{
		return;
	}

	if (ActiveAssistantHandle.IsValid())
	{
		ActiveAssistantHandle->Cancel();
	}
}

void SUnrealMcpChatPanel::StartAssistantRequest(const FString& UserPrompt)
{
	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("The chat panel is not connected to the module."), true);
		return;
	}

	bAssistantRequestInFlight = true;
	ActiveAssistantRequestStartTime = FDateTime::UtcNow();
	ActiveAssistantEntry = AppendMessage(EUnrealMcpChatEntryType::Assistant, TEXT("Unreal MCP AI"), FString());
	if (ActiveAssistantEntry.IsValid())
	{
		ActiveAssistantEntry->Title = TEXT("Assistant");
		ActiveAssistantEntry->bIsPending = true;
		InvalidateEntryWidgets();
	}

	FString ConversationContext;
	if (!bHasInjectedPersistedContextThisSession || LastAssistantResponseId.TrimStartAndEnd().IsEmpty())
	{
		ConversationContext = BuildAssistantConversationContext(UserPrompt);
	}
	else
	{
		ConversationContext = BuildRagContextBlock(UserPrompt);
	}

	TWeakPtr<SUnrealMcpChatPanel> WeakPanel = SharedThis(this);
	ActiveAssistantHandle = OwnerModule->ExecuteAssistantTurnAsync(
		UserPrompt,
		ConversationContext,
		LastAssistantResponseId,
		[WeakPanel](const FUnrealMcpAssistantEvent& Event)
		{
			if (const TSharedPtr<SUnrealMcpChatPanel> PinnedThis = WeakPanel.Pin())
			{
				switch (Event.Type)
				{
				case EUnrealMcpAssistantEventType::TextDelta:
					if (PinnedThis->ActiveAssistantEntry.IsValid())
					{
						PinnedThis->ActiveAssistantEntry->bIsPending = false;
						PinnedThis->ActiveAssistantEntry->Body += Event.Text;
						PinnedThis->MoveEntryToEnd(PinnedThis->ActiveAssistantEntry);
						PinnedThis->InvalidateEntryWidgets();
						PinnedThis->ScrollTranscriptToEnd();
					}
					break;
				case EUnrealMcpAssistantEventType::ToolCallStarted:
					PinnedThis->AppendToolCard(Event.ToolName, Event.ToolCallId, Event.ToolArgumentsJson);
					break;
					case EUnrealMcpAssistantEventType::ToolCallFinished:
						if (const TSharedPtr<FUnrealMcpChatEntry>* ExistingEntry = PinnedThis->ToolEntriesByCallId.Find(Event.ToolCallId))
						{
							(*ExistingEntry)->Body = Event.Text;
							(*ExistingEntry)->bIsPending = false;
							(*ExistingEntry)->bIsError = Event.bIsError;
							if (!Event.bIsError && Event.ToolName.Equals(TEXT("unreal.tail_log"), ESearchCase::CaseSensitive))
							{
								PinnedThis->LastLogText = Event.Text;
							}
							PinnedThis->InvalidateEntryWidgets();
							PinnedThis->ScrollToolLogToEnd();
							PinnedThis->SaveHistory();
					}
					else
					{
						TSharedPtr<FUnrealMcpChatEntry> ToolEntry = PinnedThis->AppendToolCard(Event.ToolName, Event.ToolCallId, Event.ToolArgumentsJson);
							if (ToolEntry.IsValid())
							{
								ToolEntry->Body = Event.Text;
								ToolEntry->bIsPending = false;
								ToolEntry->bIsError = Event.bIsError;
								if (!Event.bIsError && Event.ToolName.Equals(TEXT("unreal.tail_log"), ESearchCase::CaseSensitive))
								{
									PinnedThis->LastLogText = Event.Text;
								}
								PinnedThis->InvalidateEntryWidgets();
								PinnedThis->ScrollToolLogToEnd();
								PinnedThis->SaveHistory();
							}
					}
					break;
				case EUnrealMcpAssistantEventType::Status:
				default:
					{
						TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
						Arguments->SetStringField(TEXT("source"), TEXT("assistant"));
						Arguments->SetStringField(TEXT("eventType"), TEXT("status"));

						FUnrealMcpExecutionResult StatusResult;
						StatusResult.Text = Event.Text;
						StatusResult.bIsError = Event.bIsError;
						StatusResult.StructuredContent = MakeShared<FJsonObject>();
						StatusResult.StructuredContent->SetStringField(TEXT("action"), TEXT("assistant_status"));
						StatusResult.StructuredContent->SetStringField(TEXT("toolName"), Event.ToolName);
						StatusResult.StructuredContent->SetStringField(TEXT("toolCallId"), Event.ToolCallId);
						StatusResult.StructuredContent->SetBoolField(TEXT("isError"), Event.bIsError);

						PinnedThis->AppendToolExecutionResult(TEXT("assistant.status"), *Arguments, StatusResult);
					}
					break;
				}
			}
		},
		[WeakPanel](const FUnrealMcpAssistantTurnResult& Result)
		{
			if (const TSharedPtr<SUnrealMcpChatPanel> PinnedThis = WeakPanel.Pin())
			{
				PinnedThis->bAssistantRequestInFlight = false;
				PinnedThis->ActiveAssistantRequestStartTime = FDateTime();
				PinnedThis->ActiveAssistantHandle.Reset();
				if (!Result.bIsError && !Result.ResponseId.IsEmpty())
				{
					PinnedThis->LastAssistantResponseId = Result.ResponseId;
				}
				else if (Result.bIsError
					&& Result.Text.Contains(TEXT("previous response"), ESearchCase::IgnoreCase)
					&& Result.Text.Contains(TEXT("not found"), ESearchCase::IgnoreCase))
				{
					PinnedThis->LastAssistantResponseId.Reset();
					PinnedThis->bHasInjectedPersistedContextThisSession = false;
				}

				if (PinnedThis->ActiveAssistantEntry.IsValid())
				{
					PinnedThis->ActiveAssistantEntry->bIsPending = false;
					if (PinnedThis->ActiveAssistantEntry->Body.IsEmpty() && !Result.Text.IsEmpty())
					{
						PinnedThis->ActiveAssistantEntry->Body = Result.Text;
					}
					PinnedThis->ActiveAssistantEntry->bIsError = Result.bIsError;
					PinnedThis->MoveEntryToEnd(PinnedThis->ActiveAssistantEntry);
				}

				if (Result.bIsError && PinnedThis->ActiveAssistantEntry.IsValid() && PinnedThis->ActiveAssistantEntry->Body.IsEmpty())
				{
					PinnedThis->ActiveAssistantEntry->Body = Result.Text;
				}
				else if (Result.bWasCancelled)
				{
					PinnedThis->AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), Result.Text.IsEmpty() ? TEXT("Generation stopped.") : Result.Text);
				}
				else if (Result.bIsError && (!PinnedThis->ActiveAssistantEntry.IsValid() || !PinnedThis->ActiveAssistantEntry->Body.Equals(Result.Text, ESearchCase::CaseSensitive)))
				{
					PinnedThis->AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP Error"), Result.Text, true);
				}

				PinnedThis->ActiveAssistantEntry.Reset();
				PinnedThis->bHasInjectedPersistedContextThisSession = true;
				PinnedThis->InvalidateEntryWidgets();
				PinnedThis->ScrollTranscriptToEnd();
				PinnedThis->SaveHistory();
			}
		});
}

TSharedPtr<FUnrealMcpChatEntry> SUnrealMcpChatPanel::AppendMessage(EUnrealMcpChatEntryType Type, const FString& Speaker, const FString& Message, bool bIsError)
{
	TSharedPtr<FUnrealMcpChatEntry> Entry = MakeShared<FUnrealMcpChatEntry>();
	Entry->Type = Type;
	Entry->Speaker = Speaker;
	Entry->Title = Speaker;
	Entry->Body = Message;
	Entry->bIsError = bIsError;

	Entries.Add(Entry);
	AddEntryWidget(Entry);
	SaveHistory();
	return Entry;
}

TSharedPtr<FUnrealMcpChatEntry> SUnrealMcpChatPanel::AppendToolCard(const FString& ToolName, const FString& ToolCallId, const FString& ArgumentsJson)
{
	TSharedPtr<FUnrealMcpChatEntry> Entry = MakeShared<FUnrealMcpChatEntry>();
	Entry->Type = EUnrealMcpChatEntryType::Tool;
	Entry->Speaker = TEXT("Tool");
	Entry->Title = ToolName;
	Entry->Details = ArgumentsJson;
	Entry->ToolCallId = ToolCallId;
	Entry->Body = TEXT("Running...");
	Entry->bIsPending = true;

	Entries.Add(Entry);
	if (!ToolCallId.IsEmpty())
	{
		ToolEntriesByCallId.Add(ToolCallId, Entry);
	}

	AddEntryWidget(Entry);
	SaveHistory();
	return Entry;
}

void SUnrealMcpChatPanel::AddEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry)
{
	AddEntryWidgetToPane(Entry, true);
}

void SUnrealMcpChatPanel::AddEntryWidgetToPane(const TSharedPtr<FUnrealMcpChatEntry>& Entry, bool bScrollAfterAdd)
{
	if (!Entry.IsValid())
	{
		return;
	}

	const bool bIsToolEntry = Entry->Type == EUnrealMcpChatEntryType::Tool;
	const bool bShouldAutoScroll = bScrollAfterAdd
		&& (bIsToolEntry ? UnrealMcpChat::IsScrollBoxNearBottom(ToolLogScrollBox) : IsTranscriptNearBottom());
	TSharedPtr<SVerticalBox> TargetEntriesBox = bIsToolEntry ? ToolLogEntriesBox : TranscriptEntriesBox;
	if (!TargetEntriesBox.IsValid())
	{
		return;
	}

	TargetEntriesBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildEntryWidget(Entry)
	];

	if (!bShouldAutoScroll)
	{
		return;
	}

	if (bIsToolEntry)
	{
		ScrollToolLogToEnd();
	}
	else
	{
		ScrollTranscriptToEnd();
	}
}

void SUnrealMcpChatPanel::RebuildEntryWidgets(bool bScrollTranscript, bool bScrollToolLog)
{
	const bool bShouldScrollTranscript = bScrollTranscript && IsTranscriptNearBottom();
	const bool bShouldScrollToolLog = bScrollToolLog && UnrealMcpChat::IsScrollBoxNearBottom(ToolLogScrollBox);

	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->ClearChildren();
	}

	if (ToolLogEntriesBox.IsValid())
	{
		ToolLogEntriesBox->ClearChildren();
	}

	for (const TSharedPtr<FUnrealMcpChatEntry>& Entry : Entries)
	{
		AddEntryWidgetToPane(Entry, false);
	}

	InvalidateEntryWidgets();
	if (bShouldScrollTranscript)
	{
		ScrollTranscriptToEnd();
	}
	if (bShouldScrollToolLog)
	{
		ScrollToolLogToEnd();
	}
}

TSharedRef<SWidget> SUnrealMcpChatPanel::BuildEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry)
{
	if (Entry->Type == EUnrealMcpChatEntryType::Tool)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([Entry]()
			{
				return UnrealMcpChat::GetBorderColor(*Entry);
			})
			.Padding(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([Entry]()
				{
					return UnrealMcpChat::GetBackgroundColor(*Entry);
				})
				.Padding(10.0f)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(!Entry->bToolCardExpanded)
					.OnAreaExpansionChanged(FOnBooleanValueChanged::CreateLambda([Entry](bool bIsExpanded)
					{
						Entry->bToolCardExpanded = bIsExpanded;
					}))
					.HeaderContent()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
							.ColorAndOpacity_Lambda([Entry]()
							{
								return UnrealMcpChat::GetEntryTitleColor(*Entry);
							})
							.Text_Lambda([Entry]()
							{
								return FText::FromString(UnrealMcpChat::BuildEntryTitleText(*Entry));
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.ContentPadding(FMargin(4.0f, 1.0f))
							.ToolTipText(LOCTEXT("CopyEntryTooltip", "Copy this entry."))
							.Text(LOCTEXT("CopyEntryIcon", "📋"))
							.OnClicked_Lambda([this, Entry]()
							{
								return HandleEntryCopyClicked(Entry);
							})
						]
					]
					.BodyContent()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							UnrealMcpChat::MakeSelectableReadOnlyText(
								TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([Entry]()
								{
									if (Entry->Body.IsEmpty() && Entry->bIsPending)
									{
										return FText::FromString(TEXT("Running..."));
									}

									return FText::FromString(Entry->Body);
								})),
								FAppStyle::GetFontStyle("NormalFont"))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 8.0f, 0.0f, 0.0f)
						[
							SNew(SBox)
							.Visibility_Lambda([Entry]()
							{
								return Entry->Details.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
							})
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								[
									SNew(STextBlock)
									.Font(FAppStyle::GetFontStyle("SmallFont"))
									.ColorAndOpacity(FLinearColor(0.78f, 0.80f, 0.84f, 1.0f))
									.Text(LOCTEXT("DetailsLabel", "Details"))
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.Padding(0.0f, 3.0f, 0.0f, 0.0f)
								[
									UnrealMcpChat::MakeSelectableReadOnlyText(
										TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([Entry]()
										{
											return FText::FromString(Entry->Details);
										})),
										FAppStyle::GetFontStyle("SmallFont"))
								]
							]
						]
					]
				]
			];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([Entry]()
		{
			return UnrealMcpChat::GetBorderColor(*Entry);
		})
		.Padding(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([Entry]()
			{
				return UnrealMcpChat::GetBackgroundColor(*Entry);
			})
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.ColorAndOpacity_Lambda([Entry]()
						{
							return UnrealMcpChat::GetEntryTitleColor(*Entry);
						})
						.Text_Lambda([Entry]()
						{
							return FText::FromString(UnrealMcpChat::BuildEntryTitleText(*Entry));
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ContentPadding(FMargin(4.0f, 1.0f))
						.ToolTipText(LOCTEXT("CopyEntryTooltip", "Copy this entry."))
						.Text(LOCTEXT("CopyEntryIcon", "📋"))
						.OnClicked_Lambda([this, Entry]()
						{
							return HandleEntryCopyClicked(Entry);
						})
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					UnrealMcpChat::MakeSelectableReadOnlyText(
						TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([Entry]()
						{
							if (Entry->Body.IsEmpty() && Entry->bIsPending)
							{
								return FText::FromString(TEXT("Thinking..."));
							}

							return FText::FromString(Entry->Body);
						})),
						FAppStyle::GetFontStyle("NormalFont"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([Entry]()
					{
						return Entry->Details.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
					})
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("SmallFont"))
							.ColorAndOpacity(FLinearColor(0.78f, 0.80f, 0.84f, 1.0f))
							.Text(LOCTEXT("DetailsLabel", "Details"))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 3.0f, 0.0f, 0.0f)
						[
							UnrealMcpChat::MakeSelectableReadOnlyText(
								TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([Entry]()
								{
									return FText::FromString(Entry->Details);
								})),
								FAppStyle::GetFontStyle("SmallFont"))
						]
					]
				]
			]
		];
}

FString SUnrealMcpChatPanel::BuildTranscriptText() const
{
	TArray<FString> Blocks;
	Blocks.Reserve(Entries.Num());

	for (const TSharedPtr<FUnrealMcpChatEntry>& Entry : Entries)
	{
		if (!Entry.IsValid() || Entry->Type == EUnrealMcpChatEntryType::Tool)
		{
			continue;
		}

		Blocks.Add(UnrealMcpChat::BuildEntryClipboardText(*Entry));
	}

	return FString::Join(Blocks, TEXT("\n\n"));
}

FString SUnrealMcpChatPanel::BuildToolLogText() const
{
	TArray<FString> Blocks;
	Blocks.Reserve(Entries.Num());

	for (const TSharedPtr<FUnrealMcpChatEntry>& Entry : Entries)
	{
		if (!Entry.IsValid() || Entry->Type != EUnrealMcpChatEntryType::Tool)
		{
			continue;
		}

		Blocks.Add(UnrealMcpChat::BuildEntryClipboardText(*Entry));
	}

	return FString::Join(Blocks, TEXT("\n\n"));
}

FString SUnrealMcpChatPanel::BuildEntryCopyText(const FUnrealMcpChatEntry& Entry) const
{
	return UnrealMcpChat::BuildEntryClipboardText(Entry);
}

FText SUnrealMcpChatPanel::GetActiveRequestProgressText() const
{
	if (!bAssistantRequestInFlight)
	{
		return FText::GetEmpty();
	}

	const FTimespan Elapsed = FDateTime::UtcNow() - ActiveAssistantRequestStartTime;
	const int32 ElapsedSeconds = FMath::Max(0, FMath::FloorToInt(Elapsed.GetTotalSeconds()));

	// FUnrealMcpAssistantEvent and FUnrealMcpAssistantTurnResult do not expose token usage yet,
	// so the v1 progress indicator reports elapsed wall-clock time only.
	return FText::FromString(FString::Printf(TEXT("⏱ %ds"), ElapsedSeconds));
}

bool SUnrealMcpChatPanel::IsTranscriptNearBottom() const
{
	return UnrealMcpChat::IsScrollBoxNearBottom(TranscriptScrollBox);
}

bool SUnrealMcpChatPanel::HasTranscriptEntries() const
{
	for (const TSharedPtr<FUnrealMcpChatEntry>& Entry : Entries)
	{
		if (Entry.IsValid() && Entry->Type != EUnrealMcpChatEntryType::Tool)
		{
			return true;
		}
	}

	return false;
}

bool SUnrealMcpChatPanel::HasToolLogEntries() const
{
	for (const TSharedPtr<FUnrealMcpChatEntry>& Entry : Entries)
	{
		if (Entry.IsValid() && Entry->Type == EUnrealMcpChatEntryType::Tool)
		{
			return true;
		}
	}

	return false;
}

FString SUnrealMcpChatPanel::BuildRagContextBlock(const FString& CurrentUserPrompt) const
{
	if (!OwnerModule)
	{
		return FString();
	}

	const FString Prompt = CurrentUserPrompt.TrimStartAndEnd();
	if (Prompt.IsEmpty() || Prompt.StartsWith(TEXT("/")))
	{
		return FString();
	}

	if (Prompt.Equals(LastRagContextPrompt, ESearchCase::CaseSensitive) && !LastRagContextBlock.IsEmpty())
	{
		return LastRagContextBlock;
	}

	auto RunToolRecommend = [this, &Prompt]()
	{
		TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("task"), Prompt);
		Arguments->SetStringField(TEXT("riskMax"), TEXT("critical"));
		Arguments->SetNumberField(TEXT("limit"), 6.0);
		Arguments->SetBoolField(TEXT("includeKnowledge"), true);
		Arguments->SetBoolField(TEXT("includeWorkflowDraft"), true);
		return OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.tool_recommend"), *Arguments);
	};

	FUnrealMcpExecutionResult RecommendResult = RunToolRecommend();
	if (RecommendResult.StructuredContent.IsValid())
	{
		FString KnowledgeNote;
		if (RecommendResult.StructuredContent->TryGetStringField(TEXT("knowledgeNote"), KnowledgeNote)
			&& KnowledgeNote.Contains(TEXT("Knowledge index not found"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> RefreshArguments = MakeShared<FJsonObject>();
			RefreshArguments->SetBoolField(TEXT("includeOfficialDocs"), true);
			RefreshArguments->SetBoolField(TEXT("includeVersionedDocs"), true);
			RefreshArguments->SetBoolField(TEXT("includeToolRegistry"), true);
			RefreshArguments->SetBoolField(TEXT("skipLowContent"), true);
			RefreshArguments->SetNumberField(TEXT("maxCards"), 2000.0);
			OwnerModule->ExecuteToolFromEditorUI(TEXT("unreal.knowledge_index_refresh"), *RefreshArguments);
			RecommendResult = RunToolRecommend();
		}
	}

	if (RecommendResult.bIsError || !RecommendResult.StructuredContent.IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> Root = RecommendResult.StructuredContent;
	TArray<FString> Lines;
	Lines.Add(TEXT("Local RAG/tool-planning capsule. Use this as compact evidence for tool choice; do not repeat it verbatim."));

	const TArray<TSharedPtr<FJsonValue>>* Recommendations = nullptr;
	if (Root->TryGetArrayField(TEXT("recommendations"), Recommendations) && Recommendations)
	{
		Lines.Add(TEXT("Recommended tools:"));
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Recommendations)
		{
			if (++Count > 5 || !Value.IsValid() || !Value->AsObject().IsValid())
			{
				break;
			}
			const TSharedPtr<FJsonObject> Tool = Value->AsObject();
			Lines.Add(FString::Printf(
				TEXT("- %s [%s, risk=%s]: %s"),
				*UnrealMcpChat::GetJsonStringField(Tool, TEXT("toolName")),
				*UnrealMcpChat::GetJsonStringField(Tool, TEXT("category")),
				*UnrealMcpChat::GetJsonStringField(Tool, TEXT("riskLevel")),
				*UnrealMcpChat::ClampForAssistantContext(UnrealMcpChat::GetJsonStringField(Tool, TEXT("description")), 180)));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* KnowledgeCards = nullptr;
	if (Root->TryGetArrayField(TEXT("knowledgeCards"), KnowledgeCards) && KnowledgeCards && KnowledgeCards->Num() > 0)
	{
		Lines.Add(TEXT("Relevant KnowledgeCards:"));
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : *KnowledgeCards)
		{
			if (++Count > 3 || !Value.IsValid() || !Value->AsObject().IsValid())
			{
				break;
			}
			const TSharedPtr<FJsonObject> Card = Value->AsObject();
			Lines.Add(FString::Printf(
				TEXT("- %s (%s): %s"),
				*UnrealMcpChat::GetJsonStringField(Card, TEXT("title")),
				*UnrealMcpChat::GetJsonStringField(Card, TEXT("sourcePath")),
				*UnrealMcpChat::ClampForAssistantContext(UnrealMcpChat::GetJsonStringField(Card, TEXT("excerpt")), 260)));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* WorkflowDraft = nullptr;
	if (Root->TryGetArrayField(TEXT("workflowDraft"), WorkflowDraft) && WorkflowDraft && WorkflowDraft->Num() > 0)
	{
		Lines.Add(TEXT("Suggested workflow gates:"));
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : *WorkflowDraft)
		{
			if (++Count > 6 || !Value.IsValid() || !Value->AsObject().IsValid())
			{
				break;
			}
			const TSharedPtr<FJsonObject> Step = Value->AsObject();
			Lines.Add(FString::Printf(
				TEXT("- %s: %s"),
				*UnrealMcpChat::GetJsonStringField(Step, TEXT("tool")),
				*UnrealMcpChat::GetJsonStringField(Step, TEXT("purpose"))));
		}
	}

	LastRagContextPrompt = Prompt;
	LastRagContextBlock = UnrealMcpChat::ClampForAssistantContext(FString::Join(Lines, TEXT("\n")), 2400);
	return LastRagContextBlock;
}

FString SUnrealMcpChatPanel::BuildToolsOverviewText(const FUnrealMcpExecutionResult& Result) const
{
	TArray<FString> Lines;
	TArray<FString> SelfExtensionLines;
	TArray<FString> LegacyLines;
	TArray<FString> CliDynamicLines;
	TArray<FString> BuiltInLines;
	TSet<FString> AlreadyListedLegacyNames;

	const TSharedPtr<FJsonObject>& Root = Result.StructuredContent;
	const TSharedPtr<FJsonObject>* AuditObject = nullptr;
	const TSharedPtr<FJsonObject>* RegistryValidationObject = nullptr;
	const TSharedPtr<FJsonObject>* HandlerRegistryObject = nullptr;

	const TSharedPtr<FJsonObject> Audit = Root.IsValid() && Root->TryGetObjectField(TEXT("audit"), AuditObject) && AuditObject && (*AuditObject).IsValid()
		? *AuditObject
		: Root;

	if (Root.IsValid())
	{
		Root->TryGetObjectField(TEXT("toolHandlerRegistry"), HandlerRegistryObject);
	}
	if (Audit.IsValid())
	{
		Audit->TryGetObjectField(TEXT("toolRegistryValidation"), RegistryValidationObject);
	}

	const TArray<TSharedPtr<FJsonValue>>* VisibleTools = nullptr;
	if (Audit.IsValid() && Audit->TryGetArrayField(TEXT("tools"), VisibleTools) && VisibleTools)
	{
		for (const TSharedPtr<FJsonValue>& ToolValue : *VisibleTools)
		{
			if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
			{
				continue;
			}

			const UnrealMcpChat::FToolOverviewItem Item = UnrealMcpChat::ToolOverviewItemFromVisibleTool(ToolValue->AsObject());
			const FString Line = UnrealMcpChat::FormatToolOverviewLine(Item);
			if (UnrealMcpChat::IsSelfExtensionTool(Item))
			{
				SelfExtensionLines.Add(Line);
			}
			else if (UnrealMcpChat::IsCliOrDynamicTool(Item))
			{
				CliDynamicLines.Add(Line);
			}
			else
			{
				BuiltInLines.Add(Line);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* LegacyTools = nullptr;
	if (Root.IsValid() && Root->TryGetArrayField(TEXT("legacyHiddenTools"), LegacyTools) && LegacyTools)
	{
		for (const TSharedPtr<FJsonValue>& ToolValue : *LegacyTools)
		{
			if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
			{
				continue;
			}

			const UnrealMcpChat::FToolOverviewItem Item = UnrealMcpChat::ToolOverviewItemFromRegistryEntry(ToolValue->AsObject());
			if (!Item.Name.IsEmpty() && !AlreadyListedLegacyNames.Contains(Item.Name))
			{
				LegacyLines.Add(UnrealMcpChat::FormatToolOverviewLine(Item));
				AlreadyListedLegacyNames.Add(Item.Name);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* AliasTools = nullptr;
	if (Root.IsValid() && Root->TryGetArrayField(TEXT("handlerAliases"), AliasTools) && AliasTools)
	{
		for (const TSharedPtr<FJsonValue>& ToolValue : *AliasTools)
		{
			if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
			{
				continue;
			}

			UnrealMcpChat::FToolOverviewItem Item = UnrealMcpChat::ToolOverviewItemFromRegistryEntry(ToolValue->AsObject());
			if (!Item.Name.IsEmpty() && !AlreadyListedLegacyNames.Contains(Item.Name))
			{
				Item.bLegacyHidden = true;
				LegacyLines.Add(UnrealMcpChat::FormatToolOverviewLine(Item));
				AlreadyListedLegacyNames.Add(Item.Name);
			}
		}
	}

	const int32 VisibleToolCount = UnrealMcpChat::GetJsonIntField(Audit, TEXT("toolCount"));
	const int32 RegistryEntryCount = UnrealMcpChat::GetJsonIntField(Root, TEXT("toolRegistryEntryCount"));
	const int32 HiddenLegacyCount = UnrealMcpChat::GetJsonIntField(Root, TEXT("legacyHiddenToolCount"));
	const int32 AliasCount = UnrealMcpChat::GetJsonIntField(Root, TEXT("handlerAliasCount"));
	const int32 MissingHandlerCount = UnrealMcpChat::GetJsonIntField(Audit, TEXT("missingHandlerCount"));
	const int32 SchemaIncompatibleCount = UnrealMcpChat::GetJsonIntField(Audit, TEXT("schemaIncompatibleCount"));
	const bool bFunctionalHealthy = UnrealMcpChat::GetJsonBoolField(Root, TEXT("functionalHealthy"), MissingHandlerCount == 0 && SchemaIncompatibleCount == 0);
	const bool bDocumentationHealthy = UnrealMcpChat::GetJsonBoolField(Root, TEXT("documentationHealthy"), true);

	int32 RegisteredHandlerCount = 0;
	if (RegistryValidationObject && (*RegistryValidationObject).IsValid())
	{
		RegisteredHandlerCount = UnrealMcpChat::GetJsonIntField(*RegistryValidationObject, TEXT("registeredHandlerCount"));
	}
	else if (HandlerRegistryObject && (*HandlerRegistryObject).IsValid())
	{
		RegisteredHandlerCount = UnrealMcpChat::GetJsonIntField(*HandlerRegistryObject, TEXT("registeredHandlerCount"));
	}

	Lines.Add(TEXT("MCP Tools Overview"));
	Lines.Add(FString::Printf(
		TEXT("visible=%d, registryEntries=%d, handlers=%d, hiddenLegacy=%d, aliases=%d, schemaIncompatible=%d, missingHandlers=%d"),
		VisibleToolCount,
		RegistryEntryCount,
		RegisteredHandlerCount,
		HiddenLegacyCount,
		AliasCount,
		SchemaIncompatibleCount,
		MissingHandlerCount));
	Lines.Add(FString::Printf(
		TEXT("functionalHealthy=%s, documentationHealthy=%s"),
		bFunctionalHealthy ? TEXT("true") : TEXT("false"),
		bDocumentationHealthy ? TEXT("true") : TEXT("false")));
	Lines.Add(TEXT("Legend: self-extension tools mutate or protect UEvolve's own MCP capabilities; AI/CLI dynamic tools run scripts, builds, external processes, or dynamic commands; legacy tools are hidden compatibility entries or aliases; built-in tools are normal editor/project tools."));

	UnrealMcpChat::AppendToolGroup(Lines, TEXT("Self Extension Tools"), SelfExtensionLines);
	UnrealMcpChat::AppendToolGroup(Lines, TEXT("AI / CLI Dynamic Tools"), CliDynamicLines);
	UnrealMcpChat::AppendToolGroup(Lines, TEXT("Legacy / Hidden Compatibility Tools"), LegacyLines);
	UnrealMcpChat::AppendToolGroup(Lines, TEXT("Built-in Editor / Project Tools"), BuiltInLines);

	return FString::Join(Lines, TEXT("\n"));
}

FString SUnrealMcpChatPanel::BuildAssistantConversationContext(const FString& CurrentUserPrompt) const
{
	TArray<FString> Blocks;
	Blocks.Reserve(UnrealMcpChat::AssistantHistoryMaxEntries);

	int32 RemainingChars = UnrealMcpChat::AssistantHistoryMaxChars;
	TArray<FString> CapsuleSections;

	const FString ActiveTaskBlock = UnrealMcpChat::BuildActiveTaskMemoryContextBlock();
	if (!ActiveTaskBlock.IsEmpty())
	{
		CapsuleSections.Add(ActiveTaskBlock);
		RemainingChars -= FMath::Min(RemainingChars, ActiveTaskBlock.Len() + 2);
	}

	const FString RagContextBlock = BuildRagContextBlock(CurrentUserPrompt);
	if (!RagContextBlock.IsEmpty() && RemainingChars > 0)
	{
		CapsuleSections.Add(RagContextBlock);
		RemainingChars -= FMath::Min(RemainingChars, RagContextBlock.Len() + 2);
	}

	bool bSkippedTrailingCurrentUserPrompt = false;

	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<FUnrealMcpChatEntry>& Entry = Entries[Index];
		if (!Entry.IsValid() || UnrealMcpChat::ShouldSkipForAssistantContext(*Entry))
		{
			continue;
		}

		if (!bSkippedTrailingCurrentUserPrompt
			&& Entry->Type == EUnrealMcpChatEntryType::User
			&& Entry->Body.Equals(CurrentUserPrompt, ESearchCase::CaseSensitive))
		{
			bSkippedTrailingCurrentUserPrompt = true;
			continue;
		}

		const FString Block = UnrealMcpChat::BuildAssistantContextBlock(*Entry);
		if (Block.IsEmpty())
		{
			continue;
		}

		const int32 Cost = Block.Len() + 2;
		if (Blocks.Num() > 0 && Cost > RemainingChars)
		{
			break;
		}

		Blocks.Add(Block);
		RemainingChars -= Cost;
		if (Blocks.Num() >= UnrealMcpChat::AssistantHistoryMaxEntries || RemainingChars <= 0)
		{
			break;
		}
	}

	if (Blocks.Num() > 0)
	{
		Algo::Reverse(Blocks);
		CapsuleSections.Add(
			TEXT("Recent local transcript and compressed tool evidence:\n")
			+ FString::Join(Blocks, TEXT("\n\n")));
	}

	if (CapsuleSections.Num() == 0)
	{
		return FString();
	}

	return
		TEXT("Codex-style compressed context capsule from this local Unreal Editor session. ")
		TEXT("Use it for continuity only: preserve verified facts, respect constraints, continue from the next step, and do not repeat this capsule back unless directly relevant.\n\n")
		TEXT("Compression schema: active objective/status, known verified tool evidence, constraints/risks, and next action.\n\n")
		+ UnrealMcpChat::ClampForAssistantContext(FString::Join(CapsuleSections, TEXT("\n\n")), UnrealMcpChat::AssistantHistoryMaxChars);
}

void SUnrealMcpChatPanel::InvalidateEntryWidgets()
{
	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (TranscriptScrollBox.IsValid())
	{
		TranscriptScrollBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (ToolLogEntriesBox.IsValid())
	{
		ToolLogEntriesBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (ToolLogScrollBox.IsValid())
	{
		ToolLogScrollBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}
}

bool SUnrealMcpChatPanel::MoveEntryToEnd(const TSharedPtr<FUnrealMcpChatEntry>& Entry)
{
	if (!Entry.IsValid())
	{
		return false;
	}

	const int32 ExistingIndex = Entries.IndexOfByPredicate([Entry](const TSharedPtr<FUnrealMcpChatEntry>& Candidate)
	{
		return Candidate == Entry;
	});
	if (ExistingIndex == INDEX_NONE || ExistingIndex == Entries.Num() - 1)
	{
		return false;
	}

	Entries.RemoveAt(ExistingIndex, 1, EAllowShrinking::No);
	Entries.Add(Entry);
	RebuildEntryWidgets(Entry->Type != EUnrealMcpChatEntryType::Tool, Entry->Type == EUnrealMcpChatEntryType::Tool);
	return true;
}

void SUnrealMcpChatPanel::ScrollTranscriptToEnd()
{
	bDeferredTranscriptShouldAutoScroll = IsTranscriptNearBottom();

	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (TranscriptScrollBox.IsValid())
	{
		TranscriptScrollBox->Invalidate(EInvalidateWidgetReason::Prepass);
		if (bDeferredTranscriptShouldAutoScroll)
		{
			TranscriptScrollBox->ScrollToEnd();
		}
	}

	if (!bDeferredTranscriptShouldAutoScroll)
	{
		DeferredTranscriptScrollFrames = 0;
		return;
	}

	DeferredTranscriptScrollFrames = FMath::Max(DeferredTranscriptScrollFrames, 10);
	if (!bDeferredTranscriptScrollActive)
	{
		bDeferredTranscriptScrollActive = true;
		RegisterActiveTimer(
			0.0f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SUnrealMcpChatPanel::HandleDeferredTranscriptScroll));
	}
}

void SUnrealMcpChatPanel::ScrollToolLogToEnd()
{
	bDeferredToolLogShouldAutoScroll = UnrealMcpChat::IsScrollBoxNearBottom(ToolLogScrollBox);

	if (ToolLogEntriesBox.IsValid())
	{
		ToolLogEntriesBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (ToolLogScrollBox.IsValid())
	{
		ToolLogScrollBox->Invalidate(EInvalidateWidgetReason::Prepass);
		if (bDeferredToolLogShouldAutoScroll)
		{
			ToolLogScrollBox->ScrollToEnd();
		}
	}

	if (!bDeferredToolLogShouldAutoScroll)
	{
		DeferredToolLogScrollFrames = 0;
		return;
	}

	DeferredToolLogScrollFrames = FMath::Max(DeferredToolLogScrollFrames, 10);
	if (!bDeferredToolLogScrollActive)
	{
		bDeferredToolLogScrollActive = true;
		RegisterActiveTimer(
			0.0f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SUnrealMcpChatPanel::HandleDeferredToolLogScroll));
	}
}

EActiveTimerReturnType SUnrealMcpChatPanel::HandleDeferredTranscriptScroll(double InCurrentTime, float InDeltaTime)
{
	(void)InCurrentTime;
	(void)InDeltaTime;

	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (TranscriptScrollBox.IsValid())
	{
		TranscriptScrollBox->Invalidate(EInvalidateWidgetReason::Prepass);
		if (bDeferredTranscriptShouldAutoScroll)
		{
			TranscriptScrollBox->ScrollToEnd();
		}
	}

	if (!bDeferredTranscriptShouldAutoScroll)
	{
		bDeferredTranscriptScrollActive = false;
		return EActiveTimerReturnType::Stop;
	}

	--DeferredTranscriptScrollFrames;
	if (DeferredTranscriptScrollFrames > 0)
	{
		return EActiveTimerReturnType::Continue;
	}

	bDeferredTranscriptScrollActive = false;
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SUnrealMcpChatPanel::HandleDeferredToolLogScroll(double InCurrentTime, float InDeltaTime)
{
	(void)InCurrentTime;
	(void)InDeltaTime;

	if (ToolLogEntriesBox.IsValid())
	{
		ToolLogEntriesBox->Invalidate(EInvalidateWidgetReason::Prepass);
	}

	if (ToolLogScrollBox.IsValid())
	{
		ToolLogScrollBox->Invalidate(EInvalidateWidgetReason::Prepass);
		if (bDeferredToolLogShouldAutoScroll)
		{
			ToolLogScrollBox->ScrollToEnd();
		}
	}

	if (!bDeferredToolLogShouldAutoScroll)
	{
		bDeferredToolLogScrollActive = false;
		return EActiveTimerReturnType::Stop;
	}

	--DeferredToolLogScrollFrames;
	if (DeferredToolLogScrollFrames > 0)
	{
		return EActiveTimerReturnType::Continue;
	}

	bDeferredToolLogScrollActive = false;
	return EActiveTimerReturnType::Stop;
}

void SUnrealMcpChatPanel::LoadHistory()
{
	Entries.Reset();
	ToolEntriesByCallId.Reset();
	LastAssistantResponseId.Reset();
	LastLogText.Reset();
	LastRagContextPrompt.Reset();
	LastRagContextBlock.Reset();
	bHasInjectedPersistedContextThisSession = false;

	const FString HistoryPath = UnrealMcpChat::GetHistoryFilePath();
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *HistoryPath))
	{
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return;
	}

	RootObject->TryGetStringField(TEXT("last_response_id"), LastAssistantResponseId);
	RootObject->TryGetStringField(TEXT("last_log_text"), LastLogText);

	const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), EntriesArray) || !EntriesArray)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		TSharedPtr<FUnrealMcpChatEntry> Entry = MakeShared<FUnrealMcpChatEntry>();
		Entry->Type = UnrealMcpChat::EntryTypeFromString(EntryObject->GetStringField(TEXT("type")));
		EntryObject->TryGetStringField(TEXT("speaker"), Entry->Speaker);
		EntryObject->TryGetStringField(TEXT("title"), Entry->Title);
		EntryObject->TryGetStringField(TEXT("body"), Entry->Body);
		EntryObject->TryGetStringField(TEXT("details"), Entry->Details);
		EntryObject->TryGetStringField(TEXT("tool_call_id"), Entry->ToolCallId);
		EntryObject->TryGetBoolField(TEXT("is_error"), Entry->bIsError);
		EntryObject->TryGetBoolField(TEXT("is_pending"), Entry->bIsPending);

		Entries.Add(Entry);
		AddEntryWidget(Entry);
	}
}

void SUnrealMcpChatPanel::SaveHistory() const
{
	const FString HistoryPath = UnrealMcpChat::GetHistoryFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(HistoryPath), true);

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("last_response_id"), LastAssistantResponseId);
	RootObject->SetStringField(TEXT("last_log_text"), LastLogText);

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (const TSharedPtr<FUnrealMcpChatEntry>& Entry : Entries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetStringField(TEXT("type"), UnrealMcpChat::EntryTypeToString(Entry->Type));
		EntryObject->SetStringField(TEXT("speaker"), Entry->Speaker);
		EntryObject->SetStringField(TEXT("title"), Entry->Title);
		EntryObject->SetStringField(TEXT("body"), Entry->Body);
		EntryObject->SetStringField(TEXT("details"), Entry->Details);
		EntryObject->SetStringField(TEXT("tool_call_id"), Entry->ToolCallId);
		EntryObject->SetBoolField(TEXT("is_error"), Entry->bIsError);
		EntryObject->SetBoolField(TEXT("is_pending"), Entry->bIsPending);
		EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObject));
	}

	RootObject->SetArrayField(TEXT("entries"), EntriesArray);

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(JsonText, *HistoryPath);
}

void SUnrealMcpChatPanel::ResetHistory(bool bAddReadyMessage)
{
	Entries.Reset();
	ToolEntriesByCallId.Reset();
	LastAssistantResponseId.Reset();
	LastLogText.Reset();
	LastRagContextPrompt.Reset();
	LastRagContextBlock.Reset();
	bHasInjectedPersistedContextThisSession = false;
	ActiveAssistantEntry.Reset();
	ActiveAssistantHandle.Reset();
	bAssistantRequestInFlight = false;
	ActiveAssistantRequestStartTime = FDateTime();
	bDeferredTranscriptScrollActive = false;
	bDeferredTranscriptShouldAutoScroll = true;
	DeferredTranscriptScrollFrames = 0;
	bDeferredToolLogScrollActive = false;
	bDeferredToolLogShouldAutoScroll = true;
	DeferredToolLogScrollFrames = 0;

	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->ClearChildren();
	}

	if (ToolLogEntriesBox.IsValid())
	{
		ToolLogEntriesBox->ClearChildren();
	}

	SaveHistory();

	if (bAddReadyMessage)
	{
		AppendMessage(
			EUnrealMcpChatEntryType::System,
			TEXT("Unreal MCP"),
			TEXT("Ready. Type plain text or /ask to use the AI assistant, /steer <guidance> to guide a running generation, press Stop to cancel, and use /help for direct commands. Tool calls will appear as cards and this transcript is persisted in Saved/UnrealMcp/ChatHistory.json."));
	}
}

#undef LOCTEXT_NAMESPACE
