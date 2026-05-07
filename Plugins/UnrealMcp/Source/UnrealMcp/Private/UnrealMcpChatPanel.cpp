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
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
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
		switch (Entry.Type)
		{
		case EUnrealMcpChatEntryType::User:
			return FLinearColor(0.08f, 0.12f, 0.18f, 0.95f);
		case EUnrealMcpChatEntryType::Assistant:
			return Entry.bIsError ? FLinearColor(0.20f, 0.08f, 0.08f, 0.95f) : FLinearColor(0.08f, 0.16f, 0.10f, 0.95f);
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
				.Text(LOCTEXT("Subtitle", "Use the panel as either a command surface or an AI copilot. Plain text and /ask stream live model output, tool calls appear as cards, and /help still shows all direct slash commands."))
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
				SAssignNew(TranscriptScrollBox, SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(TranscriptEntriesBox, SVerticalBox)
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
					.IsEnabled_Lambda([this]()
					{
						return Entries.Num() > 0;
					})
					.OnClicked(this, &SUnrealMcpChatPanel::HandleCopyChatClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyLog", "Copy Log"))
					.IsEnabled_Lambda([this]()
					{
						return !LastLogText.IsEmpty();
					})
					.OnClicked(this, &SUnrealMcpChatPanel::HandleCopyLastLogClicked)
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

FReply SUnrealMcpChatPanel::HandleCopyLastLogClicked()
{
	if (!LastLogText.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*LastLogText);
	}
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
	ScrollTranscriptToEnd();
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
	AppendMessage(EUnrealMcpChatEntryType::System, Result.bIsError ? TEXT("Unreal MCP Error") : TEXT("Unreal MCP"), Result.Text, Result.bIsError);
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
							PinnedThis->ScrollTranscriptToEnd();
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
								PinnedThis->SaveHistory();
							}
					}
					break;
				case EUnrealMcpAssistantEventType::Status:
				default:
					PinnedThis->AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), Event.Text, Event.bIsError);
					break;
				}
			}
		},
		[WeakPanel](const FUnrealMcpAssistantTurnResult& Result)
		{
			if (const TSharedPtr<SUnrealMcpChatPanel> PinnedThis = WeakPanel.Pin())
			{
				PinnedThis->bAssistantRequestInFlight = false;
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
	if (!TranscriptEntriesBox.IsValid() || !Entry.IsValid())
	{
		return;
	}

	TranscriptEntriesBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildEntryWidget(Entry)
	];

	ScrollTranscriptToEnd();
}

TSharedRef<SWidget> SUnrealMcpChatPanel::BuildEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry) const
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
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					.Text_Lambda([Entry]()
					{
						if (Entry->Type == EUnrealMcpChatEntryType::Tool)
						{
							const FString Status = Entry->bIsPending ? TEXT("running") : (Entry->bIsError ? TEXT("error") : TEXT("done"));
							return FText::FromString(FString::Printf(TEXT("Tool • %s (%s)"), *Entry->Title, *Status));
						}

						return FText::FromString(Entry->Speaker);
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([Entry]()
					{
						if (Entry->Body.IsEmpty() && Entry->bIsPending)
						{
							return FText::FromString(TEXT("Thinking..."));
						}

						return FText::FromString(Entry->Body);
					})
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
							SNew(STextBlock)
							.AutoWrapText(true)
							.Font(FAppStyle::GetFontStyle("SmallFont"))
							.Text_Lambda([Entry]()
							{
								return FText::FromString(Entry->Details);
							})
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
		if (!Entry.IsValid())
		{
			continue;
		}

		Blocks.Add(UnrealMcpChat::BuildEntryClipboardText(*Entry));
	}

	return FString::Join(Blocks, TEXT("\n\n"));
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
		TranscriptEntriesBox->Invalidate(EInvalidateWidgetReason::Layout);
	}
}

	void SUnrealMcpChatPanel::ScrollTranscriptToEnd()
	{
		if (TranscriptScrollBox.IsValid())
		{
			TranscriptScrollBox->ScrollToEnd();
		}

		DeferredTranscriptScrollFrames = 2;
		if (!bDeferredTranscriptScrollActive)
		{
			bDeferredTranscriptScrollActive = true;
			RegisterActiveTimer(
				0.0f,
				FWidgetActiveTimerDelegate::CreateSP(this, &SUnrealMcpChatPanel::HandleDeferredTranscriptScroll));
		}
	}

	EActiveTimerReturnType SUnrealMcpChatPanel::HandleDeferredTranscriptScroll(double InCurrentTime, float InDeltaTime)
	{
		(void)InCurrentTime;
		(void)InDeltaTime;

		if (TranscriptScrollBox.IsValid())
		{
			TranscriptScrollBox->ScrollToEnd();
		}

		--DeferredTranscriptScrollFrames;
		if (DeferredTranscriptScrollFrames > 0)
		{
			return EActiveTimerReturnType::Continue;
		}

		bDeferredTranscriptScrollActive = false;
		return EActiveTimerReturnType::Stop;
	}

void SUnrealMcpChatPanel::LoadHistory()
{
	Entries.Reset();
	ToolEntriesByCallId.Reset();
	LastAssistantResponseId.Reset();
	LastLogText.Reset();
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
	bHasInjectedPersistedContextThisSession = false;
	ActiveAssistantEntry.Reset();
	ActiveAssistantHandle.Reset();
	bAssistantRequestInFlight = false;

	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->ClearChildren();
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
