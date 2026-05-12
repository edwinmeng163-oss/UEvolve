# MCP Self Extension

Use this skill when extending the Unreal MCP plugin itself from Editor Chat or the external supervisor. The goal is to preserve the reviewable, reversible path from capability gap to compiled, tested MCP tool.

Reviewer/agent-side rules are in `Tools/codex-prompt-header.md` § Self-extension workflow.

## Two kinds of tools

- Built-in tools live in shared plugin source such as `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEditorTools.cpp`, `UnrealMcpBlueprintTools.cpp`, `UnrealMcpActorTools.cpp`, and related category files. They ship with the plugin and travel through normal git history.
- User-extended tools begin as project-local scaffold drafts under
  `<ProjectDir>/Tools/UnrealMcpToolScaffolds/<toolId>/`.
- A scaffold draft contains `ScaffoldMetadata.json`, `ToolRegistryPatch.json`, descriptor-first `.patch.cpp` fragments, `TestRequest.json`, and human-readable review docs.
- Applying a scaffold with `unreal.mcp_apply_scaffold` or `unreal.mcp_extension_pipeline` merges the handler, dispatcher, registrar, and registry entry into the shared plugin source.
- After apply, the tool is no longer isolated: it ships with the plugin,
  participates in the shared registry, and needs build, restart, and tests.
- The scaffold directory remains after apply as history and package source; do
  not delete it just because the tool was promoted.

## Scaffold location is project-local

- Scaffold drafts are Category B local project artifacts and must be rooted at
  `FPaths::ProjectDir()`.
- If the editor runs `UEvolve.uproject`, `<ProjectDir>` is the repo root and
  drafts land at `<repoRoot>/Tools/UnrealMcpToolScaffolds/`.
- If the editor runs
  `Examples/UEvolveExample57/UEvolveExample57.uproject`, `<ProjectDir>` is that
  example project and drafts land under
  `Examples/UEvolveExample57/Tools/UnrealMcpToolScaffolds/`.
- Do not walk up to the repository root when writing scaffolds.
- `UnrealMcpSharedPathResolver` is for Category A shared inputs such as skills,
  tests, and knowledge, not for scaffolds, `Saved/UnrealMcp/*`, or
  `SkillDrafts`.
- Project-local drafts let two editor sessions on different `.uproject` files
  iterate on separate tool drafts without colliding.

## Lifecycle state interpretation

`unreal.mcp_inspect_scaffold` reports orthogonal facts. `toolListed` means the
running editor's MCP `tools/list` knows the tool right now. `readyForApply`
means the scaffold files on disk are present, patch-safe, registry-valid, and
test-request-valid; it does not mean the tool is live.

| `toolListed` | `readyForApply` | State | Assistant action |
| --- | --- | --- | --- |
| `true` | `true` | Already promoted | Treat the tool as live and the scaffold as historical reference; do not re-apply it. |
| `false` | `true` | Ready draft | Use `unreal.mcp_extension_pipeline` or stepwise apply, build, test, and restart. |
| `false` | `false` | Incomplete draft | Use inspect warnings as a checklist, repair metadata, patches, or test request, then validate again. |
| `true` | `false` | Drifted or orphaned | Treat the live tool as authoritative and the scaffold as broken history; do not re-apply it. |

Schema or patch warnings on an already-promoted scaffold are informational
unless the user is intentionally repairing the historical scaffold. For a
multi-scaffold overview, prefer `unreal.mcp_workbench_status` and
`unreal.mcp_pipeline_status` before iterating `unreal.mcp_inspect_scaffold` by
hand.

## Canonical workflow

**Think first, act second**. The order below moves from understanding to
information gathering to gap decision to action; each step has a specific
existing tool. Scaffold creation is a first-class outcome when the gap
analyzer says so, not a fallback.

1. **Understand the requirement.** Restate the user's goal in your own
   words: acceptance criteria, target assets/source, risk class. If
   anything is unclear, ask before tool selection.
2. **Plan the change.** Run `unreal.preview_change_plan` to size the
   intended mutation and capture an explicit risk class. The preview is
   read-only and is the early gate that anchors everything that follows.
3. **Research existing knowledge.** Run `unreal.knowledge_search` on the
   task domain. If it reports a missing index, run
   `unreal.knowledge_index_refresh` and retry. Read the returned cards
   before deciding.
4. **Inventory capabilities and decide the gap.** Run
   `unreal.tool_recommend` and `unreal.tool_gap_analyze`. The gap analyzer
   returns one of `use_existing_tool`, `compose_existing_tools`, or
   `scaffold_new_tool`; treat that as the decision, not as advice.
5. **Self-extension audit (when the task touches the MCP plugin itself).**
   Run `unreal.mcp_tool_audit` and/or `unreal.mcp_workbench_status` to
   understand registry health, current scaffolds, and any pending
   manifests before mutating tool source.
