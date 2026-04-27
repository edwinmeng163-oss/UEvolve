#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FUnrealMcpModule;
class IUnrealMcpAssistantHandle;
class SEditableTextBox;
class SScrollBox;
class SVerticalBox;

enum class EUnrealMcpChatEntryType : uint8
{
	User,
	Assistant,
	System,
	Tool
};

struct FUnrealMcpChatEntry
{
	EUnrealMcpChatEntryType Type = EUnrealMcpChatEntryType::System;
	FString Speaker;
	FString Title;
	FString Body;
	FString Details;
	FString ToolCallId;
	bool bIsError = false;
	bool bIsPending = false;
};

class SUnrealMcpChatPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpChatPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, FUnrealMcpModule* InOwnerModule);

private:
	FReply HandleSendClicked();
	FReply HandleStopClicked();
	FReply HandleClearClicked();
	FReply HandleCopyChatClicked();
	FReply HandleCopyLastLogClicked();
	void HandleInputCommitted(const FText& InText, ETextCommit::Type CommitType);
	void HandlePresetClicked(FString CommandText);
	void SendCurrentInput();
	void SendCommand(const FString& CommandText);
	void StopAssistantRequest();
	void StartAssistantRequest(const FString& UserPrompt);
	TSharedPtr<FUnrealMcpChatEntry> AppendMessage(EUnrealMcpChatEntryType Type, const FString& Speaker, const FString& Message, bool bIsError = false);
	TSharedPtr<FUnrealMcpChatEntry> AppendToolCard(const FString& ToolName, const FString& ToolCallId, const FString& ArgumentsJson);
	void AddEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry);
	TSharedRef<SWidget> BuildEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry) const;
	void InvalidateEntryWidgets();
	void ScrollTranscriptToEnd() const;
	void LoadHistory();
	void SaveHistory() const;
	void ResetHistory(bool bAddReadyMessage);
	FString BuildTranscriptText() const;
	FString BuildAssistantConversationContext(const FString& CurrentUserPrompt) const;

	FUnrealMcpModule* OwnerModule = nullptr;
	FString LastAssistantResponseId;
	FString LastLogText;
	bool bAssistantRequestInFlight = false;
	bool bHasInjectedPersistedContextThisSession = false;
	TArray<TSharedPtr<FUnrealMcpChatEntry>> Entries;
	TMap<FString, TSharedPtr<FUnrealMcpChatEntry>> ToolEntriesByCallId;
	TSharedPtr<FUnrealMcpChatEntry> ActiveAssistantEntry;
	TSharedPtr<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> ActiveAssistantHandle;
	TSharedPtr<SEditableTextBox> InputTextBox;
	TSharedPtr<SScrollBox> TranscriptScrollBox;
	TSharedPtr<SVerticalBox> TranscriptEntriesBox;
};
