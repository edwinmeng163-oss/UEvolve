# Unreal MCP Self-Extension Workbench Roadmap

## Product Direction

Unreal MCP is evolving from an editor automation plugin into an Unreal Editor MCP self-extension workbench.

The goal is not only to let AI call existing editor tools. The goal is to let AI safely extend the MCP surface through a guarded workflow:

1. Scaffold a new tool.
2. Validate schema compatibility.
3. Validate generated descriptor-first C++ patch fragments.
4. Preview source diffs with dry run.
5. Apply with backups and a lock.
6. Build the editor target.
7. Restart or hand off to an external supervisor.
8. Run generated tests.
9. Audit the tool surface.
10. Persist memory and rollback data.

## Current Baseline

- Editor MCP endpoint is local-first at `http://127.0.0.1:8765/mcp`.
- AI-facing tool schemas are validated through `unreal.mcp_validate_tool_schema`.
- Self-extension is orchestrated by `unreal.mcp_extension_pipeline`.
- Build/test handoff is supported by project memory and supervisor scripts.
- Legacy flexible-schema actor tools are hidden from AI-facing `tools/list` by explicit ToolRegistry metadata.
- Tool policy is versioned in `Tools/UnrealMcpToolRegistry/tools.json` and mirrored into plugin resources for plugin-only installs.
- Write/build/process tools now receive preflight/postcheck metadata in their structured tool result.
- Blueprint, Widget, Actor, Memory, Skill, Scaffold, and Self-extension tools have category-specific preflight/postcheck verifiers for real editor/file/workflow state checks.
- Handler presence audit and dispatch now use a ToolRegistry-derived ToolHandlerRegistry instead of scanning source text or maintaining a second hand-written handler list.
- `unreal.mcp_workbench_status` provides a read-only dashboard summary for the self-extension system.

## Near-Term Priorities

1. Split the large module into category-specific tool files.
2. Expand workflow postcheck verifiers with deeper source diff, manifest schema, and rollback-to-manifest checks.
3. Add CI coverage for ToolRegistry/ToolHandlerRegistry validation.
4. Promote category test fixtures from smoke coverage to CI-ready disposable-project suites.
5. Add Workbench UI controls for preflight review, postcheck evidence, and registry gaps.

## Medium-Term Priorities

1. Convert scaffold patches into category-owned extension points.
2. Add CI smoke checks for schema compatibility and documentation coverage.
3. Add Windows build/restart/supervisor verification.
4. Add team-shared memory and skill packs.
5. Add marketplace-like discovery for project-local MCP extensions.

## Long-Term Vision

The workbench should make MCP extension feel like a normal Unreal development loop: generate, review, compile, test, ship, and rollback. The AI should never silently mutate source without a reversible manifest, a visible diff, and a test path.
