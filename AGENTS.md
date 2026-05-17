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
VersionName: 0.15.0
EngineVersion: 5.6.0
Type: Editor plugin
Required plugin: PythonScriptPlugin
```

The plugin supports Unreal Engine 5.6 and 5.7 from the same source tree.
`UEvolve.uproject` is the local development host and defaults its
`EngineAssociation` to `5.6` as the lower bound; UE 5.7 users can switch the
local project association when opening or generating project files.
Two optional sample-content hosts ship alongside the root:
`Examples/UEvolveExample` (UE 5.6.1) and `Examples/UEvolveExample57`
(UE 5.7.4). Pick the variant matching the installed engine.

### Multi-engine compatibility discipline

- All `#if ENGINE_*_VERSION` goes in `UnrealMcpEngineCompat.h`. Business code
  must not contain version checks.
- Run `Tools/install_git_hooks.sh` once after clone to enable the local
  pre-commit linter.
- `EAiProviderKind` values are append-only; do not renumber.

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

If the task is about Windows compatibility / Win build / Win packaging / a "works on Mac but breaks on Win" bug:

```text
Docs/WindowsCompatibilityLessons.md
Docs/Stage2WindowsVerify.md
Tools/package_plugin.ps1
Tools/UnrealMcpCodexBridge/start-bridge.ps1
```

`WindowsCompatibilityLessons.md` indexes 20 hard-won failure modes from the issue #2 saga (commits 57ce634 / fe65d25 / 9fd70ac / e08a995 / 088d056 / 8917f99). Grep your symptom against it before re-deriving.

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
  Stage2WindowsVerify.md
  Supervisor.md
  ToolNaming.md
  UnrealTaskRecipes.md
  WindowsCompatibilityLessons.md

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
  UnrealMcpToolScaffoldStarters/
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

Versioned scaffold starters live under:

```text
Tools/UnrealMcpToolScaffoldStarters/
```

These are reviewed starter packages that can be copied into the ignored
`Tools/UnrealMcpToolScaffolds/` workspace, then applied with
`unreal.mcp_apply_scaffold` so the normal manifest, backup, build, test, and
rollback trail is preserved.

## Current High-Level Feature Set

Editor and inspection:

- `unreal.editor_status`
- `unreal.editor.engine_version`
- `unreal.project_settings_get`
- `unreal.tail_log`
- `unreal.map_check`
- map, asset, selected asset, actor, and selected actor listing
- PIE start/stop
- console command execution
- Python command/file execution
- map/asset opening
- Content Browser sync
- save dirty packages
- asset move, redirector fixup, dependency remap, and project version migration

Actor tools:

- read actor property and transform state with `unreal.actor_get_property` and
  `unreal.actor_get_transform`
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
- add and delete macro graphs with macro-reference checks
- delete graph nodes
- delete member variables and user function graphs
- rename member variables and user function graphs with local reference fixup
- add and remove Blueprint interface implementations
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
- build editor/game/server/client targets and packaged BuildCookRun
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
- export/import reviewed tool packages with hashed manifests for sharing

RAG and recommendation:

- `unreal.knowledge_index_refresh`
- `unreal.knowledge_search`, with optional `sourceKinds[]` filtering over the
  eight `sourceKind` enum values from `UnrealMcpKnowledgeCard.schema.json`,
  optional `groupByKind`, and always-returned `kindStatus` map availability
  (`active`, `active-empty`, `reserved-not-active`)
- `unreal.tool_recommend`
- `unreal.tool_gap_analyze`
- `unreal.workflow_recommend`
- `unreal.knowledge_eval_run`
- `unreal.preview_change_plan` and `unreal.verify_task_outcome` auto-attach RAG
  `evidence[]` from the incoming `task`, including `cardId`, `sourcePath`,
  `sourceKind`, `excerpt`, `score`, and `queryUsed`
- Successful `unreal.verify_task_outcome` writes a `sourceKind=activity-log`
  outcome card to `Saved/UnrealMcp/KnowledgeIndex/cards.jsonl` for future search

Project memory:

- read/write/view/edit/delete local project memory under `Saved/UnrealMcp`.

Skills:

- list/read/apply project skills
- start/stop activity recording
- inspect activity status
- `unreal.chat_label_active_task` sets or clears the launch-session task label
  that subsequent ActivityLog events automatically carry
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

At the time this file was written, the registry contained 140 entries across:

- actors
- blueprint
- editor
- memory
- scaffold
- self-extension
- skills
- widget

This count includes the v0.14 Python runtime smoke tool, the three v0.15
chunk 1 C++ readback inspectors (`unreal.actor_get_property`,
`unreal.actor_get_transform`, and `unreal.project_settings_get`), and the five
v0.15 chunk 2a C++ Blueprint refactor basics (`unreal.bp_delete_node`,
`unreal.bp_delete_variable`, `unreal.bp_delete_function`,
`unreal.bp_rename_variable`, and `unreal.bp_rename_function`), and the four
v0.15 chunk 2b Blueprint macro/interface tools (`unreal.bp_add_macro_graph`,
`unreal.bp_delete_macro_graph`, `unreal.bp_interface_add`, and
`unreal.bp_interface_remove`), and the four v0.15 chunk 4 UBT target matrix
tools (`unreal.mcp_build_game`, `unreal.mcp_build_server`,
`unreal.mcp_build_client`, and `unreal.mcp_build_packaged`), and the four
v0.15 chunk 5 migration tools (`unreal.asset_move`,
`unreal.redirector_fixup`, `unreal.dependency_remap`, and
`unreal.project_version_migration`). Earlier handoff text lagged at 119
entries.

Current project status: v0.15 chunk 5 landed; migration toolchain is complete
(`asset_move` + `redirector_fixup` + `dependency_remap` +
`project_version_migration`), and v0.15 chunks 1-5 close all Known Limitations
from v0.14.0-python-track. UBT target matrix is complete (editor + game +
server + client + packaged), recipe catalog lists 7 named recipes, and Lane Z
Blueprint refactor is complete. The previous Lane V get/inspect-tool gap for
actor property, actor transform, and project settings readback is done in C++;
Blueprint delete + rename basics are done in C++, and macro + interface editing
is now done in C++. Tier 2 path resolution is fixed: example projects can host
the plugin through `AdditionalPluginDirectories` while Python bridge handlers,
ToolRegistry reads, and scaffold apply/inspect/validate readers fall back to
repo-root shared content with per-project drafts taking precedence.
Issue #2 comment 8 follow-up is upstreamed in the Codex App Server bridge:
Windows now defaults to stdio outbound transport, prefers user-mode Codex under
`%LOCALAPPDATA%\OpenAI\Codex\bin\*`, and skips WindowsApps binaries that Bun
cannot spawn.

`unreal.configure_fps_settings` and
`unreal.bp_add_input_axis_event_node` were moved back to scaffold-only status
pending functional verification and replacement by the planned
`unreal.fps.bootstrap` plus `unreal.simulation.verify_input_drives_pawn` tools.

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

UnrealMcpSession.h
  launch-scoped session ID accessor

UnrealMcpActivityLog.h
  always-on ActivityLog event writer

UnrealMcpKnowledgeBridge.h
  shared bridge for knowledge-index card writes

UnrealMcpEditorTools.cpp
UnrealMcpEditorEngineVersionTool.cpp
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

