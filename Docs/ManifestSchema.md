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

- `schemaVersion`: currently `2`.
- `manifestSchema`: currently `UnrealMcpExtensionManifest.v2`.
- `sessionId`: extension lock session active during apply. This may be empty only when `skipLock` is used.
- `toolName`: public MCP tool name.
- `toolId`: path-safe id derived from the public tool name.

## Safety Fields

- `files[]` records every changed file with `sourcePath`, `backupPath`, `afterPath`, `hashBefore`, and `hashAfter`.
- Per-file `hashBefore` and `hashAfter` protect rollback from restoring over unexpected source changes.
- `conflictCount` and `missingAnchorCount` summarize insertion risk.
- `conflictPolicy` records the insertion rules used by `PlanOrApplyPatchInsertion`.
- `changes` records every planned/applied source section.

## Conflict Detection Rules

`mcp_apply_scaffold` treats these as blocking conflicts:

- A required patch fragment is empty.
- The source already contains the tool-name conflict needle but not the exact patch fragment.
- The insertion anchor is missing.
- Static patch validation has errors and `allowUnsafePatches` is not enabled.

Exact patch-fragment matches are idempotent and are reported as `skipped_already_integrated`.

## Rollback Contract

Rollback tools should prefer a manifest over ad hoc file paths. Before a rollback, compare each current source hash to its `hashAfter`; if any differ, require explicit force or human review.
