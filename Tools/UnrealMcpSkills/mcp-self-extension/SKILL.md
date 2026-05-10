# MCP Self Extension

Use this skill when extending the Unreal MCP plugin itself from Editor Chat or the external supervisor.

## Workflow

1. Start with `unreal.tool_recommend` for the user's task. Prefer existing tools, recipes, and `unreal.workflow_run` compositions before scaffolding a new tool.
2. If the task needs project knowledge, call `unreal.knowledge_search`. If it reports a missing index, run `unreal.knowledge_index_refresh` and retry the search. Do not assume the index is already connected.
3. For RAG-sensitive changes, run `unreal.knowledge_eval_run` before and after editing docs, ToolRegistry metadata, scoring, or synonyms.
4. Run `unreal.preview_change_plan` before source or asset mutation. Use the recommendation/search evidence to keep the plan bounded.
5. For repeatable multi-tool work, run `unreal.workflow_run` with `dryRun=true` first. Execute with `dryRun=false` only after the planned steps, risk gates, and verification are clear.
6. If a real MCP capability gap remains, run `unreal.mcp_tool_audit`, then `unreal.scaffold_mcp_tool` and validate generated schemas with `unreal.mcp_validate_tool_schema`.
7. Validate generated descriptor-first patch fragments with `unreal.mcp_validate_cpp_patch` and use `unreal.mcp_apply_scaffold` in `dryRun=true` mode before any real source edit.
8. Before high-risk changes, create a state snapshot with `unreal.mcp_backup_project_state`.
9. Build with `unreal.mcp_build_editor`, then restart Unreal Editor before expecting new C++ tools to appear in `tools/list`.
10. If the build fails, run `unreal.mcp_classify_error` first, then `unreal.mcp_compile_error_fix_plan` against the newest build log before patching.
11. If an integration is unsafe, prefer `unreal.mcp_rollback_to_manifest` over ad-hoc manual rollback.
12. Finish with `unreal.verify_task_outcome`; record restart handoff and next steps with `unreal.project_memory_edit` or `unreal.project_memory_write`.

## Safety Notes

- Keep generated launchers and Saved artifacts out of Git unless they are intentionally generic.
- Official Unreal documentation caches and generated KnowledgeCard indexes live under `Saved/UnrealMcp`; do not commit them.
- `Saved/UnrealMcp/KnowledgeIndex/cards.jsonl` is generated UTF-8 JSONL for local inspection, but it still belongs in Saved and should not be committed.
- User-created tool scaffolds are review artifacts until applied. They are not hot-loaded C++ tools and should be packaged or committed intentionally.
- Avoid force rollback unless the source hash mismatch has been inspected.
- Treat `dryRun=false` as a commit point: make the next action either build, test, or rollback.
- When context is long, write `chat.active_task` and continue in one bounded step rather than re-reading the full history.
