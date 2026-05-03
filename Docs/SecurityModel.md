# Unreal MCP Security Model

## Trust Boundary

The MCP endpoint is intended for local development by default. It runs inside Unreal Editor and can perform powerful editor actions. Treat it as a developer tool, not a public network service.

## Local-First Defaults

- Default host is localhost.
- Optional auth token and allowed origins are configured through Unreal MCP settings.
- Generated runtime state is stored under `Saved/UnrealMcp`.

## Tool Risk Levels

Recommended policy categories:

- Read-only: status, list assets, map check, audit.
- Editor write: actor transforms, Blueprint edits, widget edits, save packages.
- Code write: scaffold patch/apply, snippet edits, rollback.
- Build/process: build editor, supervisor launch/restart.
- Dynamic execution: Python and console command execution.

Each AI-facing tool now carries the same policy object in `tools/list`, `unreal.mcp_tool_audit`, and `unreal.mcp_workbench_status`:

- `riskLevel`
- `requiresWrite`
- `requiresBuild`
- `requiresExternalProcess`
- `requiresRestart`
- `requiresProjectMemory`
- `requiresLock`

## Schema Safety

AI-facing tools should use fixed JSON schemas. OpenAI function calling rejects schemas that rely on `additionalProperties=true`.

Legacy flexible-schema tools are kept for compatibility but hidden from AI-facing `tools/list`:

- `unreal.batch_set_actor_properties`
- `unreal.spawn_actor`
- `unreal.spawn_actor_batch`

Use fixed-schema wrappers instead.

## Source Mutation Safety

Self-extension tools should preserve these rules:

- Dry run before apply.
- Static snippet validation before source insertion.
- Lock during apply/build/test/rollback.
- Backup before source mutation.
- Manifest after source mutation.
- Manifest includes `schemaVersion`, `manifestSchema`, active `sessionId`, conflict counts, conflict policy, and source hashes.
- Build log capture after compile.
- Test suite after restart.
- Rollback path if build or test fails.

## Tool Outcome Verification

Write-capable tools attach structured `preflight` and `postcheck` results. Generic checks come from ToolRegistry metadata. Blueprint, Widget, Actor, Memory, Skill, Scaffold, and Self-extension tools additionally inspect real editor/file/workflow state before and after execution, so Chat and Workbench can distinguish "the tool returned success" from "the target asset, graph, widget, actor, transform, memory key, skill file, manifest, build log, or test result actually exists as expected."

## Remaining Hardening Work

- Add CI coverage for ToolRegistry/ToolHandlerRegistry validation.
- Add optional policy enforcement that blocks high-risk tools unless enabled.
- Add an audit log for all write-capable tool calls.
- Add CI checks for schema compatibility and missing documentation.
- Add a Workbench UI warning surface for risky actions.
