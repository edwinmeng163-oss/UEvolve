# Self-Extension Pipeline

## Purpose

The self-extension pipeline lets Unreal MCP add new MCP tools from inside Editor Chat while preserving reviewability and rollback safety.

## Main Tools

- `unreal.scaffold_mcp_tool`: create descriptor-first patch files, ToolRegistry patch metadata, an extension report, and optional legacy compatibility fragments.
- `unreal.mcp_validate_tool_schema`: check OpenAI function calling compatibility.
- `unreal.mcp_validate_cpp_patch`: static-check generated C++ patch fragments.
- `unreal.mcp_patch_scaffold_patch`: edit scaffold patch fragments with dry run and backup.
- `unreal.mcp_apply_scaffold`: apply `ToolRegistryPatch.json`, registrar descriptor, category handler, and dispatcher patches with dry run, backup, and idempotence checks.
- `unreal.mcp_build_editor`: run UBT for the editor target and parse build logs.
- `unreal.mcp_build_game`, `unreal.mcp_build_server`, `unreal.mcp_build_client`: run UBT for non-editor target coverage and parse build logs.
- `unreal.mcp_build_packaged`: run RunUAT BuildCookRun for cooked packaged output and parse UAT logs.
- `unreal.mcp_run_tool_test`: run one generated test request.
- `unreal.mcp_run_test_suite`: run a generated test directory.
- `unreal.mcp_rollback_last_extension`: restore the latest apply backup.
- `unreal.mcp_rollback_to_manifest`: restore a selected historical manifest.
- `unreal.mcp_extension_pipeline`: orchestrate the flow.
- `unreal.mcp_workbench_status`: summarize health and next steps.
- `unreal.knowledge_eval_run`: run local RAG regression cases after changing
  docs, ToolRegistry metadata, search scoring, or tool recommendation logic.

The same status and safety tools are also exposed through `Window > Unreal MCP Workbench`, a thin Slate panel for people who prefer a dashboard over typing `/tool` commands.

Versioned core tests live in `Tools/UnrealMcpTests/Core`. Generated tests remain local under `Saved/UnrealMcp/TestScaffolds`.

Shared contracts:

- Tool names follow [Tool Naming](ToolNaming.md).
- Real apply manifests follow [Extension Manifest Schema](ManifestSchema.md).
- Restart handoff is documented in [External Supervisor](Supervisor.md).

## Path Resolution

Scaffold writers are project-local by design. `unreal.scaffold_mcp_tool` and
other draft/Saved creators write under the active `<ProjectDir>` so multiple
host projects can keep isolated drafts.

Reader-side tools use project-first fallback. `unreal.mcp_apply_scaffold`,
`unreal.mcp_inspect_scaffold`, `unreal.mcp_validate_*`, Python bridge handler
resolution, and ToolRegistry reads check active-project `Tools/` content first,
then walk up to the shared repo-root `Tools/` tree. This lets example projects
loaded through `AdditionalPluginDirectories` use canonical repo-root handlers,
registry metadata, and pre-staged scaffolds while still allowing per-project
overrides.

Applier source writes use the loaded plugin location from `IPluginManager`,
not `<ProjectDir>/Plugins/UnrealMcp`. Example projects can therefore apply
patches to the real plugin source even when the plugin is imported through
`AdditionalPluginDirectories`.

## Normal Flow

1. Search or recommend tools with `unreal.knowledge_search`,
   `unreal.tool_recommend`, or `unreal.tool_gap_analyze`.
2. Preview the change plan with `unreal.preview_change_plan`.
3. Validate schema.
4. Generate tests.
5. Dry-run apply.
6. Capture a before snapshot when real work will run.
7. Apply with backup.
8. Write project memory.
9. Build editor.
10. Restart if new plugin code must be loaded.
11. Run test suite.
12. Run `unreal.knowledge_eval_run` when RAG, docs, recommendation, or
    ToolRegistry metadata changed.
13. Capture an after snapshot, diff it, and run `unreal.verify_task_outcome`
    when no restart deferral is needed.
14. Audit and inspect workbench status.

`unreal.mcp_extension_pipeline` now treats this as the default gate:

```text
preview_change_plan -> validate_schema -> generate_tests -> apply_dry_run -> capture_before_snapshot -> apply -> build -> test_suite -> capture_after_snapshot -> diff_project_snapshot -> verify_task_outcome
```

The gate can be relaxed with `enforceGate=false`, `captureSnapshots=false`, or `verifyOutcome=false`, but normal self-extension work should keep all three enabled.

Run the versioned core suite when validating baseline health:

```text
/tool unreal.mcp_run_test_suite {"testsDir":"Tools/UnrealMcpTests/Core","readProjectMemory":false,"writeProjectMemory":false}
```

## Restart Handoff

Unreal Editor cannot keep executing while its own process is closed. For strict self-extension, use the external supervisor:

