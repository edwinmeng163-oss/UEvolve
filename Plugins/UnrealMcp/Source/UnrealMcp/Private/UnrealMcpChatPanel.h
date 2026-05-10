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
enum class EAiProviderKind : uint8;
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
	bool bToolCardExpanded = false;
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
	FReply HandleCopyToolLogClicked();
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
	void HandleProviderSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo); void HandleModelSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo); void HandleModelTextCommitted(const FText& InText, ETextCommit::Type CommitType);
	void HandleSkillSelectionChanged(TSharedPtr<FUnrealMcpSkillOption> NewSelection, ESelectInfo::Type SelectInfo);
	void HandleSkillApplyModeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void RefreshProviderOptions(); void RefreshModelOptionsForActiveProvider(); void LoadRecentModelsFromDisk(); void SaveRecentModelsToDisk() const;
	void SetActiveProviderById(const FString& NewId); void RememberRecentModel(const FString& ProviderId, const FString& Model);
	FText GetCurrentProviderDisplayText() const; FText GetCurrentModelDisplayText() const; bool IsActiveProviderModelLocked() const; static FString KindShortName(EAiProviderKind Kind);
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
	void AddEntryWidgetToPane(const TSharedPtr<FUnrealMcpChatEntry>& Entry, bool bScrollAfterAdd);
	TSharedRef<SWidget> BuildEntryWidget(const TSharedPtr<FUnrealMcpChatEntry>& Entry);
	void RebuildEntryWidgets(bool bScrollTranscript, bool bScrollToolLog);
	void InvalidateEntryWidgets();
	bool MoveEntryToEnd(const TSharedPtr<FUnrealMcpChatEntry>& Entry);
	void ScrollTranscriptToEnd();
	void ScrollToolLogToEnd();
	EActiveTimerReturnType HandleDeferredTranscriptScroll(double InCurrentTime, float InDeltaTime);
	EActiveTimerReturnType HandleDeferredToolLogScroll(double InCurrentTime, float InDeltaTime);
	FText GetActiveRequestProgressText() const;
	FString BuildEntryCopyText(const FUnrealMcpChatEntry& Entry) const;
	bool IsTranscriptNearBottom() const;
	FReply HandleEntryCopyClicked(TSharedPtr<FUnrealMcpChatEntry> Entry);
	void LoadHistory();
	void SaveHistory() const;
	void ResetHistory(bool bAddReadyMessage);
	FString BuildTranscriptText() const;
	FString BuildToolLogText() const;
	bool HasTranscriptEntries() const;
	bool HasToolLogEntries() const;
	FString BuildRagContextBlock(const FString& CurrentUserPrompt) const;
	FString BuildAssistantConversationContext(const FString& CurrentUserPrompt) const;
	FString BuildToolsOverviewText(const FUnrealMcpExecutionResult& Result) const;

	FUnrealMcpModule* OwnerModule = nullptr;
	FString LastAssistantResponseId;
	FString LastLogText;
	mutable FString LastRagContextPrompt;
	mutable FString LastRagContextBlock;
	bool bAssistantRequestInFlight = false;
	FDateTime ActiveAssistantRequestStartTime;
	bool bHasInjectedPersistedContextThisSession = false;
	TArray<TSharedPtr<FUnrealMcpChatEntry>> Entries;
	TMap<FString, TSharedPtr<FUnrealMcpChatEntry>> ToolEntriesByCallId;
	TArray<TSharedPtr<FUnrealMcpSkillOption>> SkillOptions;
	TSharedPtr<FUnrealMcpSkillOption> SelectedSkill;
	TArray<TSharedPtr<FString>> SkillApplyModes;
	TSharedPtr<FString> SelectedSkillApplyMode;
	TArray<TSharedPtr<FString>> ProviderOptionIds; TSharedPtr<FString> SelectedProviderId;
	TArray<TSharedPtr<FString>> ModelOptions; TSharedPtr<FString> SelectedModel;
	TMap<FString, TArray<FString>> RecentModelsByProvider;
	TSharedPtr<FUnrealMcpChatEntry> ActiveAssistantEntry;
	TSharedPtr<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> ActiveAssistantHandle;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveAiTestRequest;
	TSharedPtr<SEditableTextBox> InputTextBox;
	TSharedPtr<SEditableTextBox> SkillTaskTextBox;
	TSharedPtr<SEditableTextBox> ModelEditableTextBox; TSharedPtr<SComboBox<TSharedPtr<FString>>> ProviderComboBox; TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FUnrealMcpSkillOption>>> SkillComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> SkillApplyModeComboBox;
	TSharedPtr<STextBlock> SkillDescriptionText;
	TSharedPtr<SScrollBox> TranscriptScrollBox;
	TSharedPtr<SVerticalBox> TranscriptEntriesBox;
	TSharedPtr<SScrollBox> ToolLogScrollBox;
	TSharedPtr<SVerticalBox> ToolLogEntriesBox;
	bool bDeferredTranscriptScrollActive = false;
	bool bDeferredTranscriptShouldAutoScroll = true;
	int32 DeferredTranscriptScrollFrames = 0;
	bool bDeferredToolLogScrollActive = false;
	bool bDeferredToolLogShouldAutoScroll = true;
	int32 DeferredToolLogScrollFrames = 0;
};
