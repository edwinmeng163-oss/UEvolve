# Knowledge RAG Source Taxonomy

## 1. Purpose

UEvolve's current RAG layer can explain tools, docs, tests, and official Unreal
documentation, but it cannot yet answer session-grounded requests such as
"按上次的方式布置 FPS 场景". The current index refresh writes cards from official
docs, versioned markdown, and the ToolRegistry only
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:1200`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:1213`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:1218`).

This document is the authoritative source taxonomy for the closed-loop RAG work until it is superseded.
It sits beside the RAG plan named by the project read order (`AGENTS.md:103`) and is covered by the documentation freshness rule (`AGENTS.md:112`).

## 2. Source inventory - Category A: already on disk

| Source | Producer (file:line) | Record schema | Current write trigger | Planned T3 treatment | Privacy |
| --- | --- | --- | --- | --- | --- |
| `Saved/UnrealMcp/ChatHistory.json` | `SUnrealMcpChatPanel::SaveHistory` writes root metadata and `entries[]` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpChatPanel.cpp:2676`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpChatPanel.cpp:2681`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpChatPanel.cpp:2693`). | `{last_response_id,last_log_text,entries:[{type,speaker,title,body,details,tool_call_id,is_error,is_pending}]}` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpChatPanel.cpp:2682`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpChatPanel.cpp:2694`). | Chat panel history persistence through `SaveHistory`. | Do not index raw full chat by default; index summary cards only after explicit local opt-in as `runtime-memory`. | `local-only` |
| `Saved/UnrealMcp/ProjectMemory.json` | `ProjectMemoryWrite` updates entries and calls `SaveProjectMemory` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpMemoryTools.cpp:67`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpMemoryTools.cpp:93`). | `{version,projectName,updatedAtUtc,entries:[{key,summary,status,nextStep,updatedAtUtc,tags,content}]}` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpMemoryTools.cpp:28`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpMemoryTools.cpp:160`). | Explicit memory tools and assistant active-task checkpoints. | Index selected keys such as `chat.active_task` as `runtime-memory` with content caps. | `local-only` |
| `Saved/UnrealMcp/ActivityLog/*.jsonl` | Skill recorder owns the log root, session ID, append helper, and event writer (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:102`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:122`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:185`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:509`). | One JSON object per line: `{sessionId,goal,timestampUtc,eventType,summary,details}` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:521`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:525`). | Skill recording start/stop, heartbeat, distill/save/promote, plus MCP call/result records while recording is active (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:928`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:942`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:570`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpToolDispatcher.cpp:127`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:349`). | Index bounded summaries as `activity-log`; keep payload excerpts short. | `local-only` |
| Skill files: drafts and promoted skills | Draft paths, promoted paths, and writes are in SkillTools (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:161`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:167`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:447`). | Markdown `SKILL.md` with goal, summary, reusable steps, or distilled workflow text (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1101`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1043`). | Distill activity, save draft, or promote draft (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:987`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1075`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1132`). | Index promoted skills and user-approved drafts as `skill`. | `promotable` |
| Self-extension manifests and backups | Backup root and latest manifest path are defined in core tools (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionCoreTools.cpp:26`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionCoreTools.cpp:41`). | Writer emits `{action,applyMode,schemaVersion,manifestSchema,sessionId,toolName,toolId,scaffoldDir,backupDirectory,appliedAtUtc,changes,files,postcheck}` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1071`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1081`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1085`). | Real `mcp_apply_scaffold` with backup enabled writes `ExtensionBackups/<timestamp>_<tool>/Manifest.json` and `LastExtensionApply.json` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1011`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1092`). | Index manifest summaries as `activity-log` unless T3 adds `extension-outcome`. | `local-only` |
| Extension backup snapshots | Backup files are written beside the manifest as `.before` and `.after` snapshots (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1032`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1041`). | Text snapshots plus manifest `files[]` entries with hashes and paths (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1046`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1086`). | Same real apply path as manifests. | Do not index raw snapshots; use manifest summary and hashes only. | `local-only` |
| `Saved/UnrealMcp/SupervisorLogs/*.log` | The external supervisor sets a timestamped log path and appends text/JSON lines (`Tools/unreal_mcp_supervisor.py:42`, `Tools/unreal_mcp_supervisor.py:49`, `Tools/unreal_mcp_supervisor.py:33`, `Tools/unreal_mcp_supervisor.py:53`). | Mixed text lines and JSON lines `{time,event,payload}` (`Tools/unreal_mcp_supervisor.py:56`). | Supervisor commands run with `--log-dir`; plugin installer passes a default log dir (`Tools/unreal_mcp_supervisor.py:717`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionBuildTools.cpp:655`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionBuildTools.cpp:733`). | Skip by default; optionally index failure summaries as `activity-log`. | `local-only` |

