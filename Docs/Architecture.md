# Unreal MCP Architecture

## Runtime Shape

The plugin runs inside Unreal Editor and exposes a local HTTP JSON-RPC MCP endpoint. The in-editor Chat panel uses the same tool handlers as external MCP clients.

Core layers:

- `FUnrealMcpModule`: thin module lifecycle shell for startup/shutdown, the skill activity ticker, assistant turn creation, tab/menu registration, and `IMPLEMENT_MODULE`.
- Split implementation files own protocol routing, tool dispatch, tool categories, UI, registry, execution guards, verifiers, and self-extension workflows. New tool work should happen in the relevant category file, not in `UnrealMcpModule.cpp`.
- `UnrealMcpToolDescriptor` plus `UnrealMcpToolRegistrar`: code-owned default descriptors for tool name, title, description, category, handler, source file, risk, side effects, docs, and test coverage.
- `UnrealMcpToolRegistry`: reviewed JSON overrides for visibility, handler aliases, risk policy, owners, docs, dry-run support, and test coverage.
- `UnrealMcpToolHandlerRegistry`: explicit handler registration view used by audit, registry validation, and dispatch. It is derived from the combined descriptor-plus-JSON ToolRegistry, so handler visibility, aliases, categories, and policy metadata stay on the same self-extension path.
- `UnrealMcpToolExecutionGuard` plus `UnrealMcp*OutcomeVerifier`: shared execution checks with category-specific state verification for Blueprint, Widget, Actor, Memory, Skill, Scaffold, and Self-extension tools.
- Precision tools: `unreal.preview_change_plan`, `unreal.capture_project_snapshot`, `unreal.diff_project_snapshot`, `unreal.verify_task_outcome`, `unreal.mcp_classify_error`, Blueprint graph inspectors, and Widget tree dumping give Chat a read-back loop before and after edits.
- `Tools/unreal_mcp_supervisor.py`: external process for restart-aware pipeline automation.
- `Tools/UnrealMcpSupervisorTemplates`: versioned macOS/Windows supervisor launcher templates with placeholders instead of machine-specific paths.
- `Tools/UnrealMcpCodexBridge`: Bun daemon for Plan B Codex Desktop/App Server integration. It spawns `codex app-server` on a temporary Unix socket, connects through Codex's WebSocket-over-UDS App Server transport, and exposes a small UE-facing WebSocket protocol on `ws://127.0.0.1:8766/uevolve`.
- `Saved/UnrealMcp/ActivityLog`: local JSONL activity stream used to distill repeatable workflows into skill drafts.
- `Schemas/UnrealMcpExtensionManifest.schema.json`: versioned contract for source apply manifests.
- `Tools/UnrealMcpToolRegistry/schema.json`: versioned contract for explicit tool metadata under `Tools/UnrealMcpToolRegistry/tools.json`.
- `Saved/UnrealMcp`: local runtime state, memory, manifests, backups, generated tests, and logs.

## Editor UI Surfaces

- `SUnrealMcpChatPanel`: conversational command and AI surface.
- `SUnrealMcpWorkbenchPanel`: thin self-extension console over existing MCP tools. It should not own self-extension business logic; it delegates to `ExecuteToolFromEditorUI` so Chat, HTTP MCP, tests, and Workbench continue sharing the same backend behavior.
- `Tools/UnrealMcpSkills`: project-local skill instructions.

## External AI Bridge

The Codex bridge is intentionally outside `Plugins/UnrealMcp` for P7.A. The UE
plugin does not consume it yet; P7.B will add the UE-side client.

Bridge runtime shape:

- Spawned process: `codex app-server --listen unix://<temp>/codex.sock`.
- App Server transport: HTTP WebSocket upgrade over Unix domain socket, then one
  Codex JSON-RPC object per WebSocket text frame. The app-server protocol does
  not use the standard `jsonrpc:"2.0"` field.
- UE-facing transport: WebSocket on `ws://127.0.0.1:8766/uevolve`.
- Model defaults: `gpt-5.5` with reasoning effort `xhigh`.
- Approval default: reject file changes, command execution, permission requests,
  MCP elicitations, and tool user-input requests so Codex remains text-only.
