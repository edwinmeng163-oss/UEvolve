# Codex Prompt Header (UEvolve convention)

> Cat this file in front of every codex-agent task prompt. It encodes the
> always-on conventions for this repo so the per-task prompt can stay focused
> on the actual EDIT list.
>
> Example usage:
>   codex-agent start "$(cat Tools/codex-prompt-header.md; cat /tmp/my-task.md)" \
>     -m gpt-5.5 -r xhigh -s workspace-write \
>     -d /Users/wmbt7052/Documents/Unreal Projects/MyProject

---

## Repo conventions you must honor (UEvolve)

- This is a UE 5.6 / 5.7 dual-engine editor plugin that also targets both
  Windows and macOS users. C++ must compile on both engines on both
  platforms.
- All `#if ENGINE_MAJOR_VERSION` / `#if ENGINE_MINOR_VERSION` shims live in
  exactly one file: `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpEngineCompat.h`.
  Do NOT add engine-version preprocessor logic anywhere else in business code.
  `#if PLATFORM_WINDOWS` / `#if PLATFORM_MAC` are platform guards (not
  engine-version guards) and may appear inline.
- `EAiProviderKind` (in `Plugins/UnrealMcp/Source/UnrealMcp/Public/UnrealMcpSettings.h`)
  is an append-only enum. Never change existing numeric values; never
  re-order; only add new entries at the end with a new numeric.
- The `Tools/git-hooks/pre-commit` linter and `Tools/validate_tool_registry.py`
  must keep passing. If you are about to touch the tool registry or
  schemas, run the validator yourself before reporting done.
- Do NOT touch unrelated pre-existing untracked files (e.g. `Tools/git-hooks/post-*`
  artifacts from local hook installation). Leave them alone in `git status`.

## Workflow expectations

- Stay STRICTLY inside the EDIT list declared in the per-task prompt.
- Do NOT run `git commit`, `git add`, `git checkout`, `git push`, or any
  branch / index mutation. Leave your changes in the working tree only;
  the reviewer (Claude as PM) stages and commits.
- After completing the EDIT list, evaluate the change against the
  AGENTS.md "Documentation Freshness Rule". Specifically check whether
  your change crosses any of these thresholds (and if so, include minimal
  AGENTS.md edits in the same change set):
  - Tool-count line ("the registry contained N entries")
  - Tool list section
  - RAG / Knowledge layer section
  - C++ Architecture file inventory
  - Provider matrix / platform-support / current project status lines
  Pure bugfix / pure refactor / docs-only changes usually need NO AGENTS.md
  edits — state so explicitly in your final report.

## VISIBILITY (Codex Desktop readability)

The user views your sessions in the Codex Desktop macOS app. The app
default-collapses tool calls (read_file, apply_patch, shell, ...) and
reasoning blocks into cards in the side rail, surfacing only the
agent's plain-text replies in the main thread. To make your work
readable WITHOUT requiring the viewer to expand every tool card,
follow this narration rule:

- BEFORE each non-trivial tool call (file read, patch apply, shell
  command, web fetch), emit ONE short plain-text sentence saying what
  you are about to do and why.