Small examples of current records:

```json
{"entries":[{"type":"assistant","speaker":"AI","title":"","body":"Done","details":"","tool_call_id":"","is_error":false,"is_pending":false}]}
{"entries":[{"key":"chat.active_task","summary":"Resume task","status":"in_progress","nextStep":"Verify","updatedAtUtc":"2026-05-11T00:00:00Z","tags":["chat"],"content":{}}]}
{"sessionId":"20260511-000000-12345678","goal":"Build FPS scene","timestampUtc":"2026-05-11T00:00:00Z","eventType":"recording_started","summary":"Skill activity recording started.","details":{"reset":true}}
{"action":"mcp_apply_scaffold","schemaVersion":2,"toolName":"unreal.example","appliedAtUtc":"2026-05-11T00:00:00Z","changes":[],"files":[]}
```

The checked-in extension manifest schema still describes a v1 required shape
with `sourcePath`, `backupSourcePath`, and `afterSourcePath`
(`Schemas/UnrealMcpExtensionManifest.schema.json:8`,
`Schemas/UnrealMcpExtensionManifest.schema.json:24`). The current writer emits
the descriptor-first v2 shape shown above, so schema alignment is an open T2/T3
documentation and validation question.

## 3. Source inventory - Category B: UE editor delegates available but unhooked

As of this document, the plugin registers zero listeners for the delegate
families below. T1 greps over `Plugins/UnrealMcp/Source` found no matches for
`FEditorDelegates::`, asset registry event names, exact `USelection`, exact
`FOutputDevice`, `FCoreUObjectDelegates::OnObjectPropertyChanged`,
`IConsoleManager` hook patterns, or `UBlueprint::OnCompiled`. Existing Map Check
support is an explicit tool call, not a listener
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:651`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:667`).
Every row is therefore net-new infrastructure.

| Delegate identifier | Signal value | Expected volume | Capture cost | Recommended P-tier |
| --- | --- | --- | --- | --- |
| `FEditorDelegates::OnMapOpened` | high | low | low | P1 |
| `FEditorDelegates::BeginPIE / EndPIE` | high | low | low | P1 |
| `FEditorDelegates::PreSaveWorld` | medium | medium | low | P2 |
| `FAssetRegistryModule::OnAssetAdded` | high | medium | medium | P1 |
| `FAssetRegistryModule::OnAssetRemoved` | high | medium | medium | P1 |
| `FAssetRegistryModule::OnAssetRenamed` | high | medium | medium | P1 |
| `USelection` selection-changed delegate | medium | high | medium | P2 |
| `FCoreUObjectDelegates::OnObjectPropertyChanged` | high | very high | high | P2 |
| `FOutputDevice` Output Log interception | medium | high | high | P2 |
| `IConsoleManager` console hook | medium | medium | medium | P2 |
| `UBlueprint::OnCompiled` | high | medium | medium | P1 |
| Map Check via `FMessageLog` | high | low | medium | P1 |

The explicit console-command tool currently runs `GEditor->Exec` with a local
`FStringOutputDevice`; that is useful evidence for tool calls but not an
always-on console hook
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:386`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:405`).