```bash
python3 Tools/unreal_mcp_supervisor.py pipeline --auto-restart --args-json '{"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}'
```

The supervisor can restart the editor and resume tests through project memory.

Generate local launchers with:

```bash
python3 Tools/unreal_mcp_supervisor.py install --platform all --output-dir Tools/UnrealMcpSupervisor --overwrite
```

The generated launchers are local and ignored; versioned templates are stored under `Tools/UnrealMcpSupervisorTemplates`.

## Tool Sharing

Reviewed tool work can be moved between projects with package export/import
instead of copying loose scaffold files by hand.

`unreal.tools.export_package` accepts a live `toolName`, optional `version`,
optional `scaffoldDir`, optional `packagePath`, and `dryRun`. It requires the
tool to exist in the live registry, then builds a manifest preview from:

- the ToolRegistry entry JSON;
- the tool scaffold directory when one is present or explicitly supplied;
- versioned MCP test fixtures that call that tool;
- the file referenced by the registry `docsPath`, when available.

Real export writes a zip under:

```text
Saved/UnrealMcp/Packages/<toolName>-<version>.zip
```

The zip contains `manifest.json`, `registry/tool.json`, and optional
`scaffold/`, `tests/`, and `docs/` entries. The manifest schema is
`Schemas/UnrealMcpToolPackageManifest.schema.json`; every entry is recorded
with `path`, `kind`, `sizeBytes`, and `sha256`, plus package metadata such as
`toolName`, `version`, `created_at`, and `source_repo_commit`.

`unreal.tools.import_package` accepts a `packagePath`, `dryRun`, and optional
`overwriteScaffold`. It validates the manifest, rejects unsafe archive paths,
checks every SHA-256 hash, reads `registry/tool.json`, and prepares an import
plan. Dry run is the normal review step and does not write anything.

Real import requires the self-extension lock. It writes scaffold files under
`Tools/UnrealMcpToolScaffolds/<toolName>`, writes package test fixtures under
`Tools/UnrealMcpTests`, and appends the registry entry to
`Tools/UnrealMcpToolRegistry/tools.json` only when that tool name is not already
present. Duplicate registry names are reported in dry run and rejected for real
imports.

Recommended sharing flow:

```text
/tool unreal.tools.export_package {"toolName":"unreal.my_tool","dryRun":true}
/tool unreal.tools.export_package {"toolName":"unreal.my_tool","version":"reviewed-1","dryRun":false}
/tool unreal.tools.import_package {"packagePath":"Saved/UnrealMcp/Packages/unreal.my_tool-reviewed-1.zip","dryRun":true}
/tool unreal.tools.import_package {"packagePath":"Saved/UnrealMcp/Packages/unreal.my_tool-reviewed-1.zip","dryRun":false}
```

After a real import, run `python3 Tools/validate_tool_registry.py`, build the
editor, restart, then run `unreal.mcp_tool_audit` and the relevant MCP test
suite before treating the imported tool as available.

## Failure Flow

When a pipeline step fails, `unreal.mcp_extension_pipeline` attaches `failureAnalyses`.
Each analysis includes `unreal.mcp_classify_error` output and next-step guidance.
If source changes were already applied, the pipeline also attaches a dry-run rollback plan using `unreal.mcp_rollback_to_manifest`.

If build fails:

1. Run `unreal.mcp_compile_error_fix_plan`.
2. Patch the relevant descriptor-first patch/source if the fix is deterministic.
3. Rebuild.
4. If unsafe or unclear, rollback to manifest.

If tests fail:

1. Inspect failed test case output.
2. Compare expected vs actual structured content.
3. Patch handler or tests.
4. Rebuild/restart only if C++ changed.

## Descriptor-First Scaffold Output

New MCP scaffolds now produce the files Chat needs for the registry-derived handler path:

- `ToolRegistrar.patch.cpp`: descriptor and fixed schema helper.
- `ToolRegistrarCall.patch.cpp`: call site for `RegisterAllMcpToolDescriptors`.
- `CategoryHandlerFunction.patch.cpp`: implementation stub for the selected category source file.
- `CategoryDispatcherBranch.patch.cpp`: category dispatcher branch for the selected `TryExecute*Tool`.
- `ToolRegistryPatch.json`: reviewed policy metadata override candidate.
- `ScaffoldMetadata.json`: machine-readable category, risk, source-file, and side-effect metadata.
- `ExtensionReport.md`: human-reviewable summary of gates, files, and risks.

Legacy ToolDefinition and ExecuteToolHandler fragments are only generated when `includeLegacyCompatibility=true`; new self-extension work must use the descriptor-first patch path and should not add tool logic to `UnrealMcpModule.cpp`.

If source state is inconsistent:

1. Run `unreal.mcp_diff_last_apply`.
2. Run `unreal.mcp_rollback_to_manifest` with `dryRun=true`.
3. Roll back with `force=true` only after manual review.
