# Contributing to Unreal MCP

## Development Principle

Prefer small, reviewable tool changes over broad source edits. The self-extension pipeline is powerful, but it should still produce human-reviewable diffs.

## Before Adding a Tool

1. Define the tool purpose and owner category.
2. Pick a name that follows [Tool Naming](ToolNaming.md).
3. Add an `FUnrealMcpToolDescriptor` and fixed input schema through the C++ tool registrar.
4. Use existing fixed-schema wrappers before adding flexible inputs.
5. Prefer fixed schemas with `additionalProperties=false`.
6. Add or confirm handler registry coverage.
7. Add reviewed ToolRegistry JSON override metadata when the descriptor defaults are not enough.
8. Add docs and at least one test request.
9. Run schema validation, registry validation, build, restart, tests, and tool audit.

## Recommended Extension Flow

1. Generate a scaffold with `unreal.scaffold_mcp_tool`.
2. Inspect with `unreal.mcp_inspect_scaffold`.
3. Validate schema with `unreal.mcp_validate_tool_schema`.
4. Validate snippets with `unreal.mcp_validate_cpp_snippet`.
5. Preview with `unreal.mcp_apply_scaffold` using `dryRun=true`.
6. Apply with backups.
7. Build with `unreal.mcp_build_editor`.
8. Restart and run `unreal.mcp_run_test_suite`.
9. Run `unreal.mcp_tool_audit`.
10. Check `unreal.mcp_workbench_status`.

## Ownership Guidelines

Suggested ownership domains:

- MCP protocol and settings.
- Editor and asset tools.
- Actor tools.
- Blueprint graph tools.
- Widget Blueprint tools.
- Self-extension pipeline.
- Supervisor and cross-platform launch.
- Documentation and tests.

Avoid adding new tool logic to `UnrealMcpModule.cpp`; it is intentionally a thin lifecycle entrypoint. Add tool behavior to the relevant category file and update the descriptor registrar, explicit ToolRegistry override, handler registry, docs, and tests in the same change.

The repository also includes `.github/CODEOWNERS` so pull requests touching MCP source, supervisor launch, docs, tests, schemas, or project-local skills request review from the current project owner by default.

## Collaboration Guards

- Check `unreal.mcp_workbench_status` before a risky edit.
- Use `unreal.mcp_lock_extension_session {"mode":"status"}` before applying scaffolds, builds, tests, or rollbacks from multiple Chat windows.
- Do not force-release a lock unless it is stale and you know which session created it.
- Real scaffold applies write manifests matching `Schemas/UnrealMcpExtensionManifest.schema.json`.
- Treat `conflictCount > 0`, `missingAnchorCount > 0`, or a changed `sourceHashAfter` as a stop-and-review signal.
- Generated local supervisor launchers stay ignored; edit `Tools/UnrealMcpSupervisorTemplates` and `Docs/Supervisor.md` for shared changes.

## Git Hygiene

- Do not commit `Saved/`, `Intermediate/`, `Binaries/`, or generated local supervisor files.
- Commit source, docs, stable test fixtures, and project-local skills.
- Use Git LFS for Unreal binary assets.
- Keep generated runtime manifests local unless they are intentionally promoted to documentation or tests.
- Keep schema files under `Schemas/` versioned and backward-compatible where practical.

## Review Checklist

- Tool appears in `tools/list` if AI-facing.
- Tool is descriptor-backed unless it is a documented legacy compatibility path.
- Tool has a handler.
- Tool is documented in root or plugin README.
- Schema passes OpenAI function calling compatibility checks.
- Audit output includes appropriate ToolRegistry policy metadata.
- Write-capable tools use dry run or clear safety controls where practical.
- Self-extension source changes have a backup/rollback path.
- Manifest, session id, and conflict-policy behavior are documented when source mutation behavior changes.