- Supervision: if the spawned app-server exits, bridge health becomes `failed`;
  V1 does not auto-restart it.

## Module Split Status

`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpModule.cpp` has been reduced to a thin lifecycle entrypoint. The previous multi-thousand-line bottleneck has been split into category and infrastructure files so unrelated tool work can proceed with far fewer merge conflicts.

## Target Module Layout

Recommended split:

- `Private/Core`: MCP protocol helpers, JSON-RPC response helpers, schema utilities.
- `Private/ToolRegistry`: tool metadata, visibility, handler aliases, policy metadata.
- `Private/ToolHandlers`: first-class handler registration metadata.
- `Private/Execution`: shared execution guard plus category-specific preflight/postcheck verifiers.
- `Private/Tools/SelfExtension`: self-extension workbench, pipeline status, audit/schema/patch-fragment helpers, MCP test execution, and extension pipeline helpers. Self-extension dispatch now lives in `UnrealMcpSelfExtensionTools.cpp`; module-private orchestration methods are invoked through explicit callbacks.
- `Private/Tools/Editor`: status, logs, maps, assets, PIE, console, Python, Content Browser focus, map/asset opening, and save-dirty-packages. These editor tools now live in `UnrealMcpEditorTools.cpp`.
- `Private/Tools/Actors`: actor selection, transforms, spawning, layout, batch edits. Actor query/selection, basic write tools, batch edits, point-light edits, static-mesh actor configuration, actor layout tools, and spawn tools now live in `UnrealMcpActorTools.cpp`.
- `Private/Tools/Blueprint`: Blueprint class and graph editing in `UnrealMcpBlueprintTools.cpp`, with read-only graph inspection through `unreal.bp_list_graph_nodes` and `unreal.bp_trace_pin_connections`, plus outcome verification in `UnrealMcpBlueprintOutcomeVerifier.cpp`.
- `Private/Tools/Widget`: Widget Blueprint hierarchy, layout, event binding, template helpers, and read-only hierarchy inspection through `unreal.widget_dump_tree` in `UnrealMcpWidgetTools.cpp`, with outcome verification in `UnrealMcpWidgetOutcomeVerifier.cpp`.
- `Private/Tools/Scaffold`: `scaffold_mcp_tool` for MCP self-extension scaffolds plus legacy-hidden gameplay/demo scaffolds retained for direct compatibility in `UnrealMcpScaffoldTools.cpp`.
- `Private/Tools/SelfExtension`: validate, apply, build, test, audit, rollback, pipeline.
- `Private/Tools/Memory`: project memory CRUD in `UnrealMcpMemoryTools.cpp`.
- `Private/Tools/Skills`: project skill discovery, application, local activity recording, and skill distillation. Skill dispatch now lives in `UnrealMcpSkillTools.cpp`; promote still receives its extension-lock behavior through an explicit module callback.
- `Private/UI`: Chat panel and future Workbench panel.

## Tool Descriptor and Registry Role

Tool metadata has two layers:

- Code descriptors are the default source of truth for tool identity. A tool can be added to `UnrealMcpToolRegistrar.cpp` with an `FUnrealMcpToolDescriptor` and a fixed OpenAI-compatible input schema, then exposed through `AppendRegisteredToolDefinitions`.
- The JSON registry is a reviewed override layer. `Tools/UnrealMcpToolRegistry/tools.json` can override exposure, handler aliases, risk policy, owner, docs, and test metadata without editing the descriptor code. This keeps fast local C++ registration separate from slower policy review.

The combined descriptor-plus-registry view answers:

- Is this tool AI-facing?
- Which handler executes it?
- Which category owns it?
- Is it read-only or write-capable?
- Does it support dry run, preflight, or postcheck?
- Does it require a lock, build, restart, project memory, or external process?
- Which owner/docs/test coverage are attached to it?
- Is it deprecated or legacy-hidden?

The descriptor registry is loaded from C++ first. Then the JSON registry is loaded from `Tools/UnrealMcpToolRegistry/tools.json` when present,
then from the plugin fallback at `Plugins/UnrealMcp/Resources/ToolRegistry/tools.json`.
If no JSON file is available, descriptor-backed entries remain available. If neither descriptors nor JSON cover a tool, the plugin falls back to a tiny built-in compatibility registry and treats unregistered tools conservatively.

