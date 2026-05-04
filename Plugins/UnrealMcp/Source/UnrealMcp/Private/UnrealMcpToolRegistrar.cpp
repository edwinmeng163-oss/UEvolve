#include "UnrealMcpToolRegistrar.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UnrealMcp
{
	TSharedPtr<FJsonObject> MakeObjectSchema();
	TSharedPtr<FJsonObject> MakeStringProperty(const FString& Description, const FString& DefaultValue = FString());
	TSharedPtr<FJsonObject> MakeBoolProperty(const FString& Description, bool bDefaultValue);
	TSharedPtr<FJsonObject> MakeNumberProperty(const FString& Description, double DefaultValue);
	TSharedPtr<FJsonObject> MakeStringArrayProperty(const FString& Description);
	void AddToolDefinition(
		TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		const FString& Name,
		const FString& Title,
		const FString& Description,
		const TSharedPtr<FJsonObject>& InputSchema);

	namespace
	{
		FUnrealMcpToolDescriptor MakeDescriptor(
			const FString& Name,
			const FString& Title,
			const FString& Description,
			const FString& Category,
			const FString& SourceFile,
			EUnrealMcpToolRisk Risk = EUnrealMcpToolRisk::ReadOnly)
		{
			FUnrealMcpToolDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.Title = Title;
			Descriptor.Description = Description;
			Descriptor.Category = Category;
			Descriptor.HandlerName = Name;
			Descriptor.SourceFile = SourceFile;
			Descriptor.RiskLevel = Risk;
			Descriptor.Owner = TEXT("UEvolve Core");
			Descriptor.DocsPath = TEXT("README.md#tool-coverage");
			Descriptor.Reason = TEXT("Code descriptor default; reviewed registry JSON may override exposure, risk, owner, docs, and test metadata.");
			return Descriptor;
		}

		void RegisterEditorMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.editor_status"),
					TEXT("Editor Status"),
					TEXT("Returns the current Unreal Editor status, map, selected counts, engine version, and PIE state."),
					TEXT("editor"),
					TEXT("UnrealMcpEditorTools.cpp")),
				MakeObjectSchema());

			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.list_maps"),
					TEXT("List Maps"),
					TEXT("Lists all map assets under /Game."),
					TEXT("editor"),
					TEXT("UnrealMcpEditorTools.cpp")),
				MakeObjectSchema());

			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.list_selected_assets"),
					TEXT("List Selected Assets"),
					TEXT("Lists the assets currently selected in the Content Browser."),
					TEXT("editor"),
					TEXT("UnrealMcpEditorTools.cpp")),
				MakeObjectSchema());
		}

		void RegisterActorMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.list_selected_actors"),
					TEXT("List Selected Actors"),
					TEXT("Lists the actors currently selected in the level editor."),
					TEXT("actors"),
					TEXT("UnrealMcpActorTools.cpp")),
				MakeObjectSchema());
		}

		void RegisterBlueprintInspectorMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("blueprintPath"), MakeStringProperty(TEXT("Blueprint asset path to inspect.")));
				Properties->SetObjectField(TEXT("graphName"), MakeStringProperty(TEXT("Optional graph name. If omitted, all standard Blueprint graphs are listed.")));
				Properties->SetObjectField(TEXT("includePins"), MakeBoolProperty(TEXT("Whether to include pin details and links for each node."), true));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.bp_list_graph_nodes"),
						TEXT("List Blueprint Graph Nodes"),
						TEXT("Read-only inspection of Blueprint graphs, nodes, pins, and existing links."),
						TEXT("blueprint"),
						TEXT("UnrealMcpBlueprintTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("blueprintPath"), MakeStringProperty(TEXT("Blueprint asset path to inspect.")));
				Properties->SetObjectField(TEXT("graphName"), MakeStringProperty(TEXT("Graph name containing the node."), TEXT("EventGraph")));
				Properties->SetObjectField(TEXT("nodeGuid"), MakeStringProperty(TEXT("Node GUID to inspect.")));
				Properties->SetObjectField(TEXT("pinName"), MakeStringProperty(TEXT("Optional pin name. If omitted, all node pins are traced.")));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.bp_trace_pin_connections"),
						TEXT("Trace Blueprint Pin Connections"),
						TEXT("Read-only inspection of pin defaults and linked node/pin targets for one Blueprint node."),
						TEXT("blueprint"),
						TEXT("UnrealMcpBlueprintTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}
		}

		void RegisterWidgetInspectorMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("widgetBlueprintPath"), MakeStringProperty(TEXT("Widget Blueprint asset path to inspect.")));
			Properties->SetObjectField(TEXT("includeDesignerTree"), MakeBoolProperty(TEXT("Whether to include recursive designer tree children."), true));
			Properties->SetObjectField(TEXT("includeGraphNodes"), MakeBoolProperty(TEXT("Whether to include EventGraph node summaries."), false));
			TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
			Schema->SetObjectField(TEXT("properties"), Properties);
			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.widget_dump_tree"),
					TEXT("Dump Widget Tree"),
					TEXT("Read-only inspection of a Widget Blueprint tree, widget variables, slots, and optional EventGraph nodes."),
					TEXT("widget"),
					TEXT("UnrealMcpWidgetTools.cpp"),
					EUnrealMcpToolRisk::ReadOnly),
				Schema);
		}

		void RegisterSelfExtensionMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.mcp_tool_audit"),
					TEXT("Audit MCP Tools"),
					TEXT("Read-only audit of registered MCP tools, handlers, README documentation, and AI schema compatibility."),
					TEXT("self-extension"),
					TEXT("UnrealMcpSelfExtensionTools.cpp")),
				MakeObjectSchema());

			TSharedPtr<FJsonObject> WorkbenchProperties = MakeShared<FJsonObject>();
			WorkbenchProperties->SetObjectField(TEXT("memoryKey"), MakeStringProperty(TEXT("Project memory key to highlight in pipeline/workbench status."), TEXT("mcp.extension.pipeline")));
			WorkbenchProperties->SetObjectField(TEXT("includeBuildLogTail"), MakeBoolProperty(TEXT("Whether to include the latest build log tail."), false));
			WorkbenchProperties->SetObjectField(TEXT("buildLogTailLines"), MakeNumberProperty(TEXT("Maximum latest build log tail lines."), 80.0));

			TSharedPtr<FJsonObject> WorkbenchSchema = MakeObjectSchema();
			WorkbenchSchema->SetObjectField(TEXT("properties"), WorkbenchProperties);

			Registrar.Add(
				MakeDescriptor(
					TEXT("unreal.mcp_workbench_status"),
					TEXT("MCP Workbench Status"),
					TEXT("Read-only dashboard summary for self-extension health: tools, audit, memory, manifests, build/test artifacts, and supervisor status."),
					TEXT("self-extension"),
					TEXT("UnrealMcpSelfExtensionTools.cpp")),
				WorkbenchSchema);

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Natural-language task to turn into a structured change plan.")));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.preview_change_plan"),
						TEXT("Preview Change Plan"),
						TEXT("Converts a natural-language task into a structured plan with likely tools, risks, backups, and verification steps."),
						TEXT("self-extension"),
						TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("snapshotName"), MakeStringProperty(TEXT("Optional snapshot name. Defaults to a UTC timestamp.")));
				Properties->SetObjectField(TEXT("assetPath"), MakeStringProperty(TEXT("Content path to scan for assets."), TEXT("/Game")));
				Properties->SetObjectField(TEXT("includeActors"), MakeBoolProperty(TEXT("Include current level actors."), true));
				Properties->SetObjectField(TEXT("includeAssets"), MakeBoolProperty(TEXT("Include assets under assetPath."), true));
				Properties->SetObjectField(TEXT("includeBlueprints"), MakeBoolProperty(TEXT("Include Blueprint assets under assetPath."), true));
				Properties->SetObjectField(TEXT("includeWidgets"), MakeBoolProperty(TEXT("Include Widget Blueprint assets under assetPath."), true));
				Properties->SetObjectField(TEXT("includeMemory"), MakeBoolProperty(TEXT("Include project memory summaries."), true));
				Properties->SetObjectField(TEXT("includeSkills"), MakeBoolProperty(TEXT("Include promoted project skills."), true));
				Properties->SetObjectField(TEXT("actorLimit"), MakeNumberProperty(TEXT("Maximum actors to serialize."), 500.0));
				Properties->SetObjectField(TEXT("assetLimit"), MakeNumberProperty(TEXT("Maximum assets per asset category to serialize."), 1000.0));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.capture_project_snapshot"),
					TEXT("Capture Project Snapshot"),
					TEXT("Captures a project state snapshot for later objective diffing of actors, assets, Blueprints, widgets, memory, and skills."),
					TEXT("self-extension"),
					TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
					EUnrealMcpToolRisk::Low);
				Descriptor.bRequiresWrite = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Registrar.Add(Descriptor, Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("beforeSnapshotPath"), MakeStringProperty(TEXT("Before snapshot path. If omitted, the previous latest snapshot is used.")));
				Properties->SetObjectField(TEXT("afterSnapshotPath"), MakeStringProperty(TEXT("After snapshot path. If omitted, the latest snapshot is used.")));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.diff_project_snapshot"),
						TEXT("Diff Project Snapshot"),
						TEXT("Diffs two captured project snapshots and reports added/removed identities by area."),
						TEXT("self-extension"),
						TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Original task goal to verify.")));
				Properties->SetObjectField(TEXT("beforeSnapshotPath"), MakeStringProperty(TEXT("Optional before snapshot path.")));
				Properties->SetObjectField(TEXT("afterSnapshotPath"), MakeStringProperty(TEXT("Optional after snapshot path.")));
				Properties->SetObjectField(TEXT("expectedChangedAreas"), MakeStringArrayProperty(TEXT("Areas expected to change: actors, assets, blueprints, widgets, memory, or skills.")));
				Properties->SetObjectField(TEXT("expectedTools"), MakeStringArrayProperty(TEXT("Tool names expected to exist in tools/list.")));
				Properties->SetObjectField(TEXT("evidenceText"), MakeStringProperty(TEXT("Optional tool output or summary text to inspect.")));
				Properties->SetObjectField(TEXT("requiredEvidenceText"), MakeStringProperty(TEXT("Optional substring that must appear in evidenceText.")));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.verify_task_outcome"),
						TEXT("Verify Task Outcome"),
						TEXT("Checks task completion evidence using tools/list, snapshot diffs, and required text evidence."),
						TEXT("self-extension"),
						TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("text"), MakeStringProperty(TEXT("Raw error text to classify.")));
				Properties->SetObjectField(TEXT("logPath"), MakeStringProperty(TEXT("Optional project-local log file to tail and classify.")));
				Properties->SetObjectField(TEXT("tailLines"), MakeNumberProperty(TEXT("Lines to read from logPath."), 200.0));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.mcp_classify_error"),
						TEXT("Classify MCP Error"),
						TEXT("Classifies UBT, MCP, JSON schema, UE Python, HTTP endpoint, OpenAI API, and editor-state errors with next-step suggestions."),
						TEXT("self-extension"),
						TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("contentPath"), MakeStringProperty(TEXT("Sandbox Content Browser path. Must be under /Game/__UEvolve*."), TEXT("/Game/__UEvolveMcpTest")));
				Properties->SetObjectField(TEXT("reset"), MakeBoolProperty(TEXT("Whether to delete and recreate the sandbox directory."), true));
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Preview sandbox preparation without mutating assets."), false));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.mcp_prepare_test_sandbox"),
					TEXT("Prepare MCP Test Sandbox"),
					TEXT("Creates or resets a constrained /Game/__UEvolve* asset sandbox for disposable happy-path tests."),
					TEXT("self-extension"),
					TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
					EUnrealMcpToolRisk::Medium);
				Descriptor.bRequiresWrite = true;
				Descriptor.bDryRunSupport = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Registrar.Add(Descriptor, Schema);
			}
		}

		void RegisterAllMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			RegisterEditorMcpToolDescriptors(Registrar);
			RegisterActorMcpToolDescriptors(Registrar);
			RegisterBlueprintInspectorMcpToolDescriptors(Registrar);
			RegisterWidgetInspectorMcpToolDescriptors(Registrar);
			RegisterSelfExtensionMcpToolDescriptors(Registrar);
		}
	}

	void FUnrealMcpToolRegistrar::Add(const FUnrealMcpToolDescriptor& Descriptor, const TSharedPtr<FJsonObject>& InputSchema)
	{
		FRegisteredUnrealMcpToolDescriptor RegisteredTool;
		RegisteredTool.Descriptor = Descriptor;
		RegisteredTool.InputSchema = InputSchema.IsValid() ? InputSchema : MakeObjectSchema();
		Tools.Add(MoveTemp(RegisteredTool));
	}

	const TArray<FRegisteredUnrealMcpToolDescriptor>& FUnrealMcpToolRegistrar::GetTools() const
	{
		return Tools;
	}

	const TArray<FRegisteredUnrealMcpToolDescriptor>& GetRegisteredMcpToolDescriptors()
	{
		static const TArray<FRegisteredUnrealMcpToolDescriptor> RegisteredTools = []()
		{
			FUnrealMcpToolRegistrar Registrar;
			RegisterAllMcpToolDescriptors(Registrar);
			return Registrar.GetTools();
		}();
		return RegisteredTools;
	}

	const FRegisteredUnrealMcpToolDescriptor* FindRegisteredMcpToolDescriptor(const FString& ToolName)
	{
		for (const FRegisteredUnrealMcpToolDescriptor& RegisteredTool : GetRegisteredMcpToolDescriptors())
		{
			if (RegisteredTool.Descriptor.Name.Equals(ToolName, ESearchCase::CaseSensitive))
			{
				return &RegisteredTool;
			}
		}
		return nullptr;
	}

	void AppendRegisteredToolDefinitions(TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		for (const FRegisteredUnrealMcpToolDescriptor& RegisteredTool : GetRegisteredMcpToolDescriptors())
		{
			AddToolDefinition(
				ToolsArray,
				RegisteredTool.Descriptor.Name,
				RegisteredTool.Descriptor.Title,
				RegisteredTool.Descriptor.Description,
				RegisteredTool.InputSchema);
		}
	}

	TSharedPtr<FJsonObject> MakeToolDescriptorStatusObject()
	{
		TArray<TSharedPtr<FJsonValue>> ToolValues;
		TMap<FString, int32> CategoryCounts;
		for (const FRegisteredUnrealMcpToolDescriptor& RegisteredTool : GetRegisteredMcpToolDescriptors())
		{
			const FUnrealMcpToolDescriptor& Descriptor = RegisteredTool.Descriptor;
			TSharedPtr<FJsonObject> DescriptorObject = MakeShared<FJsonObject>();
			DescriptorObject->SetStringField(TEXT("name"), Descriptor.Name);
			DescriptorObject->SetStringField(TEXT("title"), Descriptor.Title);
			DescriptorObject->SetStringField(TEXT("category"), Descriptor.Category);
			DescriptorObject->SetStringField(TEXT("handlerName"), Descriptor.HandlerName.IsEmpty() ? Descriptor.Name : Descriptor.HandlerName);
			DescriptorObject->SetStringField(TEXT("sourceFile"), Descriptor.SourceFile);
			DescriptorObject->SetStringField(TEXT("exposure"), LexToString(Descriptor.Exposure));
			DescriptorObject->SetStringField(TEXT("riskLevel"), LexToString(Descriptor.RiskLevel));
			DescriptorObject->SetStringField(TEXT("testCoverage"), LexToString(Descriptor.TestCoverage));
			DescriptorObject->SetStringField(TEXT("docsPath"), Descriptor.DocsPath);
			ToolValues.Add(MakeShared<FJsonValueObject>(DescriptorObject));
			CategoryCounts.FindOrAdd(Descriptor.Category)++;
		}

		TSharedPtr<FJsonObject> CategoryCountsObject = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : CategoryCounts)
		{
			CategoryCountsObject->SetNumberField(Pair.Key, Pair.Value);
		}

		TSharedPtr<FJsonObject> StatusObject = MakeShared<FJsonObject>();
		StatusObject->SetNumberField(TEXT("descriptorCount"), GetRegisteredMcpToolDescriptors().Num());
		StatusObject->SetObjectField(TEXT("categoryCounts"), CategoryCountsObject);
		StatusObject->SetArrayField(TEXT("tools"), ToolValues);
		return StatusObject;
	}

	FString LexToString(EUnrealMcpToolExposure Exposure)
	{
		return Exposure == EUnrealMcpToolExposure::LegacyHidden ? TEXT("legacy_hidden") : TEXT("visible");
	}

	FString LexToString(EUnrealMcpToolRisk Risk)
	{
		switch (Risk)
		{
		case EUnrealMcpToolRisk::ReadOnly:
			return TEXT("read_only");
		case EUnrealMcpToolRisk::Medium:
			return TEXT("medium");
		case EUnrealMcpToolRisk::High:
			return TEXT("high");
		case EUnrealMcpToolRisk::Critical:
			return TEXT("critical");
		case EUnrealMcpToolRisk::Low:
		default:
			return TEXT("low");
		}
	}

	FString LexToString(EUnrealMcpToolTestCoverage TestCoverage)
	{
		switch (TestCoverage)
		{
		case EUnrealMcpToolTestCoverage::Core:
			return TEXT("core");
		case EUnrealMcpToolTestCoverage::Category:
			return TEXT("category");
		case EUnrealMcpToolTestCoverage::Manual:
			return TEXT("manual");
		case EUnrealMcpToolTestCoverage::External:
			return TEXT("external");
		case EUnrealMcpToolTestCoverage::Missing:
		default:
			return TEXT("missing");
		}
	}
}