## 4. Source inventory - Category C: out-of-scope

- Viewport mouse trajectory: no stable public API at the right semantic level,
  high volume, and low task-disambiguation value.
- Keystrokes: privacy red line. UEvolve should not record arbitrary user input.
- Screenshots: large payloads and usually require a vision model before they are
  useful to lexical RAG.
- Per-`FUICommandList` instrumentation: broad integration cost with weak
  evidence value compared with tool calls, asset events, and map events.

## 5. Source inventory - Category D: outside UE

- Project Git history: valuable for long-term change explanations, but this T1
  branch does not add Git access or Git-derived cards.
- OS-level filesystem events: can catch external edits, but they duplicate
  manifest, asset registry, and project memory signals for the first phase.
- External MCP client requests through the local endpoint: feasible in T2 because
  every HTTP request passes `HandleMcpHttpRequestInternal`, `tools/call` routes
  through `HandleToolsCall`, and execution reaches `ExecuteTool`
  (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:160`,
  `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:255`,
  `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:320`,
  `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:338`).

## 6. Unified storage spec - extend existing ActivityLog format

T1 does not change the on-disk format. This is the design target for T2.

```json
{
  "sessionId": "string",
  "ts": "ISO8601",
  "eventKind": "string (see enum)",
  "taskLabel": "string?",
  "summary": "string",
  "payload": { "...kind-specific...": null },
  "refs": {
    "mapPath": "string?",
    "blueprintPath": "string?",
    "actorLabels": ["string"],
    "assetPaths": ["string"]
  },
  "correlation": {
    "chatTurnId": "string?",
    "toolCallId": "string?",
    "manifestId": "string?",
    "parentSessionId": "string?"
  }
}
```

`sessionId` is the per-editor-launch session. Existing skill-recording sessions
remain valid child sessions.

`ts` is the event timestamp. Existing records use `timestampUtc`; T2 readers
should accept both during migration (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:525`).

`eventKind` replaces `eventType` for new writers. Existing `eventType` strings
remain valid `eventKind` values for compatibility
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:526`).

`taskLabel` is a short user-visible label, for example `FPS layout pass`.
Existing `goal` is preserved and can be treated as a legacy label
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:522`).

`summary` is capped human-readable evidence. Current ActivityLog summaries are
already capped to 2000 characters (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:527`).

`payload` holds kind-specific structured details. It is the successor to current
`details` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:528`).

`refs` holds stable UE references that help RAG connect a task to maps,
Blueprints, actors, and assets.

`correlation` ties chat turns, tool calls, manifests, and child skill-recording
sessions together. Current chat history already carries `tool_call_id`
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpChatPanel.cpp:2699`), and
extension manifests already carry `sessionId`
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1076`).

## 7. eventKind enum (canonical list)

