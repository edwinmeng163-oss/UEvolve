# Windows Compatibility Lessons (UE 5.6.1)

> Hard-won knowledge from the **issue #2 saga (2026-05-16 → 2026-05-17)**: 4
> Windows tester retest rounds + a packaging follow-up + a schema-symlink
> follow-up. Six commits, five "tiers", one production Windows release.
>
> This doc exists so the next person debugging "it works on my Mac but not on
> Windows" can grep for symptoms instead of re-deriving the diagnosis. Each
> entry: **Symptom · Root cause · Fix · Prevention/test**.

## Quick index

| # | Class | Symptom (Windows side) | Fix commit |
|---|---|---|---|
| 1 | C++ compile | UE 5.6 MSVC C4459 `LogType` shadow → error | `57ce634` |
| 2 | C++ compile | UE 5.6 MSVC misses `#include` that clang resolved transitively | various |
| 3 | Python bridge | `__import__` hook rejects `fromlist=` kwarg | `e08a995` (and earlier in Tier 2) |
| 4 | Python bridge | stdlib import blocked by `pythonImportAllowList` | `57ce634` |
| 5 | Path resolution | `<ProjectDir>/Tools/...` empty on example-host mode (AdditionalPluginDirectories) | `fe65d25` |
| 6 | Path resolution | `<ProjectDir>/Plugins/UnrealMcp/...` wrong when plugin is external | `fe65d25` |
| 7 | Path resolution | Apply scaffold can't find `fps_bootstrap` on fresh clone (gitignored Scaffolds) | `9fd70ac` |
| 8 | Build hygiene | Stale plugin-level dylib shadows fresh UBT output | (documented, no code fix) |
| 9 | Git cross-platform | `Schemas/UnrealMcpToolRegistry.schema.json` symlink → 42-byte stub on Win | `8917f99` |
| 10 | Example projects | `.uproject` hard-requires third-party plugin (`AIAssistant`) | `57ce634` |
| 11 | Example projects | Scaffold metadata `dependsOn` drift between Starters and Scaffolds | `57ce634` |
| 12 | Codex bridge | bundled `runtime/bun.exe` ignored, launcher uses PATH | `e08a995` |
| 13 | Codex bridge | `where codex` returns WindowsApps path → Bun `uv_spawn EPERM` | `e08a995` |
| 14 | Codex bridge | Codex app-server rejects `--listen ws://...` | `e08a995` |
| 15 | Codex bridge | Win launcher doesn't quote paths with spaces | `e08a995` |
| 16 | Tool quality | `project_settings_get` returns config default ignoring PIE override | `088d056` |
| 17 | Tool quality | `bp_list_graph_nodes` rejects short package path | `088d056` |
| 18 | Tool quality | `tail_log` single-candidate read fails flaky | `088d056` |
| 19 | Tool quality | `save_dirty_packages` returns `saved=false` with no skip reason | `088d056` |
| 20 | Tool quality | `capture_project_snapshot` silently misses actors, diff looks insane | `088d056` |

---

## 1. UE 5.6 Windows MSVC C4459 `LogType` shadow

**Symptom**: UBT on Win64 UE 5.6 fails with C4459 (declaration shadows global) being treated as error. The shadowed name is `LogType` — a global `FName` defined in UE's `UObject/UnrealType.h`. Mac clang issues no warning so it slipped through every Mac build.

**Root cause**: A local variable named `LogType` inside `UnrealMcpPythonToolBridge.cpp`'s `CollectPythonCommandOutput`.

**Fix** (`57ce634`): Rename local to `LogEntryType`. Added a code comment near the rename pointing at this lesson.

**Prevention**:
- Any local variable named `LogType` / `LogCategory` / similar UE-ubiquitous global names is a smell.
- `check_ue56_compat.py` would catch this if we added a denylist, but the file currently focuses on `#if ENGINE_*_VERSION` placement, not local-name shadows. Future improvement: add C4459-style scan.
- **Always do a Windows UE 5.6 build before declaring a C++ change "ready"** — Mac/clang silently accepts code that Win/MSVC rejects.

## 2. UE 5.6 MSVC needs explicit `#include` that clang resolved transitively

**Symptom**: `'AGameModeBase' incomplete type 'AGameModeBase' named in nested name specifier` when using `TSubclassOf<AGameModeBase>` after pulling in `GameFramework/WorldSettings.h` but NOT `GameFramework/GameModeBase.h`. Mac clang resolves it through some transitive include chain.

**Root cause**: PowerShell/MSVC stricter about transitive includes than clang on macOS.

**Fix** (Tier 5 PM fix on top of `088d056`): explicit `#include "GameFramework/GameModeBase.h"` in `UnrealMcpEditorTools.cpp` before using `WorldSettings->DefaultGameMode.Get()`.

**Prevention**:
- When using `TSubclassOf<X>` or any UE class member access on a typedef'd subclass, add the include for `X` even if you think a parent header pulls it in.
- This is a recurring class of bug — also expect `FProperty`, `UDeveloperSettings`, `IConsoleManager` to need their own explicit includes on MSVC.

## 3. Python `__import__` hook signature must match CPython kwargs

**Symptom**: Calling any Python stdlib function that internally re-imports (e.g. `platform.platform()` calls `__import__('subprocess', ..., fromlist=...)`) raises `_unreal_mcp_import_hook() got an unexpected keyword argument 'fromlist'`.

**Root cause**: Our `__import__` hook signature used internal names `(name, _globals=None, _locals=None, _fromlist=(), _level=0)` (underscore prefix). CPython's actual `__import__(name, globals=None, locals=None, fromlist=(), level=0)` callers — including stdlib — pass `fromlist=` by keyword.

