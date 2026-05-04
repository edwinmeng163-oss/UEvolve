# Unreal MCP Architecture

## Runtime Shape

The plugin runs inside Unreal Editor and exposes a local HTTP JSON-RPC MCP endpoint. The in-editor Chat panel uses the same tool handlers as external MCP clients.

Core layers:

- `FUnrealMcpModule`: thin module lifecycle shell for startup/shutdown, the skill activity ticker, assistant turn creation, tab/menu registration, and `IMPLEMENT_MODULE`.
- Split implementation files own protocol routing, tool dispatch, tool categories, UI, registry, execution guards, verifiers, and self-extension workflows. New tool work should happen in the relevant category file, not in `UnrealMcpModule.cpp`.
- `UnrealMcpToolRegistry`: explicit metadata for visibility, handler aliases, risk policy, owners, docs, dry-run support, and test coverage.
- `UnrealMcpToolHandlerRegistry`: explicit handler registration map used by audit and registry validation instead of source-text scanning.
- `UnrealMcpToolExecutionGuard` plus `UnrealMcp*OutcomeVerifier`: shared execution checks with category-specific state verification for Blueprint, Widget, Actor, Memory, Skill, Scaffold, and Self-extension tools.
- `Tools/unreal_mcp_supervisor.py`: external process for restart-aware pipeline automation.
- `Tools/UnrealMcpSupervisorTemplates`: versioned macOS/Windows supervisor launcher templates with placeholders instead of machine-specific paths.
- `Saved/UnrealMcp/ActivityLog`: local JSONL activity stream used to distill repeatable workflows into skill drafts.
- `Schemas/UnrealMcpExtensionManifest.schema.json`: versioned contract for source apply manifests.
- `Tools/UnrealMcpToolRegistry/schema.json`: versioned contract for explicit tool metadata under `Tools/UnrealMcpToolRegistry/tools.json`.
- `Saved/UnrealMcp`: local runtime state, memory, manifests, backups, generated tests, and logs.

## Editor UI Surfaces

- `SUnrealMcpChatPanel`: conversational command and AI surface.
- `SUnrealMcpWorkbenchPanel`: thin self-extension console over existing MCP tools. It should not own self-extension business logic; it delegates to `ExecuteToolFromEditorUI` so Chat, HTTP MCP, tests, and Workbench continue sharing the same backend behavior.
- `Tools/UnrealMcpSkills`: project-local skill instructions.

## Module Split Status

`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpModule.cpp` has been reduced to a thin lifecycle entrypoint. The previous multi-thousand-line bottleneck has been split into category and infrastructure files so unrelated tool work can proceed with far fewer merge conflicts.

## Target Module Layout

Recommended split:

- `Private/Core`: MCP protocol helpers, JSON-RPC response helpers, schema utilities.
- `Private/ToolRegistry`: tool metadata, visibility, handler aliases, policy metadata.
- `Private/ToolHandlers`: first-class handler registration metadata.
- `Private/Execution`: shared execution guard plus category-specific preflight/postcheck verifiers.
- `Private/Tools/SelfExtension`: self-extension workbench, pipeline status, audit/schema/snippet helpers, MCP test execution, and extension pipeline helpers. Self-extension dispatch now lives in `UnrealMcpSelfExtensionTools.cpp`; module-private orchestration methods are invoked through explicit callbacks.
- `Private/Tools/Editor`: status, logs, maps, assets, PIE, console, Python, Content Browser focus, map/asset opening, and save-dirty-packages. These editor tools now live in `UnrealMcpEditorTools.cpp`.
- `Private/Tools/Actors`: actor selection, transforms, spawning, layout, batch edits. Actor query/selection, basic write tools, batch edits, point-light edits, static-mesh actor configuration, actor layout tools, and spawn tools now live in `UnrealMcpActorTools.cpp`.
- `Private/Tools/Blueprint`: Blueprint class and graph editing in `UnrealMcpBlueprintTools.cpp`, with outcome verification in `UnrealMcpBlueprintOutcomeVerifier.cpp`.
- `Private/Tools/Widget`: Widget Blueprint hierarchy, layout, event binding, and template helpers in `UnrealMcpWidgetTools.cpp`, with outcome verification in `UnrealMcpWidgetOutcomeVerifier.cpp`.
- `Private/Tools/Scaffold`: gameplay scaffolds and MCP tool scaffolds in `UnrealMcpScaffoldTools.cpp`.
- `Private/Tools/SelfExtension`: validate, apply, build, test, audit, rollback, pipeline.
- `Private/Tools/Memory`: project memory CRUD in `UnrealMcpMemoryTools.cpp`.
- `Private/Tools/Skills`: project skill discovery, application, local activity recording, and skill distillation. Skill dispatch now lives in `UnrealMcpSkillTools.cpp`; promote still receives its extension-lock behavior through an explicit module callback.
- `Private/UI`: Chat panel and future Workbench panel.

## ToolRegistry Role

ToolRegistry is now the single metadata source for:

- Is this tool AI-facing?
- Which handler executes it?
- Which category owns it?
- Is it read-only or write-capable?
- Does it support dry run, preflight, or postcheck?
- Does it require a lock, build, restart, project memory, or external process?
- Which owner/docs/test coverage are attached to it?
- Is it deprecated or legacy-hidden?

The registry is loaded from `Tools/UnrealMcpToolRegistry/tools.json` when present,
then from the plugin fallback at `Plugins/UnrealMcp/Resources/ToolRegistry/tools.json`.
If neither file is available, the plugin falls back to a tiny built-in compatibility
registry and treats unregistered tools conservatively.

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

`Tools/validate_tool_registry.py` provides an editor-independent check for the registry schema file, required metadata, duplicate names, known categories, documentation files, handler map coverage, write-tool execution-check coverage, committed test fixture coverage, and exact mirror parity with the plugin resource registry.

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