| eventKind | Semantics | Tier | Planned producer |
| --- | --- | --- | --- |
| `tool_call` | Always-on MCP tool request with tool name, argument keys, risk, and result summary. | P0 | Protocol/Dispatcher (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:320`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpToolDispatcher.cpp:110`) |
| `chat_turn` | User prompt plus assistant response summary for one assistant turn. | P0 | AssistantRun finish path (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpAssistantRun.cpp:405`) |
| `manifest_apply` | Real self-extension apply result, manifest ID, changed files, and postcheck summary. | P0 | SelfExtensionApply real apply (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1071`) |
| `manifest_dryrun` | Dry-run self-extension apply plan without source writes. | P0 | SelfExtensionApply dry-run return (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:995`) |
| `skill_apply` | User applies or promotes a reusable skill. | P0 | Skill promotion path (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1132`) |
| `skill_distilled` | Activity session is distilled into a skill draft. | P0 | Skill distillation (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1059`) |
| `asset_change` | Asset added, removed, renamed, or materially changed. | P1 | New AssetRegistry listener |
| `level_open` | Map opened or editor world context changed. | P1 | New `FEditorDelegates::OnMapOpened` listener |
| `pie_event` | PIE begin/end lifecycle event. | P1 | New `FEditorDelegates::BeginPIE / EndPIE` listener |
| `map_check` | Map Check result summary with error and warning counts. | P1 | Existing explicit tool plus future listener (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:651`) |
| `console_command` | User or tool executes a console command. | P2 | Existing explicit console tool plus future hook (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:386`) |
| `extension_lock_acquire` | Extension session lock acquired. | P0 | Lock tool acquire path (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionStateTools.cpp:695`) |
| `extension_lock_release` | Extension session lock released. | P0 | Lock tool release path (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionStateTools.cpp:710`) |
| `heartbeat` | Legacy skill-recorder heartbeat; kept readable, not a new RAG signal. | legacy | Skill activity ticker (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:570`) |
| `recording_started` | Legacy skill activity session start. | legacy | Skill recording start (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:928`) |
| `recording_stopped` | Legacy skill activity session stop. | legacy | Skill recording stop (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:942`) |
| `skill_draft_saved` | Legacy draft save event; semantically part of skill authoring. | legacy | Skill draft save (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1122`) |
| `skill_draft_promoted` | Legacy promoted skill event; semantically maps to `skill_apply`. | legacy | Skill promotion (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1249`) |
| `mcp_tool_call` | Legacy skill-recorder MCP call event; semantically maps to `tool_call`. | legacy | Current dispatcher hook (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpToolDispatcher.cpp:127`) |
| `mcp_tool_result` | Legacy skill-recorder MCP result event; semantically maps to `tool_call`. | legacy | Current protocol hook (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:349`) |

Current ActivityLog is still skill-recording-scoped. T2 should promote the
shared writer into an always-on launch-session log rather than depending on the
skill recording switch.

Verification table for literal `eventType` strings found in `UnrealMcpSkillTools.cpp`:

| Current literal | Corresponding `eventKind` |
| --- | --- |
| `heartbeat` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:570`) | `heartbeat` |
| `recording_started` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:928`) | `recording_started` |
| `recording_stopped` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:942`) | `recording_stopped` |
| `skill_distilled` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1059`) | `skill_distilled` |
| `skill_draft_saved` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1122`) | `skill_draft_saved` |
| `skill_draft_promoted` (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:1249`) | `skill_draft_promoted` |

## 8. sourceKind enum reconciliation

`UnrealMcpKnowledgeCard.schema.json` already defines exactly these eight
`sourceKind` values: `tool-registry`, `versioned-doc`, `official-docs`, `skill`,
`runtime-memory`, `activity-log`, `test-fixture`, and `unknown`
(`Schemas/UnrealMcpKnowledgeCard.schema.json:48`,
`Schemas/UnrealMcpKnowledgeCard.schema.json:50`).

The current indexer emits `official-docs`, `versioned-doc`, and `tool-registry`
only (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:645`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:676`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:756`).
Search results already return `sourceKind` from cards
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:1376`).

Recommendation for T3: keep the enum unchanged. The reserved `activity-log`,
`runtime-memory`, and `skill` slots cover the new sources. Open question:
whether a future `extension-outcome` value is worth adding when T3 implements
write-back. Decision deferred to T3.

## 9. Per-launch session ID

