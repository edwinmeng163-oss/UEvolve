# Self-Extension Pipeline

## Purpose

The self-extension pipeline lets Unreal MCP add new MCP tools from inside Editor Chat while preserving reviewability and rollback safety.

## Main Tools

- `unreal.scaffold_mcp_tool`: create scaffold files.
- `unreal.mcp_validate_tool_schema`: check OpenAI function calling compatibility.
- `unreal.mcp_validate_cpp_snippet`: static-check generated C++ snippets.
- `unreal.mcp_patch_scaffold_snippet`: edit scaffold snippets with dry run and backup.
- `unreal.mcp_apply_scaffold`: insert snippets into plugin source with dry run, backup, and idempotence checks.
- `unreal.mcp_build_editor`: run UBT and parse build logs.
- `unreal.mcp_run_tool_test`: run one generated test request.
- `unreal.mcp_run_test_suite`: run a generated test directory.
- `unreal.mcp_rollback_last_extension`: restore the latest apply backup.
- `unreal.mcp_rollback_to_manifest`: restore a selected historical manifest.
- `unreal.mcp_extension_pipeline`: orchestrate the flow.
- `unreal.mcp_workbench_status`: summarize health and next steps.

Versioned core tests live in `Tools/UnrealMcpTests/Core`. Generated tests remain local under `Saved/UnrealMcp/TestScaffolds`.

Shared contracts:

- Tool names follow [Tool Naming](ToolNaming.md).
- Real apply manifests follow [Extension Manifest Schema](ManifestSchema.md).
- Restart handoff is documented in [External Supervisor](Supervisor.md).

## Normal Flow

1. Scaffold.
2. Validate schema.
3. Generate tests.
4. Dry-run apply.
5. Apply with backup.
6. Write project memory.
7. Build editor.
8. Restart if new plugin code must be loaded.
9. Run test suite.
10. Audit and inspect workbench status.

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

## Failure Flow

If build fails:

1. Run `unreal.mcp_compile_error_fix_plan`.
2. Patch the relevant snippet/source if the fix is deterministic.
3. Rebuild.
4. If unsafe or unclear, rollback to manifest.

If tests fail:

1. Inspect failed test case output.
2. Compare expected vs actual structured content.
3. Patch handler or tests.
4. Rebuild/restart only if C++ changed.

If source state is inconsistent:

1. Run `unreal.mcp_diff_last_apply`.
2. Run `unreal.mcp_rollback_to_manifest` with `dryRun=true`.
3. Roll back with `force=true` only after manual review.
