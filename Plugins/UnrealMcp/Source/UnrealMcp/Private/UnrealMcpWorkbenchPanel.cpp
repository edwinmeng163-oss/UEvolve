#include "UnrealMcpWorkbenchPanel.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "UnrealMcpModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UnrealMcpWorkbenchPanel"

namespace UnrealMcpWorkbench
{
	TSharedPtr<FJsonObject> MakeEmptyObject()
	{
		return MakeShared<FJsonObject>();
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

	FString BoolToStatus(bool bValue)
	{
		return bValue ? TEXT("OK") : TEXT("Needs attention");
	}

	FString BoolToText(bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	FString NumberFieldToString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Fallback = TEXT("-"))
	{
		if (!Object.IsValid())
		{
			return Fallback;
		}

		double Value = 0.0;
		return Object->TryGetNumberField(FieldName, Value)
			? FString::FromInt(static_cast<int32>(Value))
			: Fallback;
	}

	FString StringFieldOrFallback(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Fallback = TEXT("-"))
	{
		if (!Object.IsValid())
		{
			return Fallback;
		}

		FString Value;
		return Object->TryGetStringField(FieldName, Value) && !Value.IsEmpty() ? Value : Fallback;
	}

	TSharedPtr<FJsonObject> ObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* ChildObject = nullptr;
		return Object->TryGetObjectField(FieldName, ChildObject) && ChildObject ? *ChildObject : nullptr;
	}

	FString TruncateForPanel(const FString& Text, int32 MaxChars)
	{
		if (Text.Len() <= MaxChars)
		{
			return Text;
		}
		return Text.Left(MaxChars) + TEXT("\n... truncated ...");
	}
}

