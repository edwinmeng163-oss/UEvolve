# Unreal MCP Architecture

## Runtime Shape

The plugin runs inside Unreal Editor and exposes a local HTTP JSON-RPC MCP endpoint. The in-editor Chat panel uses the same tool handlers as external MCP clients.

Core layers:

- `FUnrealMcpModule`: module startup, HTTP routing, MCP protocol handling, Chat command dispatch, and current tool execution.
- Tool helpers in `UnrealMcpModule.cpp`: editor, actor, Blueprint, widget, scaffold, self-extension, memory, skill, build, and test logic.
- `UnrealMcpToolRegistry`: lightweight metadata for visibility, handler aliases, and migration status.
- `Tools/unreal_mcp_supervisor.py`: external process for restart-aware pipeline automation.
- `Tools/UnrealMcpSupervisorTemplates`: versioned macOS/Windows supervisor launcher templates with placeholders instead of machine-specific paths.
- `Schemas/UnrealMcpExtensionManifest.schema.json`: versioned contract for source apply manifests.
- `Saved/UnrealMcp`: local runtime state, memory, manifests, backups, generated tests, and logs.
- `Tools/UnrealMcpSkills`: project-local skill instructions.

## Current Bottleneck

Most behavior still lives in `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpModule.cpp`. This works for rapid prototyping but is not ideal for team development because unrelated tool changes collide in the same file.

## Target Module Layout

Recommended split:

- `Private/Core`: MCP protocol helpers, JSON-RPC response helpers, schema utilities.
- `Private/ToolRegistry`: tool metadata, visibility, handler aliases, policy metadata.
- `Private/Tools/SelfExtension`: self-extension workbench, pipeline status, MCP test execution, and extension pipeline helpers. The first split moved `unreal.mcp_workbench_status`, `unreal.mcp_pipeline_status`, `unreal.mcp_run_tool_test`, `unreal.mcp_run_test_suite`, and `unreal.mcp_extension_pipeline` into `UnrealMcpSelfExtensionTools.cpp`.
- `Private/Tools/Editor`: status, logs, maps, assets, PIE, console, Python.
- `Private/Tools/Actors`: actor selection, transforms, spawning, layout, batch edits.
- `Private/Tools/Blueprint`: Blueprint class and graph editing.
- `Private/Tools/Widget`: Widget Blueprint hierarchy, layout, event binding.
- `Private/Tools/Scaffold`: gameplay scaffolds and MCP tool scaffolds.
- `Private/Tools/SelfExtension`: validate, apply, build, test, audit, rollback, pipeline.
- `Private/Tools/Memory`: project memory CRUD.
- `Private/Tools/Skills`: project skill discovery and application. `unreal.skill_list`, `unreal.skill_read`, and `unreal.skill_apply` now live in `UnrealMcpSkillTools.cpp`.
- `Private/UI`: Chat panel and future Workbench panel.

## ToolRegistry Role

ToolRegistry is the migration bridge. It should eventually become the single place to answer:

- Is this tool AI-facing?
- Which handler executes it?
- Which category owns it?
- Is it read-only or write-capable?
- Does it require a lock, build, restart, or external process?
- Is it deprecated or legacy-hidden?

For now it handles legacy-hidden schema-incompatible tools and fixed-schema handler aliases.

It also provides first-pass policy metadata for every visible tool. The policy object is attached to `tools/list` entries and reused by audit/workbench status output:

- `riskLevel`
- `requiresWrite`
- `requiresBuild`
- `requiresExternalProcess`
- `requiresRestart`
- `requiresProjectMemory`
- `requiresLock`

Future work should replace broad heuristics with explicit per-tool ownership metadata.

## Data and State

Local runtime state remains under `Saved/UnrealMcp` and is ignored by Git.

Team-shared planning and policy state should live under `Docs/` or a future `.unrealmcp/` directory so it can be reviewed and versioned.