**Fix** (`e08a995` and earlier in Tier 4): rename hook kwargs to match CPython exactly (no underscore prefix).

**Prevention**:
- Any custom `__import__` hook MUST mirror CPython's standard signature character-for-character. The `level=0` final arg is also kwarg-callable.
- Test the hook against `platform.platform()` — it's the most common stdlib function that exercises kwarg passthrough.

## 4. Python stdlib imports blocked by `pythonImportAllowList`

**Symptom**: A Python tool that calls `platform.platform()` fails with import-blocked error pointing at `subprocess` (which `platform` internally needs on POSIX/macOS) or `os.path` etc.

**Root cause**: Tool's `pythonImportAllowList` in `Tools/UnrealMcpToolRegistry/tools.json` only listed `["unreal"]`. Stdlib transitive imports (`sys`, `platform`, `subprocess`, `os`) all need explicit allow.

**Fix** (`57ce634`): `unreal.editor.python_runtime_info` allow-list became `["unreal", "sys", "platform", "subprocess"]`.

**Prevention**:
- For any new Python-track tool, enumerate every stdlib module the script transitively touches (use `python -c 'import sys; ...; print(sorted(sys.modules))'` to dump).
- Add a CI check that runs Python-track tools' `main.py` and dumps `sys.modules` keys, then asserts they're a subset of `pythonImportAllowList`. (Not yet implemented; tracking as backlog.)

## 5. `<ProjectDir>/Tools/...` is wrong path on example-host mode

**Symptom**: `unreal.editor.python_runtime_info` and `unreal.mcp_apply_scaffold` report "handler file not found" / "scaffold metadata not found" when the active project is `Examples/UEvolveExample57.uproject` (which loads the plugin via `AdditionalPluginDirectories: ["../../Plugins"]`).

**Root cause**: Pre-Tier-2 code used `FPaths::ProjectDir()` to resolve `Tools/...`, which under example-host mode points at `Examples/UEvolveExample57/Tools/` (empty, no canonical content), not the repo root `Tools/` (where canonical lives).

**Fix** (`fe65d25`): Four-domain Path Resolution Policy (AGENTS.md § "Path Resolution Policy"):
- **READER** (Tools/...): new `ResolveToolsReadSubpath` walks up via `IPluginManager::FindPlugin("UnrealMcp")->GetBaseDir()` anchor.
- **WRITER** (project-local drafts): unchanged, stays in `<ProjectDir>/Tools/UnrealMcpToolScaffolds/<id>`.
- **PLUGIN SOURCE** (apply target): new `ResolvePluginSourceRoot` via `IPluginManager`.
- **SAVED** (`Saved/UnrealMcp/`): unchanged, `FPaths::ProjectSavedDir()`.

Reader returns structured `FToolsReadResolution {Path, bFound, SourceKind (ProjectLocal / SharedRepoRoot / CanonicalStarter / PluginResources), Candidates, Warning}` so callers can surface in tool response which candidate won.

**Prevention**:
- Any new code touching `Tools/...` or `Plugins/UnrealMcp/...` paths MUST use the four-domain resolver, not `FPaths::ProjectDir()` directly. AGENTS.md § "Path Resolution Policy" is the canonical contract.
- Pure-function tests at `Plugins/UnrealMcp/Source/UnrealMcp/Private/Tests/UnrealMcpSharedPathResolverTests.cpp` cover 9 cases including example-host mode + 5 root-host invariants.
- Test example-host smokes (`Examples/UEvolveExample57/UEvolveExample57.uproject`) explicitly — they catch this regression class.

## 6. Plugin source directory must come from `IPluginManager`, not `<ProjectDir>`

**Symptom**: Apply scaffold writes `.cpp` patches to `<ProjectDir>/Plugins/UnrealMcp/Source/...` which doesn't exist when the plugin is mounted via `AdditionalPluginDirectories`. Write fails or, worse, succeeds writing to a wrong path.

**Root cause**: Same as #5 — `FPaths::ProjectDir()` assumption.

**Fix** (`fe65d25`): `ResolvePluginSourceRoot()` returns `IPluginManager::Get().FindPlugin("UnrealMcp")->GetBaseDir() + "/Source"`. Always correct regardless of which `.uproject` loaded the plugin.

**Prevention**: see #5.

## 7. Fresh-clone scaffold lookup needs `Starters` fallback

**Symptom**: On a Win UE 5.6 fresh clone (no prior MCP edits), `unreal.mcp_apply_scaffold {toolName: "unreal.fps.bootstrap"}` reports "scaffold not found" listing 5 walked-up candidates under `Tools/UnrealMcpToolScaffolds/fps_bootstrap` — none exist because that whole tree is `.gitignore`d.

**Root cause**: `/Tools/UnrealMcpToolScaffolds/` is gitignored by design (per-project draft isolation, CATEGORY B). The actually-committed canonical scaffold sources are under `Tools/UnrealMcpToolScaffoldStarters/<id>/`. Tier 2 reader only checked Scaffolds, not Starters.

**Fix** (`9fd70ac`): `ResolveScaffoldReadDirectory` now adds two more candidates after the Scaffolds candidates:
1. `<ProjectDir>/Tools/UnrealMcpToolScaffoldStarters/<toolId>/`
2. walked-up `Tools/UnrealMcpToolScaffoldStarters/<toolId>/`

New `CanonicalStarter` value on `FToolsReadResolution::ESource` lets responses surface which trust domain the scaffold came from. Writer side (`ResolveProjectOutputDirectory`) untouched — drafts still land at project-local Scaffolds dir.