void SUnrealMcpWorkbenchPanel::Construct(const FArguments& InArgs, FUnrealMcpModule* InOwnerModule)
{
	OwnerModule = InOwnerModule;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "Unreal MCP Workbench"))
				.Font(FAppStyle::GetFontStyle("HeadingMedium"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 10.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("Subtitle", "A thin self-extension console. It calls existing MCP tools for status, audit, test, pipeline, and lock checks without duplicating backend logic."))
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
					.Text(LOCTEXT("RefreshStatus", "Refresh Status"))
					.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleRefreshClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("RunAudit", "Run Audit"))
					.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleAuditClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("RunCoreTests", "Run Core Tests"))
					.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleCoreTestsClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("PipelineStatus", "Pipeline Status"))
					.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandlePipelineStatusClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("LockStatus", "Lock Status"))
					.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleLockStatusClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyResult", "Copy Result"))
					.ToolTipText(LOCTEXT("CopyResultTooltip", "Copy the latest Workbench tool result, including structuredContent when available."))
					.IsEnabled_Lambda([this]()
					{
						return !LastResultText.IsEmpty();
					})
					.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleCopyResultClicked)
				]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("SkillActivityStatus", "Skill Activity"))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleSkillActivityStatusClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("DistillSkillDraft", "Distill Draft"))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleSkillDistillDraftClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PromoteSkillDryRun", "Promote Dry Run"))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleSkillPromoteDryRunClicked)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("RefreshKnowledge", "Refresh Knowledge"))
						.ToolTipText(LOCTEXT("RefreshKnowledgeTooltip", "Rebuild the local KnowledgeCard index from Saved docs, versioned docs, and ToolRegistry metadata."))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleKnowledgeRefreshClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("SearchKnowledge", "Search Knowledge"))
						.ToolTipText(LOCTEXT("SearchKnowledgeTooltip", "Run a default knowledge_search smoke query for Blueprint, Widget, and self-extension guidance."))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleKnowledgeSearchClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("RecommendTools", "Recommend Tools"))
						.ToolTipText(LOCTEXT("RecommendToolsTooltip", "Recommend tools for a representative Widget HUD task using local RAG and ToolRegistry policy."))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleToolRecommendClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("RunKnowledgeEvals", "Run RAG Evals"))
						.ToolTipText(LOCTEXT("RunKnowledgeEvalsTooltip", "Run versioned local RAG regression cases."))
						.OnClicked(this, &SUnrealMcpWorkbenchPanel::HandleKnowledgeEvalClicked)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f)
				[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(10.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SummaryTitle", "Self-Extension Health"))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot().AutoHeight()[MakeMetricRow(LOCTEXT("HealthLabel", "Health"), SAssignNew(HealthValueText, STextBlock).Text(LOCTEXT("NotLoaded", "Not loaded")))]
					+ SVerticalBox::Slot().AutoHeight()[MakeMetricRow(LOCTEXT("ToolCountLabel", "Visible tools"), SAssignNew(ToolCountValueText, STextBlock).Text(LOCTEXT("DashA", "-")))]
					+ SVerticalBox::Slot().AutoHeight()[MakeMetricRow(LOCTEXT("TestCountLabel", "Known tests"), SAssignNew(TestCountValueText, STextBlock).Text(LOCTEXT("DashB", "-")))]
					+ SVerticalBox::Slot().AutoHeight()[MakeMetricRow(LOCTEXT("SupervisorLabel", "Supervisor log"), SAssignNew(SupervisorValueText, STextBlock).Text(LOCTEXT("DashC", "-")))]
					+ SVerticalBox::Slot().AutoHeight()[MakeMetricRow(LOCTEXT("MemoryKeyLabel", "Memory key"), SAssignNew(MemoryKeyValueText, STextBlock).Text(LOCTEXT("DashD", "-")))]
					+ SVerticalBox::Slot().AutoHeight()[MakeMetricRow(LOCTEXT("NextStepLabel", "Recommended next step"), SAssignNew(NextStepValueText, STextBlock).AutoWrapText(true).Text(LOCTEXT("DashE", "-")))]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 6.0f)
			[
				MakeMetricRow(LOCTEXT("LastActionLabel", "Last action"), SAssignNew(LastActionValueText, STextBlock).Text(LOCTEXT("None", "None")))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(LastResultValueText, STextBlock)
						.AutoWrapText(true)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.Text(LOCTEXT("ResultPlaceholder", "Click Refresh Status to load the current MCP workbench snapshot."))
					]
				]
			]
		]
	];

	HandleRefreshClicked();
}