The first descriptor-backed migration covers low-risk/read-only tools:

- `unreal.editor_status`
- `unreal.list_maps`
- `unreal.list_selected_assets`
- `unreal.list_selected_actors`
- `unreal.mcp_tool_audit`
- `unreal.mcp_workbench_status`

New tools should move through this path unless there is a deliberate compatibility reason not to:

1. Add `FUnrealMcpToolDescriptor` plus fixed schema in `UnrealMcpToolRegistrar.cpp`.
2. Add the handler implementation in the relevant category file. The handler registry is derived from the descriptor/JSON ToolRegistry; do not add a separate hand-written handler-map entry.
3. Add reviewed policy override in `Tools/UnrealMcpToolRegistry/tools.json` when the default descriptor metadata is not enough.
4. Validate with `python3 Tools/validate_tool_registry.py`, build, restart Editor, run audit, and run the relevant test suite.

The policy object is attached to `tools/list` entries and reused by audit/workbench
status output:

- `riskLevel`
- `requiresWrite`
- `requiresBuild`
- `requiresExternalProcess`
- `requiresRestart`
- `requiresProjectMemory`
- `requiresLock`
- `dryRunSupport`
- `preflightSupport`
- `postcheckSupport`
- `testCoverage`
- `owner`
- `docsPath`

`Tools/validate_tool_registry.py` provides an editor-independent check for the registry schema file, required metadata, duplicate names, known categories, documentation files, registry-derived handler map coverage, write-tool execution-check coverage, committed test fixture coverage, and exact mirror parity with the plugin resource registry.

## Data and State

Local runtime state remains under `Saved/UnrealMcp` and is ignored by Git.

Team-shared planning and policy state should live under `Docs/` or a future `.unrealmcp/` directory so it can be reviewed and versioned.

Activity recording is intentionally local-first: JSONL events, distilled drafts, build logs, and transient test output stay under `Saved/UnrealMcp`. A draft only becomes team-shared when `unreal.skill_promote_draft` writes it into `Tools/UnrealMcpSkills`.

## Execution Verification

Write-capable tools receive structured execution guard metadata. The preflight object is built before the tool handler runs, then attached to the final result together with postcheck evidence:

- `policy`: explicit ToolRegistry risk and side-effect metadata.
- `preflight`: expected mutation areas plus category-specific readiness evidence when available.
- `postcheck`: generic success metadata or category-specific state verification when available.

Blueprint verifiers load the target Blueprint, inspect function/event graphs, member variables, node GUIDs, pin links, and pin defaults. Widget verifiers load the Widget Blueprint and inspect the WidgetTree, variable exposure, slots, and event binding evidence. Actor verifiers inspect the current editor world, selection state, spawned actors, reported transforms, and static mesh assignments. Workflow verifiers inspect project memory keys, skill draft/promote files, scaffold output folders/files, source apply manifests, rollback flags, build logs, and test/pipeline success fields.

## Precision Loop

For non-trivial Chat work, prefer this loop:

1. `unreal.preview_change_plan` turns the natural-language goal into likely tools, risk, backup, and verification steps.
2. `unreal.capture_project_snapshot` records the before state under `Saved/UnrealMcp/ProjectSnapshots`.
3. Category tools mutate the project.
4. Blueprint and Widget read-back tools inspect graph nodes, pin links, and widget trees.
5. `unreal.capture_project_snapshot` records the after state.
6. `unreal.diff_project_snapshot` compares objective state changes.
7. `unreal.verify_task_outcome` turns the plan, tool visibility, snapshot diff, and textual evidence into a pass/fail result.
8. `unreal.mcp_classify_error` categorizes failures into UBT, MCP protocol, schema, UE Python, HTTP endpoint, OpenAI API, or editor-state issues with next-step suggestions.

Automated happy-path tests use `unreal.mcp_prepare_test_sandbox` to create or reset `/Game/__UEvolve*` disposable content before writing test assets. The same tool can also remove level actors whose labels start with `UEvolveMcpTest_`, which gives Actor write tools a repeatable sandbox without touching unrelated user actors.