- AFTER each non-trivial tool call, emit ONE short plain-text sentence
  summarizing the result (e.g. "Found the Windows section starting at
  line 128." or "Patch applied to CodexProvider.cpp; 1 hunk, no
  rejects.").
- Trivial repeats (e.g. paging through the same file a second time)
  do NOT need narration.
- These narration sentences exist solely so a Desktop viewer reading
  the main thread can follow end-to-end without clicking tool cards.
  Keep them concise; do not summarize file contents inline that the
  viewer can read by expanding the card.

## Sandbox tier (choose before dispatch)

- `read-only`: for pure-query tasks that read files / git history and report.
  No edits, no shell side-effects.
- `workspace-write` (default for code/docs work): for tasks that ONLY edit files
  inside the repo cwd and don't shell out to long-running builds or process
  management. Most refactor / bug-fix / docs tasks.
- `danger-full-access`: required for UE build / editor smoke / verification
  tasks. UBT incidentally writes `<engine>/Intermediate/` and
  `~/Library/Application Support/Epic/UnrealBuildTool/`; process management
  needs `pgrep`/`kill`. The reviewer (Claude as PM) is responsible for
  declaring this tier explicitly when dispatching such tasks; codex must NOT
  assume it without the prompt saying so.
  Under danger-full-access codex still must obey all source-code read-only /
  no-commit / no-push constraints from this header. Sandbox tier is only an
  OS-level write allowance -- it doesn't license violating workflow rules.

## UE editor smoke test pattern

- Pre-step: `rm -f <Project>/Saved/Logs/<Project>.log` before launch so any log
  you read is fresh.
- Launch FOREGROUND with `-AbsLog=/tmp/<job>-<project>.log` so engine output
  lands at a known path immediately. Background `&` under codex's shell can
  orphan the child and produce no log file. Example invocation:
```
'/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor' \
  "$(pwd)/<Project>.uproject" -AbsLog=/tmp/<job>-smoke.log
```
- Boot markers to poll for, in priority order: `Engine is initialized`,
  `LogInit: Engine initialized`, or any `LogUnrealMcp:` line. Poll every ~5s;
  give up to 10 minutes total.
- Shutdown: `kill -INT <pid>` is UE's graceful quit. Wait up to 60s. Escalate
  to `kill -TERM` if needed; never `kill -KILL` unless every other path failed.
- Cleanup invariant: at end of task, `pgrep -fl /UnrealEditor` must return
  nothing. If it doesn't, that's a SMOKE failure regardless of other markers.

## Self-extension workflow (when adding new MCP tools)

When the task asks for a new MCP tool, NEVER hand-edit these files:

- `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcp*Tools.cpp`
  (category dispatcher + handler functions live here; they are populated by
  the toolchain on apply, not by hand)
- `Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpToolRegistrar.cpp`
  (descriptor entries; same as above)
- `Tools/UnrealMcpToolRegistry/tools.json`
  (registry; same as above; the mirror at
  `Plugins/UnrealMcp/Resources/ToolRegistry/tools.json` is a symlink)
- `Tools/UnrealMcpTests/Core/*.json`
  (test fixtures for new tools are created by the toolchain, not hand-written
  to match a hand-merged handler)

The canonical workflow is:

1. `unreal.scaffold_mcp_tool` -- produces a scaffold dir under
   `Tools/UnrealMcpToolScaffolds/<toolId>/` with ScaffoldMetadata.json,
   ToolRegistryPatch.json, four `.patch.cpp` fragments, TestRequest.json,
   and human-readable docs. The scaffold dir is local-only (gitignored);
   cross-developer transfer happens via `unreal.tools.export_package` later.
2. Validate: `unreal.mcp_validate_cpp_patch` + `unreal.mcp_validate_tool_schema`.
3. Apply + build + test in one orchestrated call:
   `unreal.mcp_extension_pipeline` (preferred) or step-by-step
   `unreal.mcp_apply_scaffold` -> `unreal.mcp_build_editor` -> `unreal.mcp_run_tool_test`.
   The pipeline writes a manifest under
   `Saved/UnrealMcp/ExtensionBackups/<timestamp>_<toolId>/Manifest.json`
   with backup snapshots, per-file hashes, and rollback metadata.
4. Reviewer (Claude as PM) reviews `git status` AFTER the pipeline runs and
   commits the staged changes; commit message names the toolId.
5. If cross-developer transfer is needed, `unreal.tools.export_package`
   produces a zip including scaffold + tests + docs + registry entry +
   SHA-256 manifest. The output zip lives under
   `Saved/UnrealMcp/Packages/<toolName>-<version>.zip` (gitignored); transfer
   is by file sharing, not by committing the zip into the repo.

**Scaffold location is PROJECT-LOCAL by design** (CATEGORY B per the original
architecture; see recovered prompt 2026-05-12 02:45). Scaffolds land at
`<ProjectDir>/Tools/UnrealMcpToolScaffolds/<toolId>/` where `<ProjectDir>` is
whichever `.uproject` the editor was launched against:

- Editor on `UEvolve.uproject` (dev mode): `<ProjectDir>` = repo root, so
  scaffold lands at `<repoRoot>/Tools/UnrealMcpToolScaffolds/`.
- Editor on `Examples/UEvolveExample57/UEvolveExample57.uproject`:
  `<ProjectDir>` = example project, so scaffold lands at
  `Examples/UEvolveExample57/Tools/UnrealMcpToolScaffolds/`.

### Self-extension path resolution domains

- READER domain (versioned `Tools` assets such as PyTools, registry source,
  scaffold recipes, skills, tests, and knowledge): use the Tools reader
  resolver. Project-local `Tools` may override shared repo-root `Tools`; always
  surface `sourceKind` and `candidates` in structured results when a reader
  path is user-visible. Readers of scaffold files check
  `Tools/UnrealMcpToolScaffolds/<id>` first (writable working copy,
  project-local then walked-up), then fall back to
  `Tools/UnrealMcpToolScaffoldStarters/<id>` (canonical committed starter,
  read-only) for fresh clones or unmodified recipe applies.
- WRITER domain (generated drafts, session output, backups, manifests, logs,
  and locks): use `<ProjectDir>` / `<ProjectSavedDir>` paths such as
  `ResolveProjectOutputDirectory`. Do NOT use SharedPathResolver or walk up to
  the repo root. Walking up breaks the per-project draft isolation that lets
  each editor session iterate on its own scaffold drafts.
- PLUGIN SOURCE domain (plugin C++ source and plugin Resources writes): use
  `IPluginManager::FindPlugin("UnrealMcp")->GetBaseDir()` through
  `ResolvePluginSourceRoot()` / `ResolvePluginBaseDir()`. Never assume
  `<ProjectDir>/Plugins/UnrealMcp`; example projects can load the plugin
  through `AdditionalPluginDirectories`.
- Apply scaffold crosses all three domains: READ scaffold content through the
  READER domain, WRITE `.cpp` / `.h` source through the PLUGIN SOURCE domain,
  and WRITE backups/manifests/logs through the WRITER domain.

Hand-merging a scaffold's `.patch.cpp` content into the main module DOES NOT
create a manifest. The tool will run, but the toolchain cannot roll it back,
the inspect tools cannot see its applied state, and any future `apply` or
`import_package` on the same toolId will conflict. PMs and codex agents have
repeatedly done this by accident; this section exists to stop it.

Out-of-band exception: if the task is to build a brand-new category
dispatcher (i.e. adding `UnrealMcpFooTools.cpp` for a category that does
not yet exist), manual edits to the category file are unavoidable. Flag
this explicitly in the task brief, and after landing, create retroactive
rollback snapshots via `unreal.mcp_backup_project_state` so the toolchain
has a baseline.

## Final report (always required at the end of the task)

1. Full unified diff of your changes (one fenced block per file).
2. `git status --short` (excluding pre-existing unrelated untracked items).
3. AGENTS.md "Documentation Freshness Rule" evaluation: yes (with the
   edits included) or no (with one-line justification).
4. Any out-of-scope gaps you noticed but DID NOT fix — list them so the
   reviewer can decide on follow-up tasks.

---