**Prevention**:
- Any new "find canonical X" reader must check **committed** locations (Starters / Resources) as fallback after **draft** locations (Scaffolds / Saved).
- E2E fixture `Tools/UnrealMcpTests/SelfExtension/31_apply_scaffold_dryrun_starter_fallback.json` asserts the resolver picks Starters when Scaffolds is absent.

## 8. Stale plugin-level dylib shadows fresh UBT output

**Symptom**: After editing plugin C++ code and running UBT against an example host, the editor's runtime responses still look exactly like pre-edit behavior. New structured-content fields you just added are missing from responses. You're confused because UBT reported `Result: Succeeded`.

**Root cause**: UBT writes the freshly-built dylib to `Examples/<X>/Binaries/<Platform>/`, but the editor mount path for the externally-loaded plugin keeps loading the OLD `Plugins/UnrealMcp/Binaries/<Platform>/UnrealEditor-UnrealMcp.{dylib,dll}` from a previous dev-host build (e.g. when you originally ran the root `UEvolve.uproject`).

**Fix** (no code change; documented as canonical practice in AGENTS.md § "Stale plugin-level binary trap"): **before any example-host smoke after a plugin code change**:

```bash
# macOS
rm -rf Plugins/UnrealMcp/Binaries Plugins/UnrealMcp/Intermediate
"<UE>/Engine/Build/BatchFiles/Mac/Build.sh" MyProjectEditor Mac Development \
  -project="$REPO_ROOT/Examples/UEvolveExample57/UEvolveExample57.uproject" -WaitMutex
```

```pwsh
# Windows
Remove-Item -Recurse -Force Plugins\UnrealMcp\Binaries, Plugins\UnrealMcp\Intermediate -ErrorAction SilentlyContinue
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" `
  MyProjectEditor Win64 Development `
  -Project="$pwd\Examples\UEvolveExample\UEvolveExample.uproject" `
  -WaitMutex
```

Diagnostic signal: runtime response of a tool you just changed lacks any new structured-content key you added in the patch.

**Prevention**:
- Treat `rm -rf Plugins/UnrealMcp/Binaries Plugins/UnrealMcp/Intermediate` as a **mandatory pre-smoke step** any time you UBT-rebuild against a different `.uproject` than the one you last built.
- The AGENTS.md Release Verification SOP has a sub-section "Stale plugin-level binary trap" with the canonical cleanup snippet.

## 9. `Schemas/UnrealMcpToolRegistry.schema.json` git symlink doesn't survive Win clone

**Symptom**: `Tools/package_plugin.ps1` on Windows fails with "schema differs from canonical" — `Schemas/UnrealMcpToolRegistry.schema.json` is 42 bytes (literal text `../Tools/UnrealMcpToolRegistry/schema.json`) instead of the real 5074-byte JSON. Hash comparison fails.

**Root cause**: The file was a git symlink (`120000` mode) pointing at the canonical schema. macOS git transparently follows it; Windows git defaults to `core.symlinks=false` and materializes the symlink as a text stub. Even with `core.symlinks=true`, only Developer Mode lets non-admin Windows accounts create symlinks at clone time.

**Fix** (`8917f99`): replace symlink with a real file byte-identical to the canonical. Git diff shows `mode change 120000 => 100644`. Drift detection lives at `Tools/validate_tool_registry.py:326` (`SCHEMA_ALIAS_PATH.read_bytes() == SCHEMA_PATH.read_bytes()` → `issueCount` bump on divergence). Verified by deliberately corrupting the alias and confirming validator catches.

**Prevention**:
- **No git symlinks in this repo, ever.** `git ls-files -s | awk '$1=="120000"'` should always return empty.
- If you need two files to share content, ship them as two real-file copies and add a validator byte-equality check.
- `.gitattributes` defenses (`* binary` or symlink-specific) are NOT enough; symlinks at the git-object level still pull through.

## 10. Example `.uproject` hard-requires `AIAssistant` plugin

**Symptom**: Win clone, no `AIAssistant` plugin installed (it's third-party / not bundled with stock UE). Editor refuses to load the project with "Failed to load plugin AIAssistant: plugin not found".

**Root cause**: `Examples/UEvolveExample{,57}/.uproject` had:

```json
{ "Name": "AIAssistant", "Enabled": true }
```

The plugin is useful when present but isn't required for UEvolve itself.

**Fix** (`57ce634`):

```json
{ "Name": "AIAssistant", "Enabled": true, "Optional": true }
```

UE silently skips optional plugins that aren't installed.

**Prevention**:
- Any plugin in an example `.uproject`'s `Plugins[]` that isn't in `Engine/Plugins/` or `<repo>/Plugins/` should have `"Optional": true`.
- Mac users with the plugin installed still get its functionality; Win/Linux users without it can still open the project.

## 11. Scaffold metadata `dependsOn` drift between Starters and Scaffolds

**Symptom**: `apply_scaffold dryRun` succeeds on the Mac dev host but fails on Win fresh clone with "dependency chain incomplete" — even though both look at the same `fps_bootstrap` toolId.

**Root cause**: Two copies of `ScaffoldMetadata.json` existed:
- `Tools/UnrealMcpToolScaffoldStarters/fps_bootstrap/ScaffoldMetadata.json` (canonical, in git) with `dependsOn: []`
- `Tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json` (working copy, `.gitignore`d) with `dependsOn: ["unreal.simulation.verify_input_drives_pawn"]`

The Tier 2 path resolver fell back to whichever one existed, so the Mac dev host (which had the working copy from a prior session) saw a different dep chain than fresh-clone Win.

**Fix** (`57ce634`): synced `Tools/UnrealMcpToolScaffoldStarters/fps_bootstrap/ScaffoldMetadata.json` `dependsOn` to match the working copy. Future scaffold edits should update BOTH copies if both exist.

**Prevention**:
- When editing a scaffold metadata under `Tools/UnrealMcpToolScaffolds/<id>/` (working copy), also update the matching Starter under `Tools/UnrealMcpToolScaffoldStarters/<id>/`.
- Pre-commit hook could enforce byte-equality between Starters and a (locally-existing) working copy; not currently implemented.

## 12-15. Codex App Server bridge (issue #2 comment 8)

Tier 4 (`e08a995`) fixed four bridge issues simultaneously. See full diagnosis in github.com/edwinmeng163-oss/UEvolve/issues/2#issuecomment-... (by edwinmeng163-oss, 2026-05-16T23:18Z):

### 12. Bundled `runtime/bun.exe` ignored

**Symptom**: `start-bridge.ps1` / `.cmd` fail with "bun: command not found" on a fresh Win box that doesn't have Bun on PATH, even though the projectroot zip ships `Tools/UnrealMcpCodexBridge/runtime/bun.exe`.

**Fix**: launchers now prefer `$PSScriptRoot\runtime\bun.exe` / `%~dp0runtime\bun.exe` if present, fall back to `$env:UEVOLVE_BUN_BIN`, then PATH `bun`.

### 13. WindowsApps Codex path → `uv_spawn EPERM`

**Symptom**: `where codex` returns `C:\Program Files\WindowsApps\OpenAI.Codex_<ver>\app\resources\codex.exe`. Bun's `child_process.spawn` against that path fails with `EPERM uv_spawn` because the WindowsApps store namespace is protected.

**Fix**: Codex binary discovery order:
1. `UEVOLVE_CODEX_BIN` env override
2. `%LOCALAPPDATA%\OpenAI\Codex\bin\<hash>\codex.exe` (user-mode install)
3. `where codex` fallback
4. Reject any candidate under `C:\Program Files\WindowsApps\` with structured warning

### 14. Codex app-server `--listen ws://...` rejected

