# Extension Manifest Schema

`unreal.mcp_apply_scaffold` writes a manifest for every real source integration. The manifest is the rollback receipt for self-extension work.

Versioned schema:

```text
Schemas/UnrealMcpExtensionManifest.schema.json
```

Runtime manifest locations:

```text
Saved/UnrealMcp/ExtensionBackups/<timestamp>_<tool_id>/Manifest.json
Saved/UnrealMcp/LastExtensionApply.json
```

## Required Identity Fields

- `schemaVersion`: currently `1`.
- `manifestSchema`: currently `UnrealMcpExtensionManifest.v1`.
- `sessionId`: extension lock session active during apply. This may be empty only when `skipLock` is used.
- `toolName`: public MCP tool name.
- `toolId`: path-safe id derived from the public tool name.

## Safety Fields

- `sourceHashBefore` and `sourceHashAfter` protect rollback from restoring over unexpected source changes.
- `backupSourcePath` and `afterSourcePath` point to before/after source snapshots.
- `conflictCount` and `missingAnchorCount` summarize insertion risk.
- `conflictPolicy` records the insertion rules used by `PlanOrApplyScaffoldInsertion`.
- `changes` records every planned/applied source section.

## Conflict Detection Rules

`mcp_apply_scaffold` treats these as blocking conflicts:

- A required snippet is empty.
- The source already contains the tool-name conflict needle but not the exact snippet.
- The insertion anchor is missing.
- Static snippet validation has errors and `allowUnsafeSnippets` is not enabled.

Exact snippet matches are idempotent and are reported as `skipped_already_integrated`.

## Rollback Contract

Rollback tools should prefer a manifest over ad hoc file paths. Before a rollback, compare the current source hash to `sourceHashAfter`; if it differs, require explicit force or human review.
