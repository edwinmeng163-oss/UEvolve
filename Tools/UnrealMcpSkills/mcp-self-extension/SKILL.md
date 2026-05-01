# MCP Self Extension

Use this skill when extending the Unreal MCP plugin itself from Editor Chat or the external supervisor.

## Workflow

1. Run `unreal.mcp_tool_audit` before changing source to understand tool registration, schema, handler, and documentation coverage.
2. Validate generated schemas with `unreal.mcp_validate_tool_schema` and generated snippets with `unreal.mcp_validate_cpp_snippet`.
3. Use `unreal.mcp_apply_scaffold` in `dryRun=true` mode before any real source edit.
4. Before high-risk changes, create a state snapshot with `unreal.mcp_backup_project_state`.
5. Build with `unreal.mcp_build_editor`, then restart Unreal Editor before expecting new C++ tools to appear in `tools/list`.
6. If the build fails, run `unreal.mcp_compile_error_fix_plan` against the newest build log before patching.
7. If an integration is unsafe, prefer `unreal.mcp_rollback_to_manifest` over ad-hoc manual rollback.
8. Record restart handoff and next steps with `unreal.project_memory_edit` or `unreal.project_memory_write`.

## Safety Notes

- Keep generated launchers and Saved artifacts out of Git unless they are intentionally generic.
- Avoid force rollback unless the source hash mismatch has been inspected.
- Treat `dryRun=false` as a commit point: make the next action either build, test, or rollback.
