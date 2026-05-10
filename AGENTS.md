# UEvolve Agent Handoff

This file is the first-read briefing for any AI agent that opens this
repository. It summarizes the current project intent, structure, tool system,
self-extension workflow, RAG layer, testing strategy, and safe working rules.

## Project Identity

UEvolve is an Unreal Editor MCP self-extension workbench.

The main deliverable is the editor plugin at:

```text
Plugins/UnrealMcp
```

The plugin runs inside Unreal Editor, exposes a local MCP JSON-RPC endpoint,
adds an in-editor Chat panel, and adds a thin Workbench UI for self-extension
status, audit, tests, RAG, and pipeline controls.

Default local endpoint:

```text
http://127.0.0.1:8765/mcp
```

Editor UI entry points:

```text
Window > Unreal MCP Chat
Window > Unreal MCP Workbench
```

Current plugin metadata:

```text
Plugins/UnrealMcp/UnrealMcp.uplugin
FriendlyName: Unreal MCP
VersionName: 0.10.4
Type: Editor plugin
Required plugin: PythonScriptPlugin
```

The repository root contains `UEvolve.uproject` as the local development host.
`Examples/UEvolveExample` is optional validation/demo content.

## Product Goal

The user is building more than a normal Unreal automation plugin. The target is:

```text
An Unreal Editor MCP self-extension workbench that lets AI add new editor
automation capabilities under audit, dry run, backup, build, test, rollback,
RAG, and long-memory safeguards.
```

Core capabilities:

- AI can call Unreal Editor tools from Chat or external MCP clients.
- AI can inspect maps, assets, actors, Blueprint graphs, Widget trees, logs,
  tests, tool metadata, memory, skills, and local knowledge cards.
- AI can edit Blueprint, Widget, and Actor state through fixed-schema tools.
- AI can scaffold new MCP tools, validate schemas and C++ patch fragments, dry
  run source edits, apply with backup manifests, build, test, classify errors,
  and roll back.
- AI can use local RAG/tool recommendation to decide whether to compose existing
  tools or create a new one.
- AI can write project memory and pause long work instead of losing context at
  tool-loop limits.
- Users can distill repeated activity into project-local skills.

## Read Order For A New Agent

Start here:

1. `AGENTS.md`
2. `README.md`
3. `Plugins/UnrealMcp/README.md`
4. `Docs/Architecture.md`
5. `Docs/SelfExtensionPipeline.md`
6. `Docs/KnowledgeRag.md`
7. `Docs/SecurityModel.md`
8. `Tools/UnrealMcpToolRegistry/tools.json`
9. `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpToolRegistrar.cpp`
10. The category source file for the area you are changing.

If the task is about install/deployment:

```text
Docs/DeploymentTroubleshooting.md
Plugins/UnrealMcp/README.md
Tools/install_unrealmcp_to_project.py
```

If the task is about self-extension:

```text
Docs/SelfExtensionPipeline.md
Tools/UnrealMcpSkills/mcp-self-extension/SKILL.md
Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtension*.cpp
```

If the task is about RAG/tool recommendation:

```text
Docs/KnowledgeRag.md
Tools/UnrealMcpKnowledge/README.md
Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp
Tools/UnrealMcpKnowledge/Evals/core_rag_eval.json
```

## Documentation Freshness Rule

After every meaningful project change, update the AI-facing docs before handoff:

1. Update `AGENTS.md` when the project structure, tool-extension workflow,
   safety rules, build/test commands, RAG behavior, or current project status
   changes.
2. Update `README.md` when user-facing install, usage, feature coverage,
   deployment, or product positioning changes.
3. Update the focused doc under `Docs/` when the change belongs to a specific
   system such as architecture, self-extension, RAG, security, supervisor, or
   deployment troubleshooting.
4. If the change adds or changes tools, update ToolRegistry metadata, tests, and
   the relevant docs in the same patch.

Treat stale docs as a product bug. This project depends on fresh context so that
future AI agents can continue work without re-discovering the whole codebase.

## Repository Map

Important versioned paths:

```text
README.md
AGENTS.md
UEvolve.uproject
open_uevolve.command

Docs/
  Architecture.md
  Contributing.md
  DeploymentTroubleshooting.md
  KnowledgeRag.md
  ManifestSchema.md
  Roadmap.md
  SecurityModel.md
  SelfExtensionPipeline.md
  Supervisor.md
  ToolNaming.md
  UnrealTaskRecipes.md

Plugins/UnrealMcp/
  UnrealMcp.uplugin
  README.md
  Resources/
    Schemas/
    ToolRegistry/
  Source/UnrealMcp/
    UnrealMcp.Build.cs
    Public/
    Private/

Schemas/
  UnrealMcpExtensionManifest.schema.json
  UnrealMcpKnowledgeCard.schema.json
  UnrealMcpToolRegistry.schema.json

Tools/
  install_unrealmcp_to_project.py
  unreal_mcp_fetch_docs.py
  unreal_mcp_stdio_proxy.py
  unreal_mcp_supervisor.py
  validate_tool_registry.py
  UnrealMcpCodexBridge/
  UnrealMcpKnowledge/
  UnrealMcpSkills/
  UnrealMcpSupervisorTemplates/
  UnrealMcpTests/
  UnrealMcpToolRegistry/
```

Important local-only or generated paths:

```text
Saved/UnrealMcp/
Content/
Tools/UnrealMcpToolScaffolds/
Tools/UnrealMcpSupervisor/
Binaries/
Intermediate/
DerivedDataCache/
Plugins/*/Binaries/
Plugins/*/Intermediate/
```

Do not commit local runtime state, fetched docs caches, generated KnowledgeIndex
files, API keys, generated supervisor launchers, local test content, or
unreviewed scaffold drafts unless the user explicitly asks.

## Current High-Level Feature Set

Editor and inspection:

- `unreal.editor_status`
- `unreal.tail_log`
- `unreal.map_check`
- map, asset, selected asset, actor, and selected actor listing
- PIE start/stop
- console command execution
- Python command/file execution
- map/asset opening
- Content Browser sync
- save dirty packages

Actor tools:

- select actors
- set transforms
- spawn actors with fixed schemas
- spawn static mesh actors
- batch scale/tags/point-light/static-mesh configuration
- grid/circle layout
- destroy selected actors
- clear level environment

Blueprint tools:

- create Blueprint class
- compile one Blueprint or all Blueprints in a path
- add variables/functions/event/call/branch/foreach nodes
- connect pins
- set pin defaults
- arrange graph
- compile/save
- inspect graph nodes
- trace pin connections

Widget tools:

- add/remove widgets
- set widget properties
- set slot layout
- bind events
- bind Blueprint variables
- build templates
- dump Widget Blueprint tree

Scaffold and workflow:

- `unreal.scaffold_recipe`
- `unreal.workflow_run`
- legacy gameplay scaffold helpers retained but hidden from AI-facing tools
  where appropriate

Self-extension:

- schema validation
- C++ patch validation
- patch-fragment editing
- scaffold apply with dry run, backup, manifest, and idempotence checks
- build editor
- run one tool test or a suite
- generate tests
- pipeline orchestration/status
- audit
- rollback last or to manifest
- backup project state
- lock extension session
- classify errors
- compile-error fix plan
- supervisor installer

RAG and recommendation:

- `unreal.knowledge_index_refresh`
- `unreal.knowledge_search`
- `unreal.tool_recommend`
- `unreal.tool_gap_analyze`
- `unreal.workflow_recommend`
- `unreal.knowledge_eval_run`

Project memory:

- read/write/view/edit/delete local project memory under `Saved/UnrealMcp`.

Skills:

- list/read/apply project skills
- start/stop activity recording
- inspect activity status
- distill activity into skill drafts
- save/promote skill drafts

## Tool Registry Status

The explicit ToolRegistry is central. Do not bypass it.

Versioned registry:

```text
Tools/UnrealMcpToolRegistry/tools.json
```

Plugin fallback mirror:

```text
Plugins/UnrealMcp/Resources/ToolRegistry/tools.json
```

Registry schema:

```text
Tools/UnrealMcpToolRegistry/schema.json
Schemas/UnrealMcpToolRegistry.schema.json
```

At the time this file was written, the registry contained 114 entries across:

- actors
- blueprint
- editor
- memory
- scaffold
- self-extension
- skills
- widget

The visible count in a running editor can differ because legacy/hidden entries
and aliases are filtered. Always trust live output from:

```text
/tool unreal.mcp_workbench_status {}
/tool unreal.mcp_tool_audit {}
```

Every AI-facing tool should have:

- fixed OpenAI-compatible schema
- explicit category
- explicit handler name or registry-derived handler
- risk level
- write/build/external-process/restart/memory/lock flags
- dry-run/preflight/postcheck metadata where applicable
- owner
- docs path
- test coverage state