Generate one launch-scoped session ID in `FUnrealMcpModule::StartupModule`
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpModule.cpp:10`). Expose it
through a tiny `UnrealMcpSession.h` accessor in T2.

Format should match the existing skill activity pattern:
`{YYYYMMDD-HHMMSS}-{guid}`. The existing helper uses UTC time and the first
eight GUID digits (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:122`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:124`).

The per-launch session resets on every editor launch. It is independent of
skill-recording sessions; skill sessions become children via
`correlation.parentSessionId`.

## 10. Privacy class table

| Class | Applies to | Indexing rule |
| --- | --- | --- |
| `local-only` | Runtime memory, ActivityLog, ChatHistory, manifests, supervisor logs. | Default storage is local. Index only into the local KnowledgeIndex when the user or tool policy opts in. |
| `promotable` | Skills and distilled drafts. | May move from private draft to reusable knowledge only through explicit user promotion or indexing opt-in. |
| `versioned` | Docs, registry, versioned tests, official-docs cache manifests. | Safe for default local indexing because it is already project source or curated reference metadata. |

No `local-only` or `promotable` content may be indexed beyond the local
KnowledgeIndex without explicit user opt-in. Versioned sources remain the only
unconditional default input.

## 11. P0/P1/P2 priority list

| Tier | Work | Notes |
| --- | --- | --- |
| P0 (executed in T2) | Add per-launch `sessionId`. | Generate during module startup (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpModule.cpp:10`). |
| P0 (executed in T2) | Expand ActivityLog writers from skill-recording-scoped events to always-on `tool_call`. | Capture Protocol/Dispatcher calls (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpProtocol.cpp:320`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpToolDispatcher.cpp:110`). |
| P0 (executed in T2) | Emit `chat_turn`. | AssistantRun completion is the capture point (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpAssistantRun.cpp:405`). |
| P0 (executed in T2) | Emit `manifest_apply` and `manifest_dryrun`. | SelfExtensionApply already distinguishes dry run and real apply (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:995`, `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSelfExtensionApplyTools.cpp:1071`). |
| P0 (executed in T2) | Add `unreal.chat_label_active_task`. | Writes a label for the current launch session; no T1 code change. |
| P1 | AssetRegistry `OnAssetAdded/Removed/Renamed` -> `asset_change`. | High signal for "what changed last time"; medium lifecycle cost. |
| P1 | `FEditorDelegates::OnMapOpened / BeginPIE / EndPIE` -> `level_open` / `pie_event`. | Low volume and useful for session timelines. |
| P1 | Map Check listener -> `map_check`. | Current explicit Map Check already returns counts (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp:667`). |
| P2 | Property-change firehose. | Deferred for volume, deduplication, and privacy review. |
| P2 | Console-command hook. | Deferred because explicit tool calls are already capturable and global hooks may include unrelated user commands. |
| P2 | Output Log capture. | Deferred for noise, size, and accidental secret capture risk. |
| P2 | Selection events. | Deferred because selection churn is high and usually weak evidence without surrounding tool context. |

## 12. Migration plan

Existing skill-only ActivityLog files must remain readable. New writers use
`eventKind`; readers accept old `eventType` as a fallback alias for one minor
version. Current readers already parse `eventType` for distillation and event
appendices (`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:355`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpSkillTools.cpp:410`).

Existing `goal` remains readable. New writers add `taskLabel`; readers use
`taskLabel`, then `goal`, then an empty label.

T2 should add `Schemas/UnrealMcpActivityLogEntry.schema.json`. Do not add that
schema in T1.

## 13. Closed-loop link to T3

T3 should extend `UnrealMcpKnowledgeTools.cpp` so
`knowledge_index_refresh` consumes `Saved/UnrealMcp/ActivityLog/*.jsonl` and
`Tools/UnrealMcpSkills/**` files. The writer already stores cards under
`Saved/UnrealMcp/KnowledgeIndex/cards.jsonl`
(`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:766`,
`Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpKnowledgeTools.cpp:1263`).

Those new cards should populate the existing `activity-log` and `skill`
`sourceKind` slots. This section is a forward link only; it does not prescribe
code changes for T1.

## 14. Open questions

- Should `chat_turn` payload include the full assistant text, or only a summary plus tool-call references?
- What rolling-window cap should ActivityLog use: max file size, max entry count per launch session, or both?
- Should `extension-outcome` become its own `sourceKind`, or stay merged into `activity-log`?
- Should `mcp_tool_call` and `mcp_tool_result` remain documented legacy `eventKind` values after the new `tool_call` writer ships, or become aliases accepted only by readers?
- Should the v1 manifest schema be updated to match the current v2 writer before T3 indexes extension manifests?