FReply SUnrealMcpWorkbenchPanel::HandleRefreshClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("memoryKey"), TEXT("mcp.extension.pipeline"));
	Arguments->SetBoolField(TEXT("includeBuildLogTail"), false);
	RunToolAndDisplay(TEXT("unreal.mcp_workbench_status"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleAuditClicked()
{
	RunToolAndDisplay(TEXT("unreal.mcp_tool_audit"), UnrealMcpWorkbench::MakeEmptyObject());
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleCoreTestsClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("testsDir"), TEXT("Tools/UnrealMcpTests/Core"));
	Arguments->SetBoolField(TEXT("readProjectMemory"), false);
	Arguments->SetBoolField(TEXT("writeProjectMemory"), false);
	RunToolAndDisplay(TEXT("unreal.mcp_run_test_suite"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandlePipelineStatusClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("memoryKey"), TEXT("mcp.extension.pipeline"));
	Arguments->SetBoolField(TEXT("includeBuildLogTail"), false);
	RunToolAndDisplay(TEXT("unreal.mcp_pipeline_status"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleLockStatusClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("mode"), TEXT("status"));
	RunToolAndDisplay(TEXT("unreal.mcp_lock_extension_session"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleSkillActivityStatusClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetBoolField(TEXT("includeRecentEvents"), true);
	Arguments->SetNumberField(TEXT("maxEvents"), 10.0);
	RunToolAndDisplay(TEXT("unreal.skill_activity_status"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleSkillDistillDraftClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetBoolField(TEXT("writeDraft"), true);
	Arguments->SetBoolField(TEXT("includeEvents"), false);
	Arguments->SetBoolField(TEXT("overwrite"), true);
	Arguments->SetNumberField(TEXT("maxEvents"), 200.0);
	RunToolAndDisplay(TEXT("unreal.skill_distill_from_activity"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleSkillPromoteDryRunClicked()
{
	if (LastSkillName.TrimStartAndEnd().IsEmpty())
	{
		UpdateLastResult(
			TEXT("unreal.skill_promote_draft"),
			TEXT("Run Distill Draft first, or call unreal.skill_promote_draft from Chat with an explicit skillName."),
			nullptr,
			true);
		return FReply::Handled();
	}

	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("skillName"), LastSkillName);
	Arguments->SetBoolField(TEXT("dryRun"), true);
	RunToolAndDisplay(TEXT("unreal.skill_promote_draft"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleKnowledgeRefreshClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetBoolField(TEXT("includeOfficialDocs"), true);
	Arguments->SetBoolField(TEXT("includeVersionedDocs"), true);
	Arguments->SetBoolField(TEXT("includeToolRegistry"), true);
	Arguments->SetBoolField(TEXT("skipLowContent"), true);
	Arguments->SetNumberField(TEXT("maxCards"), 2000.0);
	RunToolAndDisplay(TEXT("unreal.knowledge_index_refresh"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleKnowledgeSearchClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("query"), TEXT("Blueprint Widget MCP self-extension workflow"));
	Arguments->SetNumberField(TEXT("limit"), 6.0);
	Arguments->SetBoolField(TEXT("includeText"), false);
	RunToolAndDisplay(TEXT("unreal.knowledge_search"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleToolRecommendClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("task"), TEXT("Build a Widget HUD using existing MCP tools, then verify the result."));
	Arguments->SetStringField(TEXT("riskMax"), TEXT("medium"));
	Arguments->SetNumberField(TEXT("limit"), 6.0);
	Arguments->SetBoolField(TEXT("includeKnowledge"), true);
	Arguments->SetBoolField(TEXT("includeWorkflowDraft"), true);
	RunToolAndDisplay(TEXT("unreal.tool_recommend"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleKnowledgeEvalClicked()
{
	TSharedPtr<FJsonObject> Arguments = UnrealMcpWorkbench::MakeEmptyObject();
	Arguments->SetStringField(TEXT("evalPath"), TEXT("Tools/UnrealMcpKnowledge/Evals"));
	Arguments->SetBoolField(TEXT("refreshIndex"), false);
	Arguments->SetBoolField(TEXT("includeDetails"), false);
	Arguments->SetNumberField(TEXT("limit"), 6.0);
	RunToolAndDisplay(TEXT("unreal.knowledge_eval_run"), Arguments);
	return FReply::Handled();
}

FReply SUnrealMcpWorkbenchPanel::HandleCopyResultClicked()
{
	if (!LastResultText.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*LastResultText);
	}
	return FReply::Handled();
}

void SUnrealMcpWorkbenchPanel::RunToolAndDisplay(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments)
{
	if (!OwnerModule)
	{
		UpdateLastResult(ToolName, TEXT("Workbench has no module owner."), nullptr, true);
		return;
	}

	const TSharedPtr<FJsonObject> SafeArguments = Arguments.IsValid() ? Arguments : UnrealMcpWorkbench::MakeEmptyObject();
	const FUnrealMcpExecutionResult Result = OwnerModule->ExecuteToolFromEditorUI(ToolName, *SafeArguments);
	UpdateLastResult(ToolName, Result.Text, Result.StructuredContent, Result.bIsError);

	if (ToolName == TEXT("unreal.mcp_workbench_status") && Result.StructuredContent.IsValid())
	{
		UpdateWorkbenchSummary(Result.StructuredContent, Result.bIsError);
	}
}

void SUnrealMcpWorkbenchPanel::UpdateWorkbenchSummary(const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError)
{
	bool bHealthy = false;
	StructuredContent->TryGetBoolField(TEXT("healthy"), bHealthy);

	if (HealthValueText.IsValid())
	{
		HealthValueText->SetText(FText::FromString(bIsError ? TEXT("Error") : UnrealMcpWorkbench::BoolToStatus(bHealthy)));
		HealthValueText->SetColorAndOpacity(bHealthy && !bIsError ? FSlateColor(FLinearColor(0.28f, 0.78f, 0.42f)) : FSlateColor(FLinearColor(0.95f, 0.42f, 0.28f)));
	}

	if (ToolCountValueText.IsValid())
	{
		ToolCountValueText->SetText(FText::FromString(UnrealMcpWorkbench::NumberFieldToString(StructuredContent, TEXT("visibleToolCount"))));
	}

	if (TestCountValueText.IsValid())
	{
		TestCountValueText->SetText(FText::FromString(UnrealMcpWorkbench::NumberFieldToString(StructuredContent, TEXT("testCaseCount"))));
	}

	if (SupervisorValueText.IsValid())
	{
		bool bSupervisorLogFound = false;
		StructuredContent->TryGetBoolField(TEXT("supervisorLogFound"), bSupervisorLogFound);
		SupervisorValueText->SetText(FText::FromString(UnrealMcpWorkbench::BoolToText(bSupervisorLogFound)));
	}

	if (MemoryKeyValueText.IsValid())
	{
		MemoryKeyValueText->SetText(FText::FromString(UnrealMcpWorkbench::StringFieldOrFallback(StructuredContent, TEXT("memoryKey"))));
	}

	if (NextStepValueText.IsValid())
	{
		NextStepValueText->SetText(FText::FromString(UnrealMcpWorkbench::StringFieldOrFallback(StructuredContent, TEXT("recommendedNextStep"))));
	}
}

void SUnrealMcpWorkbenchPanel::UpdateLastResult(const FString& ToolName, const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError)
{
	if (StructuredContent.IsValid())
	{
		FString SkillName;
		if (StructuredContent->TryGetStringField(TEXT("skillName"), SkillName) && !SkillName.TrimStartAndEnd().IsEmpty())
		{
			LastSkillName = SkillName.TrimStartAndEnd();
		}
	}

	const FString StructuredJson = UnrealMcpWorkbench::JsonObjectToPrettyString(StructuredContent);
	LastResultText = FString::Printf(
		TEXT("%s\nStatus: %s\n\n%s\n\nstructuredContent:\n%s"),
		*ToolName,
		bIsError ? TEXT("error") : TEXT("ok"),
		*Text,
		*StructuredJson);

	if (LastActionValueText.IsValid())
	{
		LastActionValueText->SetText(FText::FromString(FString::Printf(TEXT("%s (%s)"), *ToolName, bIsError ? TEXT("error") : TEXT("ok"))));
		LastActionValueText->SetColorAndOpacity(bIsError ? FSlateColor(FLinearColor(0.95f, 0.42f, 0.28f)) : FSlateColor::UseForeground());
	}

	if (LastResultValueText.IsValid())
	{
		LastResultValueText->SetText(FText::FromString(UnrealMcpWorkbench::TruncateForPanel(LastResultText, 24000)));
		LastResultValueText->SetColorAndOpacity(bIsError ? FSlateColor(FLinearColor(0.95f, 0.42f, 0.28f)) : FSlateColor::UseForeground());
	}
}

TSharedRef<SWidget> SUnrealMcpWorkbenchPanel::MakeMetricRow(const FText& Label, const TSharedPtr<STextBlock>& ValueWidget) const
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 2.0f, 10.0f, 2.0f)
		[
			SNew(STextBlock)
			.MinDesiredWidth(150.0f)
			.Text(Label)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0.0f, 2.0f)
		[
			ValueWidget.ToSharedRef()
		];
}

#undef LOCTEXT_NAMESPACE