**Symptom**: Bridge spawns `codex app-server --listen ws://127.0.0.1:8766` and Codex exits with error: `unsupported --listen URL`; only `stdio://`, `unix://`, `unix://PATH`, or `off` are accepted.

**Fix**: Bridge transport widened to `"unix" | "ws" | "stdio"`. Windows default is now `stdio` (newline-delimited JSON over child stdin/stdout). macOS/Linux default stays `unix`. **The UE-facing inbound listener stays WebSocket** (`ws://127.0.0.1:8766/uevolve`) regardless of outbound transport.

### 15. Win paths with spaces don't survive cmd/PowerShell

**Symptom**: `start-bridge.cmd` invoked from `C:\Users\edwin\OneDrive\ドキュメント\Unreal Projects\...` splits on the space in "Unreal Projects" → can't find the bridge script.

**Fix**: Every path argument in `start-bridge.{ps1,cmd}` is now quoted. Same fix in `start-bridge.sh` for symmetry.

**Prevention** (covers all 12-15):
- Test the bridge on a clean Win box with no Bun on PATH, no `UEVOLVE_*` env overrides, default user-mode Codex install, and a project path containing a space. If all four work without manual config, the bridge is OK.
- Document the policy in `Tools/codex-prompt-header.md` § "Path resolution / Plugin source domain" extension.

## 16. `project_settings_get` returns config default ignoring PIE override

**Symptom**: `unreal.project_settings_get {category: "game", key: "GlobalDefaultGameMode"}` returns `/Script/Engine.GameModeBase` but the actual running PIE world is using `BP_FPSGameMode`.

**Root cause**: Tool read `UDeveloperSettings`/`GConfig` default value, ignoring runtime PIE override that lives in `GEditor->PlayWorld->GetWorldSettings()->DefaultGameMode`.

**Fix** (`088d056`): added `effective: bool` input flag (default false to preserve old behavior). When `effective=true` AND editor is in PIE, reads the PIE world settings. Structured content adds `effective` (echo), `runtimeSource` (`pie_world_settings` / `developer_settings` / `ini_section`), `defaultValue` (when divergent).

**Prevention**:
- Any settings-read tool should expose default-vs-effective when UE has a runtime override path.
- Document in tool description so callers know to ask for `effective=true` when they care about runtime state.

## 17. `bp_list_graph_nodes` rejects short package path

**Symptom**: `unreal.bp_list_graph_nodes {blueprintPath: "/Game/FPS/BP_FPSCharacter"}` fails with "asset not found"; the same call with `/Game/FPS/BP_FPSCharacter.BP_FPSCharacter` (full object path) works.

**Root cause**: `LoadAssetFromAnyPath` passed the package path to `LoadObject<>` which expects a full object path. UE asset paths are `<package>.<objectName>` and `<objectName>` defaults to the trailing path segment.

**Fix** (`088d056`): added `NormalizeBlueprintAssetPath` helper. If `InputPath` starts with `/Game/`/`/Engine/`/`/Script/` AND has no `.`, append `.<lastSegment>`. Wired into `LoadAssetFromAnyPath` so all Blueprint tools benefit, not just `bp_list_graph_nodes`.

**Prevention**:
- Any tool that takes an "asset path" input should normalize package → object form before `LoadObject`.
- AI clients commonly emit package paths because that's what the Content Browser displays.

## 18. `tail_log` single-candidate read is flaky

**Symptom**: `unreal.tail_log` intermittently returns "log file not found" even though the editor is clearly writing logs.

**Root cause**: `FGenericPlatformOutputDevices::GetAbsoluteLogFilename()` returns the path UE registered at boot, but `Saved/Logs/` rotation can move the file between boot and the read.

