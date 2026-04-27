#include "UnrealMcpChatPanel.h"

#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "UnrealMcpModule.h"
#include "Widgets/Input/SButton.h"
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
	static constexpr int32 AssistantHistoryMaxEntries = 12;
	static constexpr int32 AssistantHistoryMaxChars = 4000;
	static constexpr int32 AssistantHistoryMaxCharsPerEntry = 500;

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
			Lines.Add(TEXT("Arguments:"));
			Lines.Add(Entry.Details);
		}

		return FString::Join(Lines, TEXT("\n"));
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

		if (Entry.Type == EUnrealMcpChatEntryType::Tool)
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
		Block += Entry.Body.TrimStartAndEnd();

		return ClampForAssistantContext(Block, AssistantHistoryMaxCharsPerEntry);
	}
}

void SUnrealMcpChatPanel::Construct(const FArguments& InArgs, FUnrealMcpModule* InOwnerModule)
{
	OwnerModule = InOwnerModule;

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
			.Padding(0.0f, 10.0f, 0.0f, 10.0f)
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
					.HintText(LOCTEXT("InputHint", "Try build the current map, /ask inspect the TwinStick map, /stop_ai, or /tool unreal.spawn_actor {...}"))
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

	if (!OwnerModule)
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("The chat panel is not connected to the module."), true);
		return;
	}

	if (bAssistantRequestInFlight)
	{
		AppendMessage(EUnrealMcpChatEntryType::User, TEXT("You"), TrimmedCommand);
		AppendMessage(EUnrealMcpChatEntryType::System, TEXT("Unreal MCP"), TEXT("An AI request is already in progress. Wait for it to finish or press Stop."), true);
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

	TWeakPtr<SUnrealMcpChatPanel> WeakThis = SharedThis(this);
	ActiveAssistantHandle = OwnerModule->ExecuteAssistantTurnAsync(
		UserPrompt,
		ConversationContext,
		LastAssistantResponseId,
		[WeakThis](const FUnrealMcpAssistantEvent& Event)
		{
			if (const TSharedPtr<SUnrealMcpChatPanel> PinnedThis = WeakThis.Pin())
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
		[WeakThis](const FUnrealMcpAssistantTurnResult& Result)
		{
			if (const TSharedPtr<SUnrealMcpChatPanel> PinnedThis = WeakThis.Pin())
			{
				PinnedThis->bAssistantRequestInFlight = false;
				PinnedThis->ActiveAssistantHandle.Reset();
				if (!Result.bIsError && !Result.ResponseId.IsEmpty())
				{
					PinnedThis->LastAssistantResponseId = Result.ResponseId;
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
							.Text(LOCTEXT("DetailsLabel", "Arguments"))
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

	if (Blocks.Num() == 0)
	{
		return FString();
	}

	Algo::Reverse(Blocks);

	return
		TEXT("The editor persisted the following recent local chat transcript for continuity. ")
		TEXT("Treat it as prior project context. Do not repeat it back unless it is directly relevant.\n\n")
		+ FString::Join(Blocks, TEXT("\n\n"));
}

void SUnrealMcpChatPanel::InvalidateEntryWidgets()
{
	if (TranscriptEntriesBox.IsValid())
	{
		TranscriptEntriesBox->Invalidate(EInvalidateWidgetReason::Layout);
	}
}

void SUnrealMcpChatPanel::ScrollTranscriptToEnd() const
{
	if (TranscriptScrollBox.IsValid())
	{
		TranscriptScrollBox->ScrollToEnd();
	}
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
			TEXT("Ready. Type plain text or /ask to use the AI assistant, press Stop to cancel a generation, and use /help for direct commands. Tool calls will appear as cards and this transcript is persisted in Saved/UnrealMcp/ChatHistory.json."));
	}
}

#undef LOCTEXT_NAMESPACE