Private/Tests/*.cpp
  module-private automation tests such as engine-compat smoke coverage
```

The largest current files are still ChatPanel, ActorTools, KnowledgeTools,
ScaffoldTools, BlueprintTools, WidgetTools, ToolDefinitions, AssistantRun, and
several SelfExtension helpers. Prefer cautious single-category edits.

## Path Resolution Policy (Four Domains)

As of commit `fe65d25` (Tier 2 — issue #2 fix), runtime path resolution is
formally split into four trust domains. This is the **canonical contract**
for anything in the plugin source touching `Tools/...`,
`Plugins/UnrealMcp/...`, or `Saved/UnrealMcp/...` paths. Pre-Tier-2 code
assumed `FPaths::ProjectDir()` for everything, which broke example-host
mode (`Examples/UEvolveExample{,57}.uproject`) that loads the plugin via
`AdditionalPluginDirectories: ["../../Plugins"]`.

| Domain | What lives here | Resolver |
|---|---|---|
| **READER** | versioned `Tools/...` assets: PyTools, ToolRegistry source, scaffold recipes (`Tools/UnrealMcpToolScaffolds/<id>`), skills, tests, knowledge | `ResolveToolsReadSubpath(subpath, sentinels)` in `UnrealMcpSharedPathResolver.h`. Project-local first, then walks up via `IPluginManager::FindPlugin("UnrealMcp")->GetBaseDir()` anchor. Returns `FToolsReadResolution {Path, bFound, SourceKind (Unresolved|ProjectLocal|SharedRepoRoot|PluginResources), Candidates, Warning}` — surface `SourceKind` + `Candidates` in tool responses. |
| **WRITER** | new scaffold drafts from `unreal.scaffold_mcp_tool`, session output, project-local generated artifacts | `ResolveProjectOutputDirectory` in `UnrealMcpScaffoldTools.cpp`. UNCHANGED post-Tier-2. Always lands under active `<ProjectDir>/Tools/UnrealMcpToolScaffolds/<id>`. Do NOT walk up — per-project draft isolation (CATEGORY B). |
| **PLUGIN SOURCE** | C++ source / `Resources/` writes (apply scaffold target files) | `ResolvePluginSourceRoot()` in `UnrealMcpSharedPathResolver.h`. Returns `IPluginManager::FindPlugin("UnrealMcp")->GetBaseDir() + "/Source"`. Never assume `<ProjectDir>/Plugins/UnrealMcp` — example projects mount the plugin from elsewhere. |
| **SAVED** | backups, manifests, locks, ActivityLog, BuildLogs, packages, project memory | `FPaths::ProjectSavedDir()` / `Saved/UnrealMcp/`. UNCHANGED. Do NOT migrate to shared repo. Manifest body CAN reference paths under plugin BaseDir or shared Tools root for rollback target validation. |

**Apply scaffold crosses three domains in one operation:**

- READ scaffold metadata + patch fragments via **READER**
- WRITE C++ patches into plugin source via **PLUGIN SOURCE**
- WRITE backup manifests + rollback breadcrumbs via **SAVED**

When adding new code that touches any of these paths, pick the right
resolver from the table above. When refactoring code that uses
`FPaths::ProjectDir()`, ask which domain the path belongs to and convert
to the matching resolver. Pure-function variants of these resolvers
(`ResolveToolsReadSubpath_Pure(ProjectDir, PluginBaseDir, Subpath,
FileOrDirExists)`) enable unit tests without mocking — see
`Plugins/UnrealMcp/Source/UnrealMcp/Private/Tests/UnrealMcpSharedPathResolverTests.cpp`
for the canonical 9-case test matrix + 5 root-host zero-regression
invariants.

The full writer/reader/plugin-source/Saved rule is also formalized in
`Tools/codex-prompt-header.md` § "Path resolution domains" for Codex
prompt prefix purposes.

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

Tool sharing path for reviewed tools:

```text
unreal.tools.export_package dryRun=true
unreal.tools.export_package dryRun=false
unreal.tools.import_package dryRun=true
unreal.tools.import_package dryRun=false
```

Packages live under `Saved/UnrealMcp/Packages`, contain
`manifest.json`, `registry/tool.json`, and optional scaffold/test/docs entries,
and validate SHA-256 hashes before import. Real import requires the extension
lock and rejects duplicate ToolRegistry names.

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
Docs/KnowledgeRagSources.md
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

`Tools/UnrealMcpKnowledge/build_index.py` is the CI/offline parity indexer for
the same card schema used by in-Editor `unreal.knowledge_index_refresh`.

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

ActivityLog launch records live under:

```text
Saved/UnrealMcp/ActivityLog/<sessionId>.jsonl
```

`UnrealMcp::GetLaunchSessionId()` mints one per-launch session ID at editor
startup, shaped `{YYYYMMDD-HHMMSS}-{guid8}`. The always-on writer rotates to
`<sessionId>.<n>.jsonl` at 10 MB or 5000 entries. It emits `tool_call` for every
MCP dispatch, `chat_turn` per assistant turn, `manifest_apply` /
`manifest_dryrun` for self-extension events, plus existing skill events, all
independent of skill recording. See `Docs/KnowledgeRagSources.md` for the full
taxonomy.

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
`codex app-server` subprocess on a platform-selected transport (Unix socket on
macOS/Linux, stdio child-process pipe on Windows — current Codex builds reject
`--listen ws://...`; see issue #2 comment 8), performs App Server
initialization and `thread/start`, then exposes a small UE-facing WebSocket API
(the UE-facing inbound listener is always WebSocket regardless of which
outbound transport speaks to Codex):

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

The bridge defaults to `UEVOLVE_CODEX_MODEL=gpt-5.5` and
`UEVOLVE_CODEX_EFFORT=xhigh`; the UE-facing `start_turn` message may override
model and effort per turn for human-driven Codex Desktop bridge chat. Default
approval policy is `reject`: command execution, file changes, permission
escalation, MCP elicitation, dynamic tool calls, and tool user-input requests
are declined or left unanswered with empty results.
`UEVOLVE_CODEX_APPROVAL_POLICY=auto-approve` exists for local bridge development
only. The separate Codex CLI subprocess provider remains hard-locked to
`gpt-5.5` with `xhigh` reasoning and is macOS/Linux-only; Windows users should
use the `CodexAppServer` provider path.

At startup, the bridge auto-manages `~/.codex/config.toml` with
`[mcp_servers.unrealmcp]` using `transport="streamable-http"` so Codex CLI,
Codex Desktop GUI, and bridge-spawned sessions discover the editor MCP server
through the same native config path.

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

Pre-commit / CI checks:

```bash
python3 Tools/validate_tool_registry.py
python3 Tools/check_ue56_compat.py
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

## Release Verification SOP (Mac Stage 2 e2e)

Every projectroot zip MUST be e2e-tested before tag-publish. The procedure is
the "Mac Stage 2 e2e" workflow that locked the v0.14.0-python-track Mac
release. Replicate exactly:

1. **Fresh test project** at `/tmp/UEvolveMacZipTest` from `TP_Blank` (the
   Codex-bridge / scaffold-applier tools fail loudly if a stale Saved/ tree
   is reused, so always start from a clean template).
2. **Extract zip at project root**, NOT under `Plugins/`. The projectroot
   overlay creates `Plugins/UnrealMcp/`, `Tools/...`, and `Docs/FIRST_LAUNCH.md`.
3. **UBT build** the editor module against the local UE 5.7 install:
   ```bash
   "/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" \
     UEvolveMacZipTestEditor Mac Development \
     -project="/tmp/UEvolveMacZipTest/UEvolveMacZipTest.uproject" -waitmutex
   ```
   Expected: `Result: Succeeded` in ~30s on a warm cache.
4. **Launch editor headless** with `-nullrhi -unattended`; poll the log for
   `LogUnrealMcp:` listening + `Engine is initialized`. Confirm port 8765 is
   bound (`lsof -nP -iTCP:8765 -sTCP:LISTEN`).
5. **Three smoke curls** (PASS = all three return `isError=false` with the
   listed payloads):

   - `tools/list`: expect `len(tools) >= 110` AND
     `unreal.editor.python_runtime_info` present.
   - `tools/call unreal.editor.python_runtime_info` (no args): expect
     `isError=false` and the bridge resolved the handler file
     (`Tools/UnrealMcpPyTools/editor_python_runtime_info/main.py`).
   - `tools/call unreal.mcp_apply_scaffold` with
     `{toolName: "unreal.fps.bootstrap", dryRun: true}`: expect
     `canApply=true`, `scaffolds=2` (= `fps_bootstrap` +
     `verify_input_drives_pawn` dependency chain), `scaffoldDir` resolving
     to `Tools/UnrealMcpToolScaffolds/fps_bootstrap`, plus
     `buildRequirements.includesPlanned` of 3 + `requiredModules` containing
     `BlueprintGraph`.

6. **Cleanup**: `kill <editor PID>`, wait for port 8765 free, then
   `rm -rf /tmp/UEvolveMacZipTest /tmp/uezip-editor.log`.

If smoke 3 fails on a missing scaffold file, it is almost always a packager
gap, not an applier bug — re-read the projectroot overlay invariants below
before opening a defect against the applier.

### Stale plugin-level binary trap

Real failure mode observed during the 2026-05-17 Tier 2 e2e: when a previous
build wrote a dylib to `Plugins/UnrealMcp/Binaries/Mac/UnrealEditor-UnrealMcp.dylib`
(e.g. from a dev-host build on `UEvolve.uproject` at the repo root) AND you
then UBT-rebuild against an example host, UBT writes the fresh dylib to
`Examples/<ExampleName>/Binaries/<Platform>/`, but the editor mount path for
the externally-loaded plugin keeps loading the OLD plugin-level binary. The
smoke responses look exactly like pre-fix behavior, with newly-added
structured-content fields totally missing.

Diagnostic signal: the runtime response of a tool you just changed lacks any
new structured-content keys you added in the patch.

**Mandatory cleanup before any example-host smoke after a plugin code
change:**

```bash
cd "$REPO_ROOT"
rm -rf Plugins/UnrealMcp/Binaries Plugins/UnrealMcp/Intermediate
"<UE>/Engine/Build/BatchFiles/Mac/Build.sh" MyProjectEditor Mac Development \
  -project="$REPO_ROOT/Examples/UEvolveExample57/UEvolveExample57.uproject" \
  -WaitMutex
```

After rebuild, the only `UnrealEditor-UnrealMcp.dylib` on disk should be
under the example host's `Binaries/`. If the plugin-level path still has
one, the rm above did not run successfully — repeat.

### Projectroot zip overlay invariants

Every Mac source-only / Windows full-experience projectroot zip MUST ship
the following tree (extract-target = `<UserProject>/` next to the
`.uproject`):

```
<UserProject>/Plugins/UnrealMcp/                      # plugin source (Mac) or source+Win64 binaries (full)
<UserProject>/Tools/UnrealMcpToolRegistry/            # tools.json + schema.json (writable)
<UserProject>/Tools/UnrealMcpPyTools/                 # Python handler scripts
<UserProject>/Tools/UnrealMcpToolScaffoldStarters/    # pristine templates for unreal.scaffold_mcp_tool
<UserProject>/Tools/UnrealMcpToolScaffolds/           # pre-staged ready-to-apply scaffolds
  fps_bootstrap/                                      # canonical recipe (apply unit)
  verify_input_drives_pawn/                           # canonical recipe (apply unit)
<UserProject>/Tools/UnrealMcpSkills/                  # SKILL.md tree
<UserProject>/Tools/UnrealMcpKnowledge/               # Sources/ + Evals/core_rag_eval.json
<UserProject>/Tools/UnrealMcpTests/                   # Core/ + SelfExtension/ + Knowledge/closed_loop
<UserProject>/Tools/UnrealMcpCodexBridge/             # bridge source (excludes node_modules + runtime)
<UserProject>/Docs/FIRST_LAUNCH.md                    # trilingual quickstart
```

`UnrealMcpToolScaffoldStarters` and `UnrealMcpToolScaffolds` are NOT the
same. Starters are templates `unreal.scaffold_mcp_tool` clones from when a
user adds a new tool. Scaffolds are the pre-staged working copies
`unreal.mcp_apply_scaffold` reads at apply time. Both must coexist; do
not rename one to the other.

`Tools/package_plugin.sh` and `Tools/package_plugin.ps1` enforce these
invariants via `[ -f ... ] || die "Staging integrity failure: ..."` (sh) and
`Assert-PlainFile` (ps1). If you add a new top-level overlay subtree, add
both the copy step and the matching assertion in both packagers.

### Tool-count discipline

The registry tool count appears in three places that must stay synced:

- `Tools/UnrealMcpToolRegistry/tools.json` length (canonical)
- `Plugins/UnrealMcp/Resources/ToolRegistry/tools.json` length (mirror; must
  match canonical byte-for-byte via `cmp -s`)
- The `"the registry contained N entries"` line in this AGENTS.md
- The `N registered MCP tools` line in `README.md` (EN + 中文 + 日本語)
- The "Read-only and context tools" / "Editor action tools" lists in
  `Plugins/UnrealMcp/README.md`

Cross-check via:
```bash
python3 -c 'import json; print(len(json.load(open("Tools/UnrealMcpToolRegistry/tools.json"))["tools"]))'
grep -nE "registry contained|registered MCP tools" AGENTS.md README.md
```

Before any commit that adds or removes a tool, bump all three numbers in
the same commit. The "lagging by one" failure mode (v0.14 left AGENTS.md
at 119 while the registry was 120) is the canary that a freshness-clause
audit was skipped.

### Release publish flow

After a tag-moving fix (e.g. Lane P3+P4 packaging fixes on a release tag):

```bash
git tag -d <tag> && git tag <tag> <newSha>
git push origin <branch>
git push --force origin <tag>
bash Tools/package_plugin.sh --version <ver> --engine-tag ue56-ue57
gh release delete-asset <tag> <old-asset-name>     --yes --repo <repo>
gh release delete-asset <tag> <old-asset-name>.sha256 --yes --repo <repo>
gh release upload <tag> <new-zip> <new-zip>.sha256 --repo <repo>
gh release edit <tag> --notes-file <updated-body.md> --repo <repo>
```

Always update both the filename AND the SHA in the release notes' `Verify`
block; the v0.14 draft was caught with v0.12.0-pilot filenames left over
because only the body text was edited but not the asset reference.

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