**Fix** (`088d056`): 4-candidate fallback chain:
1. raw absolute log filename
2. normalized + `ConvertRelativePathToFull` of (1)
3. `<ProjectLogDir>/<ProjectName>.log`
4. newest `*.log` under `ProjectLogDir`

Structured content adds `attemptedLogPaths`, `selectedLogPath`, `rawLogPath`. Error case includes the full candidate list for diagnosis.

**Prevention**: any log/state file read in the editor should have at least 2 candidates with a "find newest" fallback.

## 19. `save_dirty_packages` returns `saved=false` with no explanation

**Symptom**: User saves project, gets `saved=false`. No indication which package failed or why. Tester thought the tool was broken when it was actually correctly refusing to save a `/Temp/Untitled_1` placeholder map.

**Root cause**: Tool only returned the `saveDirtyPackages` boolean, which is false when ANY package can't be saved (e.g. unnamed maps).

**Fix** (`088d056`): enumerate dirty packages before + after save; classify remaining-dirty ones by `skipReason`:
- `TEMP_OR_UNSAVED_MAP` — names start with `/Temp/`, no on-disk file
- `READONLY` — source-control read-only
- `UNKNOWN` — anything else

Structured content adds `dirtyPackagesBefore`, `skippedPackages: [{path, reason}]`, `savedPackages` (count). Text payload calls out the dominant skipReason when `saved=false`.

**Prevention**: any boolean-result tool that can fail for multiple reasons MUST return structured per-item reasons, not just the boolean.

## 20. `capture_project_snapshot` silently misses actors, diff looks insane

**Symptom**: `diff_project_snapshot` reports `before=0 actors, after=201`. The "before" snapshot was taken in a non-empty world so 0 is implausible.

**Root cause**: `AddActorSnapshotArray` skipped actor capture when `UEditorActorSubsystem` was unavailable (some editor states), but the result still set `actors: []`. Diff treated the empty array as a real "0 actors" instead of "couldn't capture".

**Fix** (`088d056`): added `actorCaptureAvailable` / `actorCaptureStatus` (`captured` / `empty_world` / `subsystem_unavailable` / `iterator_fallback`) / `actorSnapshotCount` to snapshot. Diff adds `beforeAvailable` / `afterAvailable` / `comparable` flag. When either side has `actorCaptureAvailable=false`, diff stops reporting added/removed totals as meaningful and surfaces a `caveat`. New `TActorIterator<AActor>` fallback walks the editor world when subsystem is unavailable.

**Prevention**: any snapshot/diff tool MUST distinguish "absent because zero" from "absent because not captured". Use an explicit `available` flag, not the array length.

---

## Cross-cutting defenses

### Validators that catch regressions

- **`Tools/validate_tool_registry.py`** — checks canonical tools.json + plugin mirror byte-equality, schema mirror byte-equality, schema alias byte-equality, dispatch coverage, schema validity. Run before every commit that touches the registry. See line 326 for the schema-symlink defense added after `8917f99`.
- **`Tools/check_ue56_compat.py`** — scans for `#if ENGINE_*_VERSION` outside `UnrealMcpEngineCompat.h`. Add new compat-related scans (LogType-shadow denylist, missing-include patterns) here if a recurring class shows up.

### Required pre-dispatch / pre-PR checks for Windows-affecting changes

1. `python3 Tools/validate_tool_registry.py` — issueCount=0
2. `python3 Tools/check_ue56_compat.py` — 0 errors / 0 warnings
3. `cmp -s Tools/UnrealMcpToolRegistry/tools.json Plugins/UnrealMcp/Resources/ToolRegistry/tools.json` — byte-identical
4. `cmp -s Tools/UnrealMcpToolRegistry/schema.json Schemas/UnrealMcpToolRegistry.schema.json` — byte-identical (the schema-symlink defense)
5. `git ls-files -s | awk '$1=="120000"'` — empty (no symlinks)
6. UE 5.7 Mac UBT build against example host — `Result: Succeeded`
7. UE 5.6 Windows UBT build against example host — `Result: Succeeded` (if you have a Win box; otherwise hand off to a Windows tester before declaring done)
8. If touching Python bridge: confirm `pythonImportAllowList` covers all stdlib transitively imported modules; confirm `__import__` hook signature matches CPython kwargs

### Required Windows e2e smoke set (post-build, per release)

The canonical example-host smoke is run from a fresh clone with `Examples/UEvolveExample/UEvolveExample.uproject` (UE 5.6) or `UEvolveExample57.uproject` (UE 5.7). Three smokes:

1. `tools/list` → expect >= 110 visible tools, includes `unreal.editor.python_runtime_info` + `unreal.mcp_apply_scaffold`
2. `unreal.editor.python_runtime_info {}` → expect `isError=false`, `pythonHandlerSourceKind` in (`SharedRepoRoot`, `ProjectLocal`)
3. `unreal.mcp_apply_scaffold {toolName: "unreal.fps.bootstrap", dryRun: true}` → expect `isError=false`, `canApply=true`, `scaffoldSourceKind` in (`SharedRepoRoot`, `CanonicalStarter`, `ProjectLocal`)

If smoke 2 fails on a clean clone, see #5 / #6. If smoke 3 fails, see #5 / #7. If bridge smoke (port 8766) fails, see #12-15.

### Stale plugin-level binary cleanup (MANDATORY pre-smoke)

Before any example-host smoke after a plugin code change:

```pwsh
# Win
Remove-Item -Recurse -Force Plugins\UnrealMcp\Binaries, Plugins\UnrealMcp\Intermediate -ErrorAction SilentlyContinue
# (then rebuild)
```

