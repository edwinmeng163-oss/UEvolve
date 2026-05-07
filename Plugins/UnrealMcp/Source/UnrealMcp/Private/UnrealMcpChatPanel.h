#pragma once

#include "CoreMinimal.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class FUnrealMcpModule;
class FJsonObject;
class IHttpRequest;
class IUnrealMcpAssistantHandle;
class SEditableTextBox;
class SScrollBox;
class STextBlock;
class SVerticalBox;
struct FUnrealMcpExecutionResult;

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

struct FUnrealMcpSkillOption
{
	FString Name;
	FString Title;
	FString Description;
	FString Path;
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
	FReply HandleToolsOverviewClicked();
	FReply HandleOpenAiSettingsClicked();
	FReply HandleTestAiConnectionClicked();
	FReply HandleRefreshSkillsClicked();
	FReply HandleReadSelectedSkillClicked();
	FReply HandleApplySelectedSkillClicked();
	FReply HandleReadMemoryClicked();
	FReply HandleWriteCurrentTaskMemoryClicked();
	void HandleInputCommitted(const FText& InText, ETextCommit::Type CommitType);
	void HandlePresetClicked(FString CommandText);
	void HandleSkillSelectionChanged(TSharedPtr<FUnrealMcpSkillOption> NewSelection, ESelectInfo::Type SelectInfo);
	void HandleSkillApplyModeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void SendCurrentInput();
	void SendCommand(const FString& CommandText);
	void StopAssistantRequest();
	void StartAssistantRequest(const FString& UserPrompt);
	void StartAiConnectionTest();
	void RefreshSkillOptions(bool bAppendResult);
	TSharedRef<SWidget> MakeSkillComboOption(TSharedPtr<FUnrealMcpSkillOption> SkillOption) const;
	TSharedRef<SWidget> MakeSkillApplyModeComboOption(TSharedPtr<FString> ApplyMode) const;
	FText GetSelectedSkillText() const;
	FText GetSelectedSkillDescriptionText() const;
	FText GetSelectedSkillApplyModeText() const;
	FString GetSelectedSkillName() const;
	FString GetSelectedSkillApplyMode() const;
	FString GetSkillTaskOrFallback() const;
	FString BuildSkillAskPrompt(const FString& SkillName, const FString& Task) const;
	void AppendToolExecutionResult(const FString& ToolName, const FJsonObject& Arguments, const FUnrealMcpExecutionResult& Result);
	void UpdateToolEntryWithResult(
		const TSharedPtr<FUnrealMcpChatEntry>& Entry,
		const FString& ArgumentsJson,
		const FUnrealMcpExecutionResult& Result);
	TSharedPtr<FUnrealMcpChatEntry> AppendMessage(EUnrealMcpChatEntryType Type, const FString& Speaker, const FString& Message, bool bIsError = false);
	TSharedPtr<FUnrealMcpChatEntry> AppendToolCard(const FString& ToolName, const FString& ToolCallId, const FString& ArgumentsJson);
	void AddEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry);
	TSharedRef<SWidget> BuildEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry) const;
	void InvalidateEntryWidgets();
	void ScrollTranscriptToEnd();
	EActiveTimerReturnType HandleDeferredTranscriptScroll(double InCurrentTime, float InDeltaTime);
	void LoadHistory();
	void SaveHistory() const;
	void ResetHistory(bool bAddReadyMessage);
	FString BuildTranscriptText() const;
	FString BuildAssistantConversationContext(const FString& CurrentUserPrompt) const;
	FString BuildToolsOverviewText(const FUnrealMcpExecutionResult& Result) const;

	FUnrealMcpModule* OwnerModule = nullptr;
	FString LastAssistantResponseId;
	FString LastLogText;
	bool bAssistantRequestInFlight = false;
	bool bHasInjectedPersistedContextThisSession = false;
	TArray<TSharedPtr<FUnrealMcpChatEntry>> Entries;
	TMap<FString, TSharedPtr<FUnrealMcpChatEntry>> ToolEntriesByCallId;
	TArray<TSharedPtr<FUnrealMcpSkillOption>> SkillOptions;
	TSharedPtr<FUnrealMcpSkillOption> SelectedSkill;
	TArray<TSharedPtr<FString>> SkillApplyModes;
	TSharedPtr<FString> SelectedSkillApplyMode;
	TSharedPtr<FUnrealMcpChatEntry> ActiveAssistantEntry;
	TSharedPtr<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> ActiveAssistantHandle;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveAiTestRequest;
	TSharedPtr<SEditableTextBox> InputTextBox;
	TSharedPtr<SEditableTextBox> SkillTaskTextBox;
	TSharedPtr<SComboBox<TSharedPtr<FUnrealMcpSkillOption>>> SkillComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> SkillApplyModeComboBox;
	TSharedPtr<STextBlock> SkillDescriptionText;
	TSharedPtr<SScrollBox> TranscriptScrollBox;
	TSharedPtr<SVerticalBox> TranscriptEntriesBox;
	bool bDeferredTranscriptScrollActive = false;
	int32 DeferredTranscriptScrollFrames = 0;
};