Validate registry changes with:

```bash
python3 Tools/validate_tool_registry.py
```

## C++ Architecture

`UnrealMcpModule.cpp` is intentionally thin. Do not add new tool logic there.

Current split:

```text
UnrealMcpModule.cpp
  module startup/shutdown and lifecycle only

UnrealMcpProtocol.cpp
  JSON-RPC/MCP protocol routing

UnrealMcpToolDefinitions.cpp
  tools/list construction and schema helpers

UnrealMcpToolDescriptor.h
UnrealMcpToolRegistrar.cpp/.h
  descriptor-first tool registration

UnrealMcpToolRegistry.cpp/.h
  combined descriptor + JSON registry policy

UnrealMcpToolHandlerRegistry.cpp/.h
  explicit handler/category/source-file view derived from registry

UnrealMcpToolDispatcher.cpp
  dispatch to category executors

UnrealMcpToolExecutionGuard.cpp/.h
  preflight/postcheck wrapper logic

UnrealMcp*OutcomeVerifier.cpp
  real state verification for actors, Blueprint, Widget, workflow

UnrealMcpEditorTools.cpp
UnrealMcpActorTools.cpp
UnrealMcpBlueprintTools.cpp
UnrealMcpWidgetTools.cpp
UnrealMcpScaffoldTools.cpp
UnrealMcpSelfExtension*.cpp
UnrealMcpMemoryTools.cpp
UnrealMcpSkillTools.cpp
UnrealMcpKnowledgeTools.cpp
UnrealMcpWorkflowTools.cpp
  category handlers

UnrealMcpChatPanel.cpp/.h
UnrealMcpWorkbenchPanel.cpp/.h
UnrealMcpEditorTabs.cpp
  Slate UI surfaces

UnrealMcpAssistantRun.cpp/.h
  AI Responses API interaction, tool loop, context compression, timeouts
```

The largest current files are still ChatPanel, ActorTools, KnowledgeTools,
ScaffoldTools, BlueprintTools, WidgetTools, ToolDefinitions, AssistantRun, and
several SelfExtension helpers. Prefer cautious single-category edits.

## How To Add A New MCP Tool

Prefer composition first:

1. Search/recommend existing tools with `unreal.tool_recommend`.
2. Search project knowledge with `unreal.knowledge_search`.
3. Consider `unreal.workflow_recommend` + `unreal.workflow_run`.
4. Only scaffold a new C++ MCP tool if there is a real capability gap.

Descriptor-first path for real new tools:

1. Add or generate a descriptor and fixed schema in
   `UnrealMcpToolRegistrar.cpp`.
2. Add the handler in the relevant category file, not in
   `UnrealMcpModule.cpp`.
3. Ensure the registry entry exists in
   `Tools/UnrealMcpToolRegistry/tools.json`.
4. Mirror registry resources under
   `Plugins/UnrealMcp/Resources/ToolRegistry/tools.json` when intended for
   packaged plugin fallback.
5. Add tests under `Tools/UnrealMcpTests/<Category>`.
6. Run registry validation.
7. Build the editor.
8. Restart Unreal Editor before expecting new C++ tools to appear.
9. Run tool audit, workbench status, and relevant test suites.

Preferred self-extension tool flow:

```text
tool_recommend
knowledge_search
preview_change_plan
scaffold_mcp_tool
mcp_validate_tool_schema
mcp_validate_cpp_patch
mcp_apply_scaffold dryRun=true
mcp_apply_scaffold dryRun=false
mcp_build_editor
restart editor
mcp_run_test_suite
knowledge_eval_run if docs/RAG/recommendation changed
verify_task_outcome
```

## RAG / Knowledge Layer

The RAG layer is local-first and deterministic. It does not require embeddings or
network access for baseline use.

Versioned inputs:

```text
README.md
Docs/**
Plugins/UnrealMcp/README.md
Tools/UnrealMcpToolRegistry/tools.json
Tools/UnrealMcpTests/**
Tools/UnrealMcpSkills/**
Tools/UnrealMcpKnowledge/Sources/*.json
```

Generated local index:

```text
Saved/UnrealMcp/KnowledgeIndex/index.json
Saved/UnrealMcp/KnowledgeIndex/cards.jsonl
```

`cards.jsonl` is generated as UTF-8 JSONL for external inspection, but it stays
under `Saved/` and should not be committed.

Official Unreal docs seed manifest:

```text
Tools/UnrealMcpKnowledge/Sources/unreal_engine_official_docs_5_7.json
```

Local downloader:

```bash
python3 Tools/unreal_mcp_fetch_docs.py --max-pages 20
```

Run RAG evals from Chat:

```text
/tool unreal.knowledge_eval_run {"evalPath":"Tools/UnrealMcpKnowledge/Evals","includeDetails":false}
```

Use RAG before designing new high-level tools. The intended behavior is:

- search existing docs/tests/skills
- recommend tools
- analyze capability gaps
- recommend workflow compositions
- scaffold only when composition is insufficient

## Chat, Context, And Memory

Chat transcript runtime file:

```text
Saved/UnrealMcp/ChatHistory.json
```

This file is local-only and ignored. It may be UTF-16 because Unreal's
`FFileHelper` can save strings that way. Do not assume it is UTF-8.

Project memory runtime file:

```text
Saved/UnrealMcp/ProjectMemory.json
```

Important memory keys:

```text
chat.active_task
chat.current_task
mcp.extension.pipeline
skill.<name>.last_apply
```

When long work nears a tool-loop limit, the plugin writes `chat.active_task`.
Future AI should read it, continue with one bounded next step, and verify rather
than re-reading all history.

Treat `Saved/UnrealMcp` as evidence for local continuity only, not as source of
truth for versioned product behavior.

## Workbench UI

Workbench is intentionally thin. It should delegate to MCP tools rather than
duplicating backend logic.

Current Workbench concepts:

- Refresh Status
- Run Audit
- Run Core Tests
- Pipeline Status
- Lock Status
- Copy Result
- Skill Activity
- Distill Draft
- Promote Dry Run
- Refresh Knowledge
- Search Knowledge
- Recommend Tools
- Run RAG Evals

If a button is added, it should call an existing MCP tool through the same path
used by Chat/HTTP tests whenever possible.

## Codex App Server Bridge

P7 Plan B adds a separate Bun daemon at:

```text
Tools/UnrealMcpCodexBridge
```

The daemon is not part of the UE plugin yet. It spawns a fresh
`codex app-server` subprocess on a temporary Unix socket, performs App Server
initialization and `thread/start`, then exposes a small UE-facing WebSocket API:

```text
ws://127.0.0.1:8766/uevolve
```

Start and smoke test:

```bash
bun run --cwd Tools/UnrealMcpCodexBridge src/index.ts
bun run --cwd Tools/UnrealMcpCodexBridge test-client.ts
```

Codex socket framing is WebSocket-over-UDS, not raw newline JSON, LSP
`Content-Length`, 4-byte length-prefixed JSON, or concatenated JSON. After the
WebSocket upgrade, each Codex App Server message is one WebSocket text frame
containing the lightweight Codex JSON-RPC object without `jsonrpc:"2.0"`.

The bridge hard-codes `gpt-5.5` with reasoning effort `xhigh`. Default approval
policy is `reject`: command execution, file changes, permission escalation, MCP
elicitation, dynamic tool calls, and tool user-input requests are declined or
left unanswered with empty results. `UEVOLVE_CODEX_APPROVAL_POLICY=auto-approve`
exists for local bridge development only.

Connecting to an already-running Codex Desktop IPC socket is deferred; V1 always
spawns its own app-server and marks health `failed` if that child exits.

## Installation Model

The plugin is normally installed into a target project as a project-level plugin.

Automated installer:

```bash
python3 Tools/install_unrealmcp_to_project.py --project "/path/to/YourProject/YourProject.uproject" --dry-run
python3 Tools/install_unrealmcp_to_project.py --project "/path/to/YourProject/YourProject.uproject"
```

Manual install copies:

```text
Plugins/UnrealMcp -> <TargetProject>/Plugins/UnrealMcp
Tools             -> <TargetProject>/Tools
Schemas           -> <TargetProject>/Schemas
Docs              -> <TargetProject>/Docs
```

Then enable `UnrealMcp` and `PythonScriptPlugin` in the target `.uproject`,
close Unreal Editor, build the project, and open the project.

The MCP endpoint only exists while Unreal Editor is open and the plugin is
loaded. It is not a standalone daemon.

Avoid having both project-level and engine-level copies of `UnrealMcp`; stale or
locked binaries cause confusing Windows failures.

## Build And Test Commands

macOS UE 5.7 build example:

```bash
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" \
  UEvolveEditor Mac Development \
  -Project="/Users/wmbt7052/Documents/Unreal Projects/MyProject/UEvolve.uproject" \
  -WaitMutex
```

Windows UE 5.7 build example:

```powershell
& "E:\3D_SOFTWARE\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  UnrealEditor Win64 Development `
  "-Project=E:\UE5_P\EasyMapper5_7\EasyMapper5_7.uproject" `
  -WaitMutex
```

Registry validation:

```bash
python3 Tools/validate_tool_registry.py
```

Core test suite from Chat:

```text
/tool unreal.mcp_run_test_suite {"testsDir":"Tools/UnrealMcpTests/Core","readProjectMemory":false,"writeProjectMemory":false}
```

Category suites:

```text
Tools/UnrealMcpTests/Actors
Tools/UnrealMcpTests/Blueprint
Tools/UnrealMcpTests/Scaffold
Tools/UnrealMcpTests/SelfExtension
Tools/UnrealMcpTests/Widget
```

RAG eval:

```text
/tool unreal.knowledge_eval_run {"evalPath":"Tools/UnrealMcpKnowledge/Evals","includeDetails":false}
```

Before compiling, close Unreal Editor or disable Live Coding. If Editor is open,
macOS/Windows builds may fail due to locked binaries or Live Coding locks.

## Testing Philosophy

Stable tests live under:

```text
Tools/UnrealMcpTests
```

Generated test scaffolds live under:

```text
Saved/UnrealMcp/TestScaffolds
```

Happy-path write tests should use disposable sandboxes:

```text
/Game/__UEvolveMcpTest*
UEvolveMcpTest_* actor labels
```

Avoid tests that mutate user content unless they first prepare and later clean a
bounded sandbox.

Tests can assert structured content fields with
`expectToolCallStructuredFields`.

## Safety Rules For Future AI

Always do these:

- Run `git status --short` before editing.
- Respect existing uncommitted changes. Do not revert files you did not change.
- Prefer `rg`/`rg --files` for search.
- Use `apply_patch` for manual edits.
- Keep generated local data out of Git.
- Use fixed schemas; do not expose flexible `additionalProperties=true` tools to
  AI-facing `tools/list`.
- For write tools, ensure policy, preflight, and postcheck metadata are correct.
- For source mutations, dry run, backup, manifest, build, test, and rollback.
- For high-risk tasks, use `unreal.preview_change_plan` or document the plan.
- For long tasks, write/read project memory instead of relying on one huge chat.
- If RAG/tool recommendation changes, run `unreal.knowledge_eval_run`.
- If ToolRegistry changes, run `python3 Tools/validate_tool_registry.py`.
- After meaningful changes, update `AGENTS.md`, `README.md`, and any focused doc
  that explains the changed subsystem.

Avoid these:

- Do not put new tools directly into `UnrealMcpModule.cpp`.
- Do not commit `Saved/`, generated KnowledgeIndex, local fetched docs, API keys,
  local supervisor launchers, or unreviewed generated scaffolds.
- Do not assume `Content/` is part of the clean distributable plugin state.
- Do not install over both engine-level and project-level plugin copies.
- Do not assume newly built C++ tools are visible until Editor restarts.
- Do not treat ChatHistory as canonical product docs.

## Known Product Direction

Near-term improvements that fit the product direction:

- Make user-generated tools portable/shareable as reviewed extension packages.
- Continue hardening `mcp_apply_scaffold` against path traversal, unsafe patch
  targets, duplicate application, stale hashes, and missing tests.
- Expand workflow composition so high-level tools can be built from existing
  low-level tools without always generating C++.
- Improve RAG retrieval quality with more curated UE docs, package docs,
  install feedback, failure examples, and tool recipes.
- Add optional local embeddings later, but keep lexical offline retrieval as the
  baseline.
- Improve UI discoverability for tool categories: built-in, self-extension,
  legacy-hidden, dynamic/CLI, and generated/user tools.
- Add CI for ToolRegistry validation, schema compatibility, and docs/test
  coverage.
- Expand tests for Windows deployment, PowerShell JSON, endpoint online checks,
  `tail_log`, build/restart workflows, and generated package sharing.

## Current Local Caveat

This file describes the repository and local context at the time it was written.
Always re-check current state with:

```bash
git status --short
git branch --show-current
python3 Tools/validate_tool_registry.py
```

If the worktree is dirty, inspect diffs before editing. Recent local work may
include self-extension hardening and path-traversal tests that should not be
overwritten casually.