```bash
# Mac
rm -rf Plugins/UnrealMcp/Binaries Plugins/UnrealMcp/Intermediate
# (then rebuild)
```

See #8 for the diagnostic signal (runtime response missing your new structured-content fields).

---

## Commit reference index

| Tier | Commit | Lessons covered |
|---|---|---|
| 1 | `57ce634` | #1, #4, #10, #11 |
| 2 | `fe65d25` | #3 (partial), #5, #6 |
| 3 | `9fd70ac` | #7 |
| 4 | `e08a995` | #3 (complete), #12, #13, #14, #15 |
| 5 | `088d056` | #2, #16, #17, #18, #19, #20 |
| — | `8917f99` | #9 |

## Public releases shipped from this saga

- `v0.14.0-python-track-mac` (macOS UE 5.6/5.7, source-only projectroot zip, ~542 KB, SHA `0eaf8c8f...`) — published 2026-05-16
- `v0.14.0-python-track` (Windows UE 5.6.1, full-experience with prebuilt Win64 binaries, ~75 MB, SHA `9f80bdde2f66889415e672662ceecd27cd9eeb02f586bcbef87e9320e74c8338`) — published 2026-05-17

## Issue thread

[github.com/edwinmeng163-oss/UEvolve/issues/2](https://github.com/edwinmeng163-oss/UEvolve/issues/2) — Windows UE 5.6 validation report. Closed after 4 retest rounds + 1 schema-symlink follow-up. Comments preserve the diagnostic trail.

---

# Codex Desktop bridge troubleshooting (Windows)

> **用 v0.14.0-python-track Windows full-experience zip (SHA `9f80bdde...`) 的用户，
> Codex Desktop / Chat 面板报 "Failed to connect to Codex App Server bridge at
> ws://127.0.0.1:8766/uevolve" 时按这个顺序排查。**
>
> Tier 4 (commit `e08a995`) 已经把已知的所有 Windows-specific failure modes 修了，
> 但用户机器可能还有 environment-specific 状态（Codex 没装/没登录、端口占用、
> 自定义环境变量、自定义 Codex 安装路径等）。下面是按概率排序的手动 unblock 步骤。

## 0. 前提确认（5 秒）

```pwsh
# UE editor 是否在跑 + UnrealMcp 是否 listening on 8765
Get-Process -Name "UnrealEditor*" -ErrorAction SilentlyContinue | Select-Object Id, MainWindowTitle
Get-NetTCPConnection -State Listen -LocalPort 8765 -ErrorAction SilentlyContinue

# Bridge 是否在跑 + listening on 8766
Get-NetTCPConnection -State Listen -LocalPort 8766 -ErrorAction SilentlyContinue
```

期望：

- 至少 1 个 `UnrealEditor*` 进程在跑
- `127.0.0.1:8765` LISTEN（UE MCP）
- `127.0.0.1:8766` LISTEN（bridge）

如果 8765 没 LISTEN：UE editor 没启动 / UnrealMcp plugin 没启用 / 端口被占。打开 `Window > Unreal MCP Workbench` 确认插件已加载，看 `Output Log` 找 `LogUnrealMcp` 日志。

如果 8766 没 LISTEN：bridge daemon 没跑（最常见）。继续看下面。

## 1. 手动起 bridge — 最常见的 unblock

很多时候 bridge 没自动跟随 UE editor 启动。手动起一遍看错误：

```pwsh
cd "<UserProject>"  # 你解压 zip 的项目根
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

bridge 启动正常时输出大致：

```text
Bun version: 1.x.x
Codex binary: C:\Users\<you>\AppData\Local\OpenAI\Codex\bin\<hash>\codex.exe
unrealmcp already current in C:\Users\<you>\.codex\config.toml
Registered MCP server 'unrealmcp' with Codex; ...
UEvolve Codex Bridge listening at ws://127.0.0.1:8766/uevolve
Codex app-server transport=stdio endpoint=stdio://
Codex app-server args: app-server --listen stdio:// ...
```

健康检查（在另一个 PowerShell 窗口）：

```pwsh
# WebSocket 健康检查（用 PowerShell 7+ 自带的 client）
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ws.ConnectAsync([Uri]"ws://127.0.0.1:8766/uevolve", [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$ws.State  # 期望 Open
```

或更简单：浏览器开 http://127.0.0.1:8766/ — bridge 会返回简单 health text。

## 2. 各类常见错误 → 手动修复

### 2.1 `bun: command not found` 或类似

**症状**：bridge 启动报 "bun not recognized" 或 ".cmd 找不到 bun"。

**根因**：launcher 没找到 bundled `runtime/bun.exe`，回退到 PATH 也没有。

**修复**:

```pwsh
# 检查 bundled bun 是否存在
Get-Item Tools\UnrealMcpCodexBridge\runtime\bun.exe
# 期望: 存在，几十 MB 的 .exe

# 如果丢失：从 zip 重新解压 Tools\UnrealMcpCodexBridge\runtime\ 子目录
# 或者临时用环境变量指定别的 bun:
$env:UEVOLVE_BUN_BIN = "C:\path\to\your\bun.exe"
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

### 2.2 `EPERM: operation not permitted, uv_spawn` 指向 WindowsApps 路径

**症状**：

```text
Error: spawn EPERM
  errno: -4048,
  syscall: 'spawn',
  path: 'C:\Program Files\WindowsApps\OpenAI.Codex_<ver>\app\resources\codex.exe'
```

**根因**：`where codex` 返回了 WindowsApps store 路径，Bun 没权限 spawn 那个 protected namespace。Tier 4 已经加了自动 skip + 优先 user-mode install，但用户机器可能：

- 只装了 WindowsApps 版的 Codex（没装 user-mode）
- 或者 `UEVOLVE_CODEX_BIN` env var 显式指向了 WindowsApps 路径

**修复**:

```pwsh
# 检查现有 Codex 二进制
Get-ChildItem "$env:LOCALAPPDATA\OpenAI\Codex\bin" -Recurse -Filter codex.exe -ErrorAction SilentlyContinue
# 期望: 至少一个 user-mode codex.exe

# 如果只在 WindowsApps 下面：装 user-mode Codex
# 方案 A: 卸载 WindowsApps 版 → 装 Codex Desktop installer (从 OpenAI 官方)
# 方案 B: 保留 WindowsApps 版，但显式跑 codex 一次让它 cache 到 user-mode 路径
codex --version   # 第一次跑可能 trigger user-mode install
Get-ChildItem "$env:LOCALAPPDATA\OpenAI\Codex\bin" -Recurse -Filter codex.exe
# 再重启 bridge

# 如果你 UEVOLVE_CODEX_BIN 设错了：
Remove-Item Env:UEVOLVE_CODEX_BIN -ErrorAction SilentlyContinue
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

### 2.3 Codex app-server 报 `unsupported --listen URL` 拒绝 `ws://`

**症状**：

```text
error: invalid value 'ws://127.0.0.1:<port>' for '--listen <URL>':
unsupported --listen URL ...; expected `stdio://`, `unix://`, `unix://PATH`, or `off`
```

**根因**：用户显式设了 `UEVOLVE_CODEX_TRANSPORT=ws`，但当前 Codex 构建只支持 stdio。

**修复**:

```pwsh
Remove-Item Env:UEVOLVE_CODEX_TRANSPORT -ErrorAction SilentlyContinue
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1
# Bridge 应该自动选 stdio (Win 默认)
```

### 2.4 端口 8766 已被占用

**症状**：

```text
EADDRINUSE: address already in use 127.0.0.1:8766
```

**根因**：

- 之前一个 bridge 实例没干净退出
- 用户改了 `UEVOLVE_CODEX_BRIDGE_PORT` 跟另一个工具冲突
- 或者真有别的 daemon 占用 8766

**修复**:

```pwsh
# 看是谁占用
Get-NetTCPConnection -LocalPort 8766 -State Listen -ErrorAction SilentlyContinue |
  ForEach-Object {
    $proc = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
    "[$($_.OwningProcess)] $($proc.Name) $($proc.Path)"
  }

# 如果是 bun.exe / node.exe 的孤儿进程：kill
Stop-Process -Name bun -Force -ErrorAction SilentlyContinue
Stop-Process -Id <PID> -Force

# 或者换端口
$env:UEVOLVE_CODEX_BRIDGE_PORT = 8866
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1
# 注意：UE plugin provider 配置里的 BaseUrl 也要同步改
```

### 2.5 UE plugin 找不到 / 端口 8765 不 LISTEN

**症状**：UE editor 开了但 `Get-NetTCPConnection -LocalPort 8765` 返空。

**根因**：

- UnrealMcp plugin 没 enabled
- Plugin 装错位置（不在 `<UserProject>\Plugins\UnrealMcp\`）
- Plugin 编译失败但 editor 没报（rare）

**修复**:

```pwsh
# 确认 plugin 文件位置 + uplugin 启用
Get-Item "<UserProject>\Plugins\UnrealMcp\UnrealMcp.uplugin"
Get-Content "<UserProject>\Plugins\UnrealMcp\UnrealMcp.uplugin" | Select-String "Enabled"

# 项目的 .uproject Plugins 数组是否启用了 UnrealMcp
Get-Content "<UserProject>\<YourProject>.uproject" | Select-String -Pattern "UnrealMcp" -Context 0,3

# UE editor 启动后看输出
# Window > Output Log > 过滤 "LogUnrealMcp"
# 应该看到 "Unreal MCP listening on http://127.0.0.1:8765/mcp"
```

如果 plugin 真的没编译进来：编辑器右下角通常会有 "Plugin failed to build" 提示，让 UE 重新 Build 一次（或重启 editor 时勾选 "Yes" rebuild）。

### 2.6 UE 项目的 provider 配置 BaseUrl 错

**症状**：bridge 起来了、health 通了，但 UE Chat 面板还说连不上。

**根因**：`<UserProject>\Config\DefaultEngine.ini` 或 `Saved\Config\WindowsEditor\` 下 UnrealMcp provider 的 BaseUrl 不是 `ws://127.0.0.1:8766/uevolve`。

**修复**:

```pwsh
# 看现有配置
Select-String -Path "<UserProject>\Config\DefaultEngine.ini","<UserProject>\Saved\Config\WindowsEditor\Game.ini" -Pattern "BaseUrl"
```

应该有这样一行：

```ini
+Providers=(Id="codex",DisplayName="codex",Kind=CodexAppServer,BaseUrl="ws://127.0.0.1:8766/uevolve",ApiKey="",Model="gpt-5.5",ReasoningEffort="xhigh",MaxOutputTokens=4096,CodexBinaryPath="",CodexExtraArgs="")
```

如果 `BaseUrl` 是别的端口 / scheme 错 / 路径错：编辑 ini 改成上面这行，或者在 UE editor 里 `Edit > Project Settings > Unreal MCP > Providers` 改 Codex provider 的 BaseUrl，保存。

### 2.7 Codex Desktop 没登录 / token 过期

**症状**：bridge 启动 OK 但 Chat 面板里发请求后 Codex 报 "unauthorized" / "not signed in"。

**修复**:

```pwsh
# 看 Codex auth 状态
codex auth status
# 或者
Get-Content "$env:USERPROFILE\.codex\auth.json" -ErrorAction SilentlyContinue | Select-String -Pattern "expires|token" -SimpleMatch | Select-Object -First 3
```

如果显示未登录或 token 过期：

```pwsh
codex login
# 跟随浏览器登录流，登 ChatGPT account（不是 OpenAI API key）
```

登录后重启 bridge。

### 2.8 路径含空格的 cmd/ps1 调用失败

**症状**：从 cmd 双击 `.cmd` 启动 bridge，但路径含 "Unreal Projects" / "OneDrive\ドキュメント"，bridge 报 "Cannot find module" 或 "ENOENT" 错。

**根因**：Tier 4 已经在所有路径参数加 quote，但用户自己包装的 `.bat` / 第三方启动器可能没。

**修复**:

```pwsh
# 用 .ps1 launcher（已经处理 quoting），不要用裸 .cmd 串
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1

# 或如果一定要 cmd: 自己 quote 路径
cd "C:\path with space\UserProject"
"C:\path with space\UserProject\Tools\UnrealMcpCodexBridge\start-bridge.cmd"
```

### 2.9 Firewall 阻止 localhost loopback

**症状**：罕见。bridge 起来了 LISTEN 但 health check 在另一个 PowerShell 报 connection refused。

**修复**:

```pwsh
# 看 Windows Defender Firewall 规则
Get-NetFirewallRule -DisplayName "*loopback*","*UnrealMcp*","*bun*","*UnrealEditor*" -ErrorAction SilentlyContinue
```

通常 localhost loopback 不会被默认 firewall 拦。如果真有：

```pwsh
# 临时允许 outbound + inbound 127.0.0.1:8766 (需要管理员)
# 或者关掉某个第三方 firewall / 安全软件 测试一次
```

## 3. 重置一切（核选项）

如果上面都试过仍不行，干净重置 bridge 状态：

```pwsh
# 1. 杀所有 bridge / bun 残留
Stop-Process -Name bun -Force -ErrorAction SilentlyContinue
Stop-Process -Name codex -Force -ErrorAction SilentlyContinue

# 2. 删 bridge 临时状态（不影响 UE 工作 — 这些是 bridge 自己的 cache）
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\UEvolveCodexBridge" -ErrorAction SilentlyContinue

# 3. 清所有 UEVOLVE_* 环境变量（确保从 launcher 默认值开始）
Get-ChildItem env: | Where-Object Name -like "UEVOLVE_*" | ForEach-Object { Remove-Item "env:$($_.Name)" }

# 4. 关 UE editor + 重新打开 (让 plugin re-register the Codex provider config)

# 5. 跑 bridge
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

## 4. 收集日志给 PM（如果还是不行）

如果 step 3 后仍不行，把以下信息打包给 PM：

```pwsh
# Bridge 启动日志（让它跑完整 60 秒收集 stderr）
$env:UEVOLVE_BRIDGE_LOG_TO_FILE = "C:\Temp\uevolve-bridge.log"
.\Tools\UnrealMcpCodexBridge\start-bridge.ps1 2>&1 | Tee-Object -FilePath C:\Temp\uevolve-bridge.log
# Ctrl+C 60 秒后停

# UE Editor 的 LogUnrealMcp 段
Get-Content "<UserProject>\Saved\Logs\<YourProject>.log" | Select-String -Pattern "LogUnrealMcp" | Tee-Object C:\Temp\ue-mcp.log

# 环境快照
@{
  OS = (Get-CimInstance Win32_OperatingSystem).Version
  Bun = & "$env:LOCALAPPDATA\UEvolveCodexBridge\bun.exe" --version 2>$null
  Codex = & codex --version 2>$null
  CodexBin = (Get-ChildItem "$env:LOCALAPPDATA\OpenAI\Codex\bin" -Recurse -Filter codex.exe -ErrorAction SilentlyContinue).FullName
  Port8765 = (Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue).OwningProcess
  Port8766 = (Get-NetTCPConnection -LocalPort 8766 -State Listen -ErrorAction SilentlyContinue).OwningProcess
  CodexProviderConfig = (Select-String -Path "<UserProject>\Config\DefaultEngine.ini" -Pattern "BaseUrl|CodexAppServer" -ErrorAction SilentlyContinue).Line
} | ConvertTo-Json -Depth 5 | Out-File C:\Temp\uevolve-env.json
```

把 3 个文件（`uevolve-bridge.log` / `ue-mcp.log` / `uevolve-env.json`）打包发给 PM。

## 5. 根本原因索引（按报错关键词查）

| 报错关键词 | 章节 | 根因 |
|---|---|---|
| `bun: command not found` / `bun not recognized` | 2.1 | bundled bun.exe 缺失或 launcher 错 |
| `EPERM: operation not permitted, uv_spawn` | 2.2 | Codex 在 WindowsApps 受保护路径 |
| `unsupported --listen URL` | 2.3 | UEVOLVE_CODEX_TRANSPORT=ws override 错 |
| `EADDRINUSE: 127.0.0.1:8766` | 2.4 | 端口被占 |
| `Failed to connect to ws://...:8765` | 2.5 | UE plugin 没起 |
| Chat 面板报 "connection refused" 但 bridge OK | 2.6 | UE provider BaseUrl 配置错 |
| Codex 返 `unauthorized` / `401` | 2.7 | Codex Desktop 未登录或 token 过期 |
| `ENOENT: ... start-bridge.cmd` | 2.8 | 路径含空格没 quote |
| LISTEN 但 connect refused | 2.9 | Firewall（罕见） |
| 全都对但仍不行 | 3 / 4 | 干净重置 + 收集日志给 PM |
