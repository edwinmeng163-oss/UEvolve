# Tool Naming and Registration Rules

This project uses predictable names so humans, Chat, tests, audit reports, and generated manifests can all reason about MCP tools without guesswork.

## Public Tool Names

Use this shape for AI-facing tools:

```text
unreal.<domain>_<verb>[_object]
```

Rules:

- Use lowercase ASCII letters, digits, and underscores only.
- Start every tool with `unreal.`.
- Prefer verb-led names such as `unreal.widget_add`, `unreal.bp_connect_pins`, or `unreal.mcp_run_test_suite`.
- Keep names stable after release. Add a new wrapper instead of changing a public contract.
- Avoid abbreviations unless the domain is already established, such as `bp`, `mcp`, or `ui`.
- Do not reuse names for different behavior. A changed schema means a changed tool or clearly versioned compatibility path.

## Domain Prefixes

- `unreal.editor_*`: editor status and general editor context.
- `unreal.list_*`: read-only asset, map, actor, and selection queries.
- `unreal.bp_*`: Blueprint graph and compilation operations.
- `unreal.widget_*`: UMG Widget Blueprint hierarchy, layout, and binding operations.
- `unreal.scaffold_*`: gameplay or MCP scaffold generation.
- `unreal.mcp_*`: MCP self-extension, audit, build, test, rollback, supervisor, and workbench tools.
- `unreal.project_memory_*`: restart-resilient project memory operations.
- `unreal.skill_*`: project-local skill discovery and application.

## Legacy and Wrapper Names

Flexible-schema legacy tools stay hidden from AI-facing `tools/list` unless explicitly reviewed:

- `unreal.spawn_actor`
- `unreal.spawn_actor_batch`
- `unreal.batch_set_actor_properties`

Fixed-schema replacements should use a descriptive suffix:

- `unreal.spawn_actor_basic`
- `unreal.spawn_actor_batch_basic`
- `unreal.spawn_static_mesh_actor`

## C++ Handler Names

Prefer one implementation function per public capability group. A handler should:

- Read only fixed, named fields from `FJsonObject`.
- Return `UnrealMcp::MakeExecutionResult`.
- Put structured output into `structuredContent`.
- Avoid writing source, assets, or external process state unless the ToolRegistry policy marks that risk.

## ToolRegistry Metadata

Every AI-facing tool should have reviewed policy metadata:

- `riskLevel`
- `requiresWrite`
- `requiresBuild`
- `requiresExternalProcess`
- `requiresRestart`
- `requiresProjectMemory`
- `requiresLock`

Tools that mutate source, run builds, restart the editor, or roll back manifests must acquire the extension session lock unless there is a deliberate, documented exception.

## Test and Documentation Names

- Stable test files live in `Tools/UnrealMcpTests/<Area>/<tool_or_case>.json`.
- Generated scaffold tests live under `Saved/UnrealMcp/TestScaffolds` or the scaffold folder itself.
- Tool documentation belongs in `README.md`, `Plugins/UnrealMcp/README.md`, or a focused doc under `Docs/`.
- Extension apply manifests follow `Schemas/UnrealMcpExtensionManifest.schema.json`.
