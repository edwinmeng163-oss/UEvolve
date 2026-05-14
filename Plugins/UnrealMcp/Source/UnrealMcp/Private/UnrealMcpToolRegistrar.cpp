#include "UnrealMcpToolRegistrar.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UnrealMcp
{
	TSharedPtr<FJsonObject> MakeObjectSchema();
	TSharedPtr<FJsonObject> MakeStringProperty(const FString& Description, const FString& DefaultValue);
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
					TEXT("unreal.editor.engine_version"),
					TEXT("Editor Engine Version"),
					TEXT("Returns the running Unreal Editor engine version as structured major, minor, patch, and version_string fields."),
					TEXT("editor"),
					TEXT("UnrealMcpEditorEngineVersionTool.cpp")),
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
				Properties->SetObjectField(TEXT("blueprintPath"), MakeStringProperty(TEXT("Blueprint asset path to inspect."), FString()));
				Properties->SetObjectField(TEXT("graphName"), MakeStringProperty(TEXT("Optional graph name. If omitted, all standard Blueprint graphs are listed."), FString()));
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
				Properties->SetObjectField(TEXT("blueprintPath"), MakeStringProperty(TEXT("Blueprint asset path to inspect."), FString()));
				Properties->SetObjectField(TEXT("graphName"), MakeStringProperty(TEXT("Graph name containing the node."), TEXT("EventGraph")));
				Properties->SetObjectField(TEXT("nodeGuid"), MakeStringProperty(TEXT("Node GUID to inspect."), FString()));
				Properties->SetObjectField(TEXT("pinName"), MakeStringProperty(TEXT("Optional pin name. If omitted, all node pins are traced."), FString()));
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
			Properties->SetObjectField(TEXT("widgetBlueprintPath"), MakeStringProperty(TEXT("Widget Blueprint asset path to inspect."), FString()));
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

		void RegisterScaffoldMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("recipeName"), MakeStringProperty(TEXT("Recipe name: first_person_ground_character, widget_hud, or mcp_self_extension_pipeline."), TEXT("first_person_ground_character")));
			Properties->SetObjectField(TEXT("rootPath"), MakeStringProperty(TEXT("Content Browser root used in generated example tool calls."), TEXT("/Game/UEvolveRecipes")));
			Properties->SetObjectField(TEXT("taskName"), MakeStringProperty(TEXT("Optional human-readable task label to store with the recipe."), FString()));
			Properties->SetObjectField(TEXT("writeMemory"), MakeBoolProperty(TEXT("Whether to write this recipe into project memory for Chat continuation."), true));
			Properties->SetObjectField(TEXT("memoryKey"), MakeStringProperty(TEXT("Project memory key to write when writeMemory=true."), TEXT("chat.active_task")));
			Properties->SetObjectField(TEXT("includeToolCalls"), MakeBoolProperty(TEXT("Whether to include example tool-call JSON for the recipe."), true));
			TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
			Schema->SetObjectField(TEXT("properties"), Properties);

			FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
				TEXT("unreal.scaffold_recipe"),
				TEXT("Scaffold High-Level Recipe"),
				TEXT("Prepares a bounded high-level editor recipe with ordered tools, verification gates, and optional chat.active_task memory handoff."),
				TEXT("scaffold"),
				TEXT("UnrealMcpScaffoldTools.cpp"),
				EUnrealMcpToolRisk::Low);
			Descriptor.bRequiresWrite = true;
			Descriptor.bRequiresProjectMemory = true;
			Descriptor.bPreflightSupport = true;
			Descriptor.bPostcheckSupport = true;
			Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
			Descriptor.Reason = TEXT("Descriptor: high-level recipe scaffold that writes optional continuation memory and reduces long-loop tool exploration.");
			Registrar.Add(Descriptor, Schema);
		}

		void RegisterSkillSessionMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			TSharedPtr<FJsonObject> LabelProperty = MakeStringProperty(TEXT("User-set label for the current launch session; empty clears."), FString());
			LabelProperty->SetNumberField(TEXT("maxLength"), 2000);

			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("label"), LabelProperty);

			TArray<TSharedPtr<FJsonValue>> Required;
			TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
			Schema->SetObjectField(TEXT("properties"), Properties);
			Schema->SetArrayField(TEXT("required"), Required);

			FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
				TEXT("unreal.chat_label_active_task"),
				TEXT("Label Active Chat Task"),
				TEXT("Sets or clears a user-visible label for subsequent launch-session ActivityLog events."),
				TEXT("skills"),
				TEXT("UnrealMcpSkillTools.cpp"),
				EUnrealMcpToolRisk::Low);
			Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
			Descriptor.Reason = TEXT("Descriptor: low-risk launch-session label setter used to tag subsequent ActivityLog events.");
			Registrar.Add(Descriptor, Schema);
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

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("toolName"), MakeStringProperty(TEXT("MCP tool name to export. Portable exports require a matching scaffold under Tools/UnrealMcpToolScaffolds or Saved/UnrealMcp/TestScaffolds."), FString()));
				Properties->SetObjectField(TEXT("version"), MakeStringProperty(TEXT("Optional package version suffix. Defaults to a UTC timestamp."), FString()));
				Properties->SetObjectField(TEXT("packagePath"), MakeStringProperty(TEXT("Optional project-relative package zip path. Defaults to Saved/UnrealMcp/Packages/<toolName>-<version>.zip."), FString()));
				Properties->SetObjectField(TEXT("scaffoldDir"), MakeStringProperty(TEXT("Deprecated compatibility field. Portable exports resolve the reviewed scaffold roots automatically."), FString()));
				Properties->SetObjectField(TEXT("outputRoot"), MakeStringProperty(TEXT("Deprecated compatibility field. Portable exports resolve Tools/UnrealMcpToolScaffolds and Saved/UnrealMcp/TestScaffolds automatically."), TEXT("Tools/UnrealMcpToolScaffolds")));
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Preview package manifest and entries without writing the zip."), true));
				Properties->SetObjectField(TEXT("allowRegistryOnly"), MakeBoolProperty(TEXT("Expert mode: allow exporting a registry-only package when no portable scaffold exists. Import refuses this package unless acceptRegistryOnly=true."), false));
				TArray<TSharedPtr<FJsonValue>> Required;
				Required.Add(MakeShared<FJsonValueString>(TEXT("toolName")));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Schema->SetArrayField(TEXT("required"), Required);

				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.tools.export_package"),
					TEXT("Export Tool Package"),
					TEXT("Exports a scaffold-backed MCP tool into a portable zip package with a hashed manifest. Expert mode allowRegistryOnly=true emits a registry-only package that is not portable and is refused by import unless explicitly accepted."),
					TEXT("self-extension"),
					TEXT("UnrealMcpToolPackager.cpp"),
					EUnrealMcpToolRisk::Medium);
				Descriptor.bRequiresWrite = true;
				Descriptor.bDryRunSupport = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
				Descriptor.DocsPath = TEXT("Docs/SelfExtensionPipeline.md#tool-sharing");
				Descriptor.Reason = TEXT("Descriptor: writes reviewed tool-sharing packages under Saved/UnrealMcp/Packages without mutating source files.");
				Registrar.Add(Descriptor, Schema);
			}

			{
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());

				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.tools.list_exportable"),
					TEXT("List Exportable Tools"),
					TEXT("Lists scaffold-backed MCP tools under Tools/UnrealMcpToolScaffolds and Saved/UnrealMcp/TestScaffolds that can be exported as portable tool packages."),
					TEXT("self-extension"),
					TEXT("UnrealMcpToolPackager.cpp"),
					EUnrealMcpToolRisk::ReadOnly);
				Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
				Descriptor.DocsPath = TEXT("Docs/SelfExtensionPipeline.md#tool-sharing");
				Descriptor.Reason = TEXT("Descriptor: read-only package picker helper for scaffold-backed user-generated tools.");
				Registrar.Add(Descriptor, Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("packagePath"), MakeStringProperty(TEXT("Project-relative or absolute tool package zip path."), FString()));
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Validate and preview the import plan without mutating registry, scaffold, or test files."), true));
				Properties->SetObjectField(TEXT("overwriteScaffold"), MakeBoolProperty(TEXT("Allow scaffold files from the package to overwrite existing scaffold files."), false));
				Properties->SetObjectField(TEXT("acceptRegistryOnly"), MakeBoolProperty(TEXT("Expert mode: allow importing a registry-only package whose handler implementation must already exist locally."), false));
				Properties->SetObjectField(TEXT("skipLock"), MakeBoolProperty(TEXT("Testing-only escape hatch for in-process test execution; normal Chat use should leave this false."), false));
				TArray<TSharedPtr<FJsonValue>> Required;
				Required.Add(MakeShared<FJsonValueString>(TEXT("packagePath")));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Schema->SetArrayField(TEXT("required"), Required);

				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.tools.import_package"),
					TEXT("Import Tool Package"),
					TEXT("Validates a tool package manifest and imports scaffold/test files plus a deduplicated ToolRegistry entry after dry-run review."),
					TEXT("self-extension"),
					TEXT("UnrealMcpToolPackager.cpp"),
					EUnrealMcpToolRisk::High);
				Descriptor.bRequiresWrite = true;
				Descriptor.bRequiresLock = true;
				Descriptor.bDryRunSupport = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
				Descriptor.DocsPath = TEXT("Docs/SelfExtensionPipeline.md#tool-sharing");
				Descriptor.Reason = TEXT("Descriptor: mutates tool-sharing registry/scaffold/test assets only after package hash validation and extension-session locking.");
				Registrar.Add(Descriptor, Schema);
			}

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
				Properties->SetObjectField(TEXT("sourceRoot"), MakeStringProperty(TEXT("Optional root containing fetched knowledge sources. Defaults to Saved/UnrealMcp/KnowledgeSources."), FString()));
				Properties->SetObjectField(TEXT("indexRoot"), MakeStringProperty(TEXT("Optional output directory for the generated KnowledgeCard index. Defaults to Saved/UnrealMcp/KnowledgeIndex."), FString()));
				Properties->SetObjectField(TEXT("includeOfficialDocs"), MakeBoolProperty(TEXT("Include fetched official documentation documents.jsonl files."), true));
				Properties->SetObjectField(TEXT("includeVersionedDocs"), MakeBoolProperty(TEXT("Include versioned README/Docs markdown files."), true));
				Properties->SetObjectField(TEXT("includeToolRegistry"), MakeBoolProperty(TEXT("Include visible ToolRegistry entries as searchable tool cards."), true));
				Properties->SetObjectField(TEXT("skipLowContent"), MakeBoolProperty(TEXT("Skip source rows flagged as low-content by the docs fetcher."), true));
				Properties->SetObjectField(TEXT("maxCards"), MakeNumberProperty(TEXT("Maximum KnowledgeCards to write."), 2000.0));
				Properties->SetObjectField(TEXT("maxChunkChars"), MakeNumberProperty(TEXT("Maximum text characters per card chunk."), 1800.0));
				Properties->SetObjectField(TEXT("chunkOverlapChars"), MakeNumberProperty(TEXT("Overlapping characters between adjacent chunks."), 160.0));
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Preview source/card counts without writing index files."), false));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.knowledge_index_refresh"),
					TEXT("Refresh Knowledge Index"),
					TEXT("Builds a local KnowledgeCard JSONL index from fetched official docs, versioned docs, and visible ToolRegistry entries."),
					TEXT("self-extension"),
					TEXT("UnrealMcpKnowledgeTools.cpp"),
					EUnrealMcpToolRisk::Low);
				Descriptor.bRequiresWrite = true;
				Descriptor.bDryRunSupport = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
				Descriptor.Reason = TEXT("Descriptor: writes local Saved/UnrealMcp KnowledgeCard indexes so Chat can retrieve docs and tool guidance without expanding prompt context.");
				Registrar.Add(Descriptor, Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("query"), MakeStringProperty(TEXT("Search query for local KnowledgeCards."), FString()));
				Properties->SetObjectField(TEXT("categories"), MakeStringArrayProperty(TEXT("Optional KnowledgeCard categories to include.")));
				Properties->SetObjectField(TEXT("indexRoot"), MakeStringProperty(TEXT("Optional index directory. Defaults to Saved/UnrealMcp/KnowledgeIndex."), FString()));
				Properties->SetObjectField(TEXT("limit"), MakeNumberProperty(TEXT("Maximum search results."), 8.0));
				Properties->SetObjectField(TEXT("maxExcerptChars"), MakeNumberProperty(TEXT("Maximum excerpt characters per result."), 420.0));
				Properties->SetObjectField(TEXT("includeText"), MakeBoolProperty(TEXT("Include full card text in results. Off by default to keep Chat context compact."), false));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.knowledge_search"),
						TEXT("Search Knowledge Index"),
						TEXT("Searches the local KnowledgeCard index and returns compact source-linked cards for planning, tool choice, and verification."),
						TEXT("self-extension"),
						TEXT("UnrealMcpKnowledgeTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Natural-language task to map to existing tools and workflows."), FString()));
				Properties->SetObjectField(TEXT("riskMax"), MakeStringProperty(TEXT("Maximum risk to recommend: read_only, low, medium, high, or critical."), TEXT("critical")));
				Properties->SetObjectField(TEXT("limit"), MakeNumberProperty(TEXT("Maximum tool recommendations."), 8.0));
				Properties->SetObjectField(TEXT("includeKnowledge"), MakeBoolProperty(TEXT("Include top matching KnowledgeCards when the index exists."), true));
				Properties->SetObjectField(TEXT("includeWorkflowDraft"), MakeBoolProperty(TEXT("Include a lightweight workflow draft using preview/search/tool/verify gates."), true));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.tool_recommend"),
						TEXT("Recommend MCP Tools"),
						TEXT("Recommends existing MCP tools and a safe workflow draft for a task using ToolRegistry policy plus optional local KnowledgeCards."),
						TEXT("self-extension"),
						TEXT("UnrealMcpKnowledgeTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Natural-language task to analyze for tool coverage gaps."), FString()));
				Properties->SetObjectField(TEXT("riskMax"), MakeStringProperty(TEXT("Maximum existing-tool risk to consider: read_only, low, medium, high, or critical."), TEXT("critical")));
				Properties->SetObjectField(TEXT("limit"), MakeNumberProperty(TEXT("Maximum existing tool recommendations to include."), 6.0));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.tool_gap_analyze"),
						TEXT("Analyze MCP Tool Gap"),
						TEXT("Decides whether a task should use existing tools, compose a workflow, or scaffold a new descriptor-first MCP tool, with schema/test/pipeline hints."),
						TEXT("self-extension"),
						TEXT("UnrealMcpKnowledgeTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Natural-language task to convert into a recommended MCP workflow draft."), FString()));
				Properties->SetObjectField(TEXT("riskMax"), MakeStringProperty(TEXT("Maximum tool risk to include in recommendations: read_only, low, medium, high, or critical."), TEXT("critical")));
				Properties->SetObjectField(TEXT("limit"), MakeNumberProperty(TEXT("Maximum recommended task-specific tools to include as skipped placeholder steps."), 5.0));
				Properties->SetObjectField(TEXT("includeKnowledge"), MakeBoolProperty(TEXT("Include knowledge_search as an early workflow step."), true));
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Set the generated workflow_run draft dryRun flag."), true));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.workflow_recommend"),
						TEXT("Recommend MCP Workflow"),
						TEXT("Generates a safe workflow_run draft from a task using KnowledgeCards, ToolRegistry policy, gap analysis, snapshot gates, skipped placeholder tool steps, and final verification."),
						TEXT("self-extension"),
						TEXT("UnrealMcpKnowledgeTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("evalPath"), MakeStringProperty(TEXT("Project-local eval JSON file or directory. Defaults to Tools/UnrealMcpKnowledge/Evals."), FString()));
				Properties->SetObjectField(TEXT("refreshIndex"), MakeBoolProperty(TEXT("Refresh the local KnowledgeCard index before running evals."), false));
				Properties->SetObjectField(TEXT("includeDetails"), MakeBoolProperty(TEXT("Include per-case structuredContent in the eval output."), true));
				Properties->SetObjectField(TEXT("limit"), MakeNumberProperty(TEXT("Search/recommendation limit used by each eval case."), 6.0));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				Registrar.Add(
					MakeDescriptor(
						TEXT("unreal.knowledge_eval_run"),
						TEXT("Run Knowledge Evals"),
						TEXT("Runs versioned local RAG eval cases against knowledge_search, tool_recommend, tool_gap_analyze, and workflow_recommend, returning pass rate and failure evidence."),
						TEXT("self-extension"),
						TEXT("UnrealMcpKnowledgeTools.cpp"),
						EUnrealMcpToolRisk::ReadOnly),
					Schema);
			}

			{
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Natural-language task to turn into a structured change plan."), FString()));
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
				Properties->SetObjectField(TEXT("snapshotName"), MakeStringProperty(TEXT("Optional snapshot name. Defaults to a UTC timestamp."), FString()));
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
				Properties->SetObjectField(TEXT("beforeSnapshotPath"), MakeStringProperty(TEXT("Before snapshot path. If omitted, the previous latest snapshot is used."), FString()));
				Properties->SetObjectField(TEXT("afterSnapshotPath"), MakeStringProperty(TEXT("After snapshot path. If omitted, the latest snapshot is used."), FString()));
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
				Properties->SetObjectField(TEXT("task"), MakeStringProperty(TEXT("Original task goal to verify."), FString()));
				Properties->SetObjectField(TEXT("beforeSnapshotPath"), MakeStringProperty(TEXT("Optional before snapshot path."), FString()));
				Properties->SetObjectField(TEXT("afterSnapshotPath"), MakeStringProperty(TEXT("Optional after snapshot path."), FString()));
				Properties->SetObjectField(TEXT("expectedChangedAreas"), MakeStringArrayProperty(TEXT("Areas expected to change: actors, assets, blueprints, widgets, memory, or skills.")));
				Properties->SetObjectField(TEXT("expectedTools"), MakeStringArrayProperty(TEXT("Tool names expected to exist in tools/list.")));
				Properties->SetObjectField(TEXT("evidenceText"), MakeStringProperty(TEXT("Optional tool output or summary text to inspect."), FString()));
				Properties->SetObjectField(TEXT("requiredEvidenceText"), MakeStringProperty(TEXT("Optional substring that must appear in evidenceText."), FString()));
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
				Properties->SetObjectField(TEXT("text"), MakeStringProperty(TEXT("Raw error text to classify."), FString()));
				Properties->SetObjectField(TEXT("logPath"), MakeStringProperty(TEXT("Optional project-local log file to tail and classify."), FString()));
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
				Properties->SetObjectField(TEXT("resetActors"), MakeBoolProperty(TEXT("Whether to delete level actors whose labels start with actorLabelPrefix."), false));
				Properties->SetObjectField(TEXT("actorLabelPrefix"), MakeStringProperty(TEXT("Safe actor label prefix for disposable actor tests. Must start with UEvolveMcpTest_ when resetActors=true."), TEXT("UEvolveMcpTest_")));
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Preview sandbox preparation without mutating assets."), false));
				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);
				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.mcp_prepare_test_sandbox"),
					TEXT("Prepare MCP Test Sandbox"),
					TEXT("Creates or resets constrained /Game/__UEvolve* asset and UEvolveMcpTest_* actor sandboxes for disposable happy-path tests."),
					TEXT("self-extension"),
					TEXT("UnrealMcpSelfExtensionPrecisionTools.cpp"),
					EUnrealMcpToolRisk::Medium);
				Descriptor.bRequiresWrite = true;
				Descriptor.bDryRunSupport = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Registrar.Add(Descriptor, Schema);
			}

			{
				TSharedPtr<FJsonObject> StepProperties = MakeShared<FJsonObject>();
				StepProperties->SetObjectField(TEXT("name"), MakeStringProperty(TEXT("Human-readable step label."), FString()));
				StepProperties->SetObjectField(TEXT("tool"), MakeStringProperty(TEXT("MCP tool name to call, for example unreal.editor_status."), FString()));
				StepProperties->SetObjectField(TEXT("argumentsJson"), MakeStringProperty(TEXT("JSON object string used as the step tool arguments. Use {} for no arguments."), TEXT("{}")));
				StepProperties->SetObjectField(TEXT("skip"), MakeBoolProperty(TEXT("Whether to skip this step."), false));
				StepProperties->SetObjectField(TEXT("expectError"), MakeBoolProperty(TEXT("Whether the step is expected to return a tool error."), false));
				StepProperties->SetObjectField(TEXT("continueOnError"), MakeBoolProperty(TEXT("Whether to continue the workflow if this step fails."), false));

				TSharedPtr<FJsonObject> StepSchema = MakeObjectSchema();
				StepSchema->SetObjectField(TEXT("properties"), StepProperties);

				TSharedPtr<FJsonObject> StepsProperty = MakeShared<FJsonObject>();
				StepsProperty->SetStringField(TEXT("type"), TEXT("array"));
				StepsProperty->SetStringField(TEXT("description"), TEXT("Bounded list of workflow steps. Each step can call one registered MCP tool."));
				StepsProperty->SetObjectField(TEXT("items"), StepSchema);

				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				Properties->SetObjectField(TEXT("workflowName"), MakeStringProperty(TEXT("Human-readable workflow name."), TEXT("mcp_workflow")));
				Properties->SetObjectField(TEXT("workflowJson"), MakeStringProperty(TEXT("Optional full workflow JSON object string. Useful when step arguments are complex."), FString()));
				Properties->SetObjectField(TEXT("workflowPath"), MakeStringProperty(TEXT("Optional project-local workflow JSON file path. Ignored when workflowJson is provided."), FString()));
				Properties->SetObjectField(TEXT("steps"), StepsProperty);
				Properties->SetObjectField(TEXT("dryRun"), MakeBoolProperty(TEXT("Preview the workflow without executing step tools. Defaults to true for safety."), true));
				Properties->SetObjectField(TEXT("stopOnFailure"), MakeBoolProperty(TEXT("Stop at the first failed step unless the step has continueOnError=true."), true));
				Properties->SetObjectField(TEXT("maxSteps"), MakeNumberProperty(TEXT("Maximum number of steps allowed in this run, clamped to 1-100."), 20.0));
				Properties->SetObjectField(TEXT("writeMemory"), MakeBoolProperty(TEXT("Write workflow status to project memory for long-task continuation."), true));
				Properties->SetObjectField(TEXT("memoryKey"), MakeStringProperty(TEXT("Project memory key used when writeMemory=true."), TEXT("chat.active_task")));
				Properties->SetObjectField(TEXT("allowHighRisk"), MakeBoolProperty(TEXT("Allow high-risk step tools to execute when dryRun=false."), false));
				Properties->SetObjectField(TEXT("allowCritical"), MakeBoolProperty(TEXT("Allow critical-risk step tools to execute when dryRun=false."), false));
				Properties->SetObjectField(TEXT("includeStepStructuredContent"), MakeBoolProperty(TEXT("Include each step structuredContent in the workflow result. Off by default to keep context compact."), false));
				Properties->SetObjectField(TEXT("maxResultChars"), MakeNumberProperty(TEXT("Maximum text preview characters retained per executed step."), 1200.0));

				TSharedPtr<FJsonObject> Schema = MakeObjectSchema();
				Schema->SetObjectField(TEXT("properties"), Properties);

				FUnrealMcpToolDescriptor Descriptor = MakeDescriptor(
					TEXT("unreal.workflow_run"),
					TEXT("Run MCP Workflow"),
					TEXT("Runs a bounded, policy-checked sequence of MCP tool calls with dry-run planning, risk gates, failure pause, and project-memory handoff."),
					TEXT("self-extension"),
					TEXT("UnrealMcpWorkflowTools.cpp"),
					EUnrealMcpToolRisk::High);
				Descriptor.bRequiresWrite = true;
				Descriptor.bRequiresProjectMemory = true;
				Descriptor.bDryRunSupport = true;
				Descriptor.bPreflightSupport = true;
				Descriptor.bPostcheckSupport = true;
				Descriptor.TestCoverage = EUnrealMcpToolTestCoverage::Category;
				Descriptor.Reason = TEXT("Descriptor: generic high-level composition executor for safe MCP tool orchestration.");
				Registrar.Add(Descriptor, Schema);
			}
		}

		void RegisterAllMcpToolDescriptors(FUnrealMcpToolRegistrar& Registrar)
		{
			RegisterEditorMcpToolDescriptors(Registrar);
			RegisterActorMcpToolDescriptors(Registrar);
			RegisterBlueprintInspectorMcpToolDescriptors(Registrar);
			RegisterWidgetInspectorMcpToolDescriptors(Registrar);
			RegisterScaffoldMcpToolDescriptors(Registrar);
			RegisterSkillSessionMcpToolDescriptors(Registrar);
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