6. **Compose existing tools when possible.** If `tool_gap_analyze` says
   `compose_existing_tools`, use `unreal.workflow_recommend` for the
   composition shape. If you want to probe a composition without
   side effects, call `unreal.workflow_run` with `dryRun=true` AND
   `writeMemory=false`; the default `writeMemory=true` writes
   `chat.active_task` even in dry-run, which is not what a probe wants.
7. **Scaffold a new tool when the gap analyzer says so.** Run
   `unreal.scaffold_mcp_tool` to produce a project-local descriptor-first
   draft under `<ProjectDir>/Tools/UnrealMcpToolScaffolds/<toolId>/`. This
   is the first-class path when `scaffold_new_tool` is the decision.
8. **Validate the draft.** Run `unreal.mcp_validate_cpp_patch` and
   `unreal.mcp_validate_tool_schema`. Repair patches or schema before
   moving on.
9. **Re-preview if scope changed.** If the concrete scaffold raises the
   risk class or touches more files than the step 2 preview anticipated,
   re-run `unreal.preview_change_plan` so the gate reflects reality.
10. **Apply, build, test.** `unreal.mcp_extension_pipeline` is preferred
    and orchestrates the full gate: preview, validation, generated test,
    dry-run apply, before-snapshot, apply, build, test suite,
    after-snapshot, diff, and outcome verification. The stepwise path
    (`unreal.mcp_apply_scaffold` -> `unreal.mcp_build_editor` ->
    `unreal.mcp_run_tool_test`) exists when finer control is needed; use
    it only with `dryRun=true` for apply first.
11. **Restart, confirm, hand off.** After the pipeline's manifest lands,
    restart the editor and confirm the new tool appears in `tools/list`.
    Re-run `unreal.mcp_tool_audit` and the generated or category test.
    Run `unreal.knowledge_eval_run` only when the change touched docs,
    ToolRegistry metadata, search scoring, or recommendation behavior.
    Finish with `unreal.verify_task_outcome`, then record restart handoff
    or next steps with `unreal.project_memory_write` or
    `unreal.project_memory_edit`.

## Cross-developer transfer

- Transfer reviewed user tools with `unreal.tools.export_package` and
  `unreal.tools.import_package`, not by pushing loose scaffold drafts.
- `unreal.tools.export_package` reads the scaffold, registry entry, tests, and
  docs, then writes a SHA-256-verified zip under
  `Saved/UnrealMcp/Packages/<toolName>-<version>.zip`.
- Share the zip outside git, such as by file transfer, email, or chat. The zip
  is gitignored on purpose.
- `unreal.tools.import_package` validates hashes and safe paths, then writes
  the scaffold into the receiver's project-local
  `<ProjectDir>/Tools/UnrealMcpToolScaffolds/`.
- A real import requires the self-extension lock and rejects duplicate registry
  names; always dry-run import first.
- After a real import, validate the registry, build, restart, audit, and run
  the relevant MCP test suite before treating the tool as available.

## Anti-patterns

- Do not hand-merge into main module: never paste `.patch.cpp` content directly
  into `UnrealMcp*Tools.cpp`, `UnrealMcpToolRegistrar.cpp`, or
  `Tools/UnrealMcpToolRegistry/tools.json` to add a tool.
- Hand-merging bypasses manifest creation, breaks rollback, confuses scaffold
  inspection, and can make future apply or import conflict.
- Do not re-apply promoted scaffolds. If `toolListed=true`, the running binary
  already has the tool; applying again risks duplicate registrations or a
  conflict detector failure.
- Do not route scaffold writes through walk-up shared-path resolution.
- Do not commit `Saved/UnrealMcp/*` artifacts such as manifests, packages,
  `ChatHistory.json`, project memory, or generated knowledge indexes.
- Do not force `unreal.mcp_rollback_to_manifest` until
  `dryRun=true` has been reviewed; rollback expects source hashes to match the
  backup snapshot.
- Do not assume a newly built C++ tool is dispatchable until the editor has
  restarted and `tools/list` includes it.

## Safety notes

- Keep generated launchers and `Saved/` artifacts out of git unless they are
  intentionally generic.
- Treat scaffold `dryRun=false` as a commit point: the next action should be
  build, test, restart, or rollback.
- Before high-risk work, create a snapshot with
  `unreal.mcp_backup_project_state`.
- If a build fails, run `unreal.mcp_classify_error`, then
  `unreal.mcp_compile_error_fix_plan` against the newest build log before
  patching.
- If integration becomes unsafe, prefer manifest rollback over ad-hoc manual
  source edits.
- For RAG-sensitive changes, run `unreal.knowledge_eval_run` before and after
  editing docs, ToolRegistry metadata, search scoring, or recommendation
  behavior.
- When context is long, write `chat.active_task` with
  `unreal.project_memory_write` or `unreal.project_memory_edit`, then continue
  in one bounded next step.
