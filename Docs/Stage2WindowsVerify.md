ROLE: Stage 2 — end-to-end Windows verification of the v0.12.0-pilot package. The Windows machine currently has only Unreal Engine installed; it does NOT have the UnrealMcp plugin, the UEvolve repo, or any prior build artifacts. This prompt walks you through:

1. cloning the repo,
2. running `Tools/package_plugin.ps1` to produce the Windows-named zip locally (this also exercises the .ps1 itself, which has only been syntax-verified on macOS so far),
3. dropping that zip into a clean Windows test project built from UE's own `TP_Blank` template,
4. UBT-building the editor target on Win64,
5. headless-launching the editor and confirming the UnrealMcp MCP endpoint exposes >= 100 tools,
6. cleaning up.

A passing run is the missing piece before we can rename the release to `v0.12.1-pilot-crossplatform` and ship a Windows-verified zip as a second asset.

CONTEXT
- Repo URL: `https://github.com/edwinmeng163-oss/UEvolve.git`. Target commit: `main` HEAD (currently `91f899e feat(pilot): add Windows package_plugin.ps1 + trilingual install/release pointers`). If `main` has moved past that, prefer the latest commit; the .ps1 and packaging logic should still match.
- UE install: assumed at `C:\Program Files\Epic Games\UE_5.7\`. If yours is elsewhere (e.g. `D:\Epic\UE_5.7\`), substitute the absolute path everywhere it appears below. Same logic applies if you want to test UE 5.6 instead — substitute `UE_5.6` everywhere.
- TP_Blank template: `<UE>\Templates\TP_Blank\` — present in every standard UE install.
- Prerequisite tooling on the Windows machine: Git for Windows, Python 3.x (in `PATH`), PowerShell 5.1 or newer (default on Windows 10/11), and Visual Studio 2022 with the "Game Development with C++" workload + Windows 10/11 SDK + .NET 6+ SDK. Without VS 2022's C++ toolchain UBT cannot link `UnrealEditor-UnrealMcp.dll`.
- Workspace dir: pick somewhere writable that's not under `Program Files` (avoids UAC surprises). Examples below use `C:\Work\UEvolve` for the clone and `C:\Temp\UEvolvePilotTest` for the test project — substitute whatever fits your machine.
- Sandbox / agent runner: if you are dispatching this as a codex-agent, use `-m gpt-5.5 -r xhigh -s danger-full-access` (UBT writes engine intermediate, the editor launch + process management need `Stop-Process` / `taskkill`). danger-full-access still does not authorize commits or pushes (this prompt forbids them explicitly below). If you are running by hand, open PowerShell with normal-user privileges; no `Start-Process -Verb RunAs` needed.

STEPS

### Step 1 — Clone the repo

```powershell
New-Item -ItemType Directory -Force C:\Work | Out-Null
cd C:\Work
git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve
git log -1 --oneline
```

PASS criteria: `git log -1` shows the `feat(pilot)` commit (or a later main HEAD that still contains `Tools\package_plugin.ps1`). Confirm:

```powershell
Test-Path Tools\package_plugin.ps1
Test-Path Plugins\UnrealMcp\UnrealMcp.uplugin
Test-Path Tools\UnrealMcpToolRegistry\tools.json
```

All three must return `True`.

### Step 2 — Run `Tools\package_plugin.ps1`

```powershell
powershell -ExecutionPolicy Bypass -File Tools\package_plugin.ps1 *>&1 | Tee-Object -FilePath C:\Temp\uevolve-stage2-package.log
```

PASS criteria: exit 0; the log ends with three lines `Zip path: ...`, `Zip size: ...`, `SHA-256: ...`, plus the `Done. Next: ...` message. Capture both the printed SHA-256 AND verify via:

```powershell
$zip = "Saved\UnrealMcp\Packages\UnrealMcp-v0.12.0-pilot-win-ue56-ue57-source.zip"
Test-Path $zip
Test-Path "$zip.sha256"
(Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Get-Content "$zip.sha256"
```

The recomputed hash must match the value in the sidecar. Quote both in your report.

FAIL handling: if the .ps1 errors out (any `Die` message), STOP and report the error verbatim plus the last 30 lines of `C:\Temp\uevolve-stage2-package.log`. Do NOT try to fix the .ps1 — fixing is a follow-up task, not part of this verification.

### Step 3 — Ensure port 8765 is free

```powershell
Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue
```

If any row is returned, identify the owning process via `Get-Process -Id <PID>` and decide whether you can stop it (your own leftover editor) or whether it's something unrelated (in which case abort and report). To stop a leftover editor:

```powershell
$existing = Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue
if ($existing) {
    $existing | ForEach-Object {
        $p = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -like "UnrealEditor*") { $p.CloseMainWindow() | Out-Null }
    }
    Start-Sleep -Seconds 60
    Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue
}
```

After cleanup, `Get-NetTCPConnection -LocalPort 8765 -State Listen` must return nothing.

### Step 4 — Create the test project from UE's `TP_Blank` template

```powershell
$proj = "C:\Temp\UEvolvePilotTest"
if (Test-Path $proj) { Remove-Item -Recurse -Force $proj }
Copy-Item -Recurse "C:\Program Files\Epic Games\UE_5.7\Templates\TP_Blank" $proj
```

Rename internal `TP_Blank` references to `UEvolvePilotTest`:

```powershell
# Rename .uproject
Move-Item "$proj\TP_Blank.uproject" "$proj\UEvolvePilotTest.uproject"

# Rewrite the .uproject JSON: rename the module + add plugin requirements.
$uproj = "$proj\UEvolvePilotTest.uproject"
$json = Get-Content $uproj -Raw | ConvertFrom-Json
$json.Modules[0].Name = "UEvolvePilotTest"
$json | Add-Member -NotePropertyName Plugins -NotePropertyValue @(
    [pscustomobject]@{ Name = "PythonScriptPlugin"; Enabled = $true },
    [pscustomobject]@{ Name = "UnrealMcp"; Enabled = $true }
) -Force
$json | ConvertTo-Json -Depth 10 | Set-Content $uproj -Encoding UTF8

# Rename the module dir + module-internal files
Move-Item "$proj\Source\TP_Blank" "$proj\Source\UEvolvePilotTest"
Get-ChildItem "$proj\Source\UEvolvePilotTest" -File | ForEach-Object {
    $newName = $_.Name -replace 'TP_Blank', 'UEvolvePilotTest'
    if ($_.Name -ne $newName) { Rename-Item $_.FullName $newName }
}

# Rename the Target.cs files
Move-Item "$proj\Source\TP_Blank.Target.cs" "$proj\Source\UEvolvePilotTest.Target.cs"
Move-Item "$proj\Source\TP_BlankEditor.Target.cs" "$proj\Source\UEvolvePilotTestEditor.Target.cs"

# Replace every TP_Blank token inside every .cs / .cpp / .h file under Source\
Get-ChildItem "$proj\Source" -Recurse -Include *.cs,*.cpp,*.h | ForEach-Object {
    $content = Get-Content $_.FullName -Raw
    $rewritten = $content -replace 'TP_Blank', 'UEvolvePilotTest'
    if ($content -ne $rewritten) {
        Set-Content $_.FullName -Value $rewritten -NoNewline
    }
}

# Drop Epic's template-metadata file; it's only used by the New Project wizard
# and its example comments reference TP_Blank, which would otherwise trip the
# zero-token assertion below.
Remove-Item "$proj\Config\TemplateDefs.ini" -ErrorAction SilentlyContinue

# Verify zero TP_Blank tokens remain
Get-ChildItem $proj -Recurse -File | Select-String -Pattern 'TP_Blank' -SimpleMatch | Select-Object -First 5
```

PASS criteria: the `Select-String` returns nothing. The preceding `Remove-Item` strips `Config\TemplateDefs.ini`; if you want to keep that file for any reason, you must instead audit the matches manually. If any match remains, report which files and STOP — do not attempt to patch by hand without first understanding the miss.

### Step 5 — Drop the .ps1-produced zip into the test project

```powershell
New-Item -ItemType Directory -Force "$proj\Plugins" | Out-Null
Expand-Archive -Path "C:\Work\UEvolve\Saved\UnrealMcp\Packages\UnrealMcp-v0.12.0-pilot-win-ue56-ue57-source.zip" -DestinationPath "$proj\Plugins" -Force
Test-Path "$proj\Plugins\UnrealMcp\UnrealMcp.uplugin"
Test-Path "$proj\Plugins\UnrealMcp\Resources\ToolRegistry\tools.json"
Test-Path "$proj\Plugins\UnrealMcp\INSTALL.md"
```

All three `Test-Path` results must be `True`.

### Step 6 — UBT build the editor target

```powershell
$buildLog = "C:\Temp\uevolve-stage2-build.log"
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
    UEvolvePilotTestEditor Win64 Development `
    "-Project=$proj\UEvolvePilotTest.uproject" `
    -waitmutex *>&1 | Tee-Object -FilePath $buildLog
$lastExit = $LASTEXITCODE
```

PASS criteria: `$lastExit -eq 0`, `Result: Succeeded` appears at the bottom of `$buildLog`, NO `error:` or `Error:` lines, NO `fatal error`. Specifically confirm:

```powershell
Select-String $buildLog -Pattern 'Compile.*Module\.UnrealMcp.*\.cpp' | Select-Object -First 5
Select-String $buildLog -Pattern 'Link.*UnrealEditor-UnrealMcp\.dll' | Select-Object -First 1
Test-Path "$proj\Plugins\UnrealMcp\Binaries\Win64\UnrealEditor-UnrealMcp.dll"
```

The first must list multiple compile lines (UnrealMcp plugin compiling from the dropped zip), the second must show the link line, and the .dll must exist post-build.

FAIL handling: if the build does NOT succeed, capture the first `error:` / `Error:` line plus the last 50 lines of `$buildLog`, then STOP. Do NOT attempt fixes.

### Step 7 — Headless editor smoke

```powershell
$smokeLog = "C:\Temp\uevolve-stage2-smoke.log"
Remove-Item "$proj\Saved\Logs\UEvolvePilotTest.log" -ErrorAction SilentlyContinue
Remove-Item $smokeLog -ErrorAction SilentlyContinue

$editorExe = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$proc = Start-Process -PassThru -FilePath $editorExe `
    -ArgumentList "`"$proj\UEvolvePilotTest.uproject`"", "-AbsLog=$smokeLog"
"PID: $($proc.Id)"
```

Poll the log every ~5 seconds for any of these boot markers — `Engine is initialized`, `LogInit: Engine initialized`, or any `LogUnrealMcp:` line — for up to 10 minutes total:

```powershell
$timeoutSec = 600
$elapsed = 0
$bootMarker = $null
while ($elapsed -lt $timeoutSec -and -not $bootMarker) {
    Start-Sleep -Seconds 5
    $elapsed += 5
    if (Test-Path $smokeLog) {
        $bootMarker = Select-String $smokeLog `
            -Pattern 'Engine is initialized|LogInit: Engine initialized|LogUnrealMcp:' `
            | Select-Object -First 1
    }
}
$bootMarker
```

PASS: `$bootMarker` is not null. If timed out, kill the editor and STOP with last 50 lines of `$smokeLog`.

Once booted, quote one `LogUnrealMcp:` line:

```powershell
Select-String $smokeLog -Pattern 'LogUnrealMcp:' | Select-Object -First 1
```

Hit the MCP endpoint and count tools:

```powershell
$body = '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
$headers = @{
    'Content-Type'         = 'application/json'
    'Accept'               = 'application/json, text/event-stream'
    'MCP-Protocol-Version' = '2025-06-18'
}
$resp = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8765/mcp' `
    -Body $body -Headers $headers -TimeoutSec 30
$resp.result.tools.Count
$resp.result.tools | Select-Object -First 5 -ExpandProperty name
```

PASS: `tools.Count -ge 100`. Quote the count + first 5 tool names. FAIL if the curl call errors out or count is < 100; report the JSON head (`$resp | ConvertTo-Json -Depth 3 | Select-Object -First 50`).

Shutdown the editor gracefully:

```powershell
$proc.CloseMainWindow() | Out-Null
$exited = $proc.WaitForExit(60000)  # 60s
if (-not $exited) {
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Seconds 5
}
Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and $_.Path -like "*$proj*"
}
```

PASS: the final `Get-Process` returns nothing (your test editor is gone). Other unrelated UnrealEditor processes elsewhere on the machine are fine.

### Step 8 — Cleanup

```powershell
Remove-Item -Recurse -Force C:\Temp\UEvolvePilotTest
```

The clone at `C:\Work\UEvolve` may stay (it has the verified zip in `Saved\UnrealMcp\Packages\`, which is gitignored and harmless). If you want a fully clean machine afterwards, also `Remove-Item -Recurse -Force C:\Work\UEvolve` — but the verified zip there is the actual artifact we may want to upload to GitHub as a second release asset, so consider keeping it.

## Stage 2b: Full-Experience Zip Verification

This path verifies `UnrealMcp-v0.12.0-pilot-full-win-ue561.zip`, which extracts at a project root and includes `Plugins/`, project-level `Tools/`, docs, schemas, a UE 5.6.1 Win64 plugin binary, and an offline-ready Codex bridge bundle. Use Epic Launcher Unreal Engine 5.6.1 for this test; the prebuilt plugin binary is BuildId-locked to that engine patch.

### Phase 1 — Build UE 5.6.1 Win64 plugin binaries

Use the same clone at `C:\Work\UEvolve`, but build against UE 5.6.1:

```powershell
$repo = "C:\Work\UEvolve"
$buildLog = "C:\Temp\uevolve-stage2b-build.log"
cd $repo
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" `
    UEvolveEditor Win64 Development `
    "-Project=$repo\UEvolve.uproject" `
    -WaitMutex *>&1 | Tee-Object -FilePath $buildLog
$LASTEXITCODE
Test-Path "$repo\Plugins\UnrealMcp\Binaries\Win64\UnrealEditor-UnrealMcp.dll"
Test-Path "$repo\Plugins\UnrealMcp\Binaries\Win64\UnrealEditor.modules"
```

PASS: build exit code is `0`, the build log reports success, and both binary files exist.

### Phase 3 — Bundle the offline Codex bridge

```powershell
cd C:\Work\UEvolve
powershell -ExecutionPolicy Bypass -File Tools\bundle_bridge_for_release.ps1 *>&1 |
    Tee-Object -FilePath C:\Temp\uevolve-stage2b-bridge-bundle.log
Test-Path Saved\UnrealMcp\Packages\UnrealMcp-CodexBridge-win-bundle.tar
```

PASS: the helper prints `Bridge bundle: ...` and the tarball exists. If Bun was not installed, the helper downloads the standalone Win64 runtime and includes it under `runtime\bun.exe`.

### Phase 4 — Package the full-experience zip

```powershell
cd C:\Work\UEvolve
$bridgeBundle = "Saved\UnrealMcp\Packages\UnrealMcp-CodexBridge-win-bundle.tar"
powershell -ExecutionPolicy Bypass -File Tools\package_plugin.ps1 `
    -FullExperience `
    -PrebuiltBinariesPath "C:\Work\UEvolve" `
    -BridgeBundlePath $bridgeBundle `
    -EngineTag ue561 *>&1 |
    Tee-Object -FilePath C:\Temp\uevolve-stage2b-package.log

$zip = "Saved\UnrealMcp\Packages\UnrealMcp-v0.12.0-pilot-full-win-ue561.zip"
Test-Path $zip
Test-Path "$zip.sha256"
(Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Get-Content "$zip.sha256"
```

PASS: the package script exits `0`, prints the zip path, size, SHA-256, and `Done. Next: open this on a clean Windows UE 5.6.1 project; see Docs/FIRST_LAUNCH.md.` The recomputed SHA-256 must match the sidecar.

### Stage 2b-BP: Blueprint-only pure-unzip cold start

Create a clean Blueprint-only blank project at `C:\Temp\UEvolveFullTestBP` by copying `TP_Blank`. You MUST remove the template `Source/` tree AND leave the `.uproject` `Modules` array empty; otherwise UBT will treat the project as a build path and the first launch will not prove the pure-unzip case.

```powershell
$proj = "C:\Temp\UEvolveFullTestBP"
if (Test-Path $proj) { Remove-Item -Recurse -Force $proj }
Copy-Item -Recurse "C:\Program Files\Epic Games\UE_5.6\Templates\TP_Blank" $proj
Move-Item "$proj\TP_Blank.uproject" "$proj\UEvolveFullTestBP.uproject"
Remove-Item -Recurse -Force "$proj\Source" -ErrorAction SilentlyContinue

$uproj = "$proj\UEvolveFullTestBP.uproject"
$json = Get-Content $uproj -Raw | ConvertFrom-Json
$json | Add-Member -NotePropertyName Modules -NotePropertyValue ([object[]]@()) -Force
$json | Add-Member -NotePropertyName Plugins -NotePropertyValue @(
    [pscustomobject]@{ Name = "PythonScriptPlugin"; Enabled = $true },
    [pscustomobject]@{ Name = "UnrealMcp"; Enabled = $true }
) -Force
$json | ConvertTo-Json -Depth 10 | Set-Content $uproj -Encoding UTF8

Expand-Archive -Path "C:\Work\UEvolve\Saved\UnrealMcp\Packages\UnrealMcp-v0.12.0-pilot-full-win-ue561.zip" `
    -DestinationPath $proj -Force
$checkJson = Get-Content $uproj -Raw | ConvertFrom-Json
$moduleCount = 0
if ($null -ne $checkJson.Modules) { $moduleCount = @($checkJson.Modules).Count }

Test-Path "$proj\UEvolveFullTestBP.uproject"
Test-Path "$proj\Source"
$moduleCount
Test-Path "$proj\Plugins\UnrealMcp\UnrealMcp.uplugin"
Test-Path "$proj\Tools\UnrealMcpToolRegistry\tools.json"
Test-Path "$proj\Tools\UnrealMcpCodexBridge\start-bridge.cmd"
Test-Path "$proj\first_launch_build.cmd"
```

PASS: `UEvolveFullTestBP.uproject` exists, `Test-Path "$proj\Source"` returns `False`, `$moduleCount` is `0`, and the extracted `Plugins/`, `Tools/`, and `first_launch_build.cmd` paths exist.

Open `UEvolveFullTestBP.uproject` in Unreal Editor. PASS: the editor opens, the plugin DLL loads as-is, and there is ZERO UBT activity: no rebuild prompt, no `UnrealBuildTool`/`Build.bat` process, and no plugin compile/link lines.

Start the bridge:

```powershell
Start-Process -FilePath "$proj\Tools\UnrealMcpCodexBridge\start-bridge.cmd" -WorkingDirectory "$proj\Tools\UnrealMcpCodexBridge"
Get-NetTCPConnection -LocalPort 8766 -State Listen
```

PASS: the bridge console reports `Bridge listening on ws://127.0.0.1:8766/uevolve` and port `8766` is listening.

Open `Window > Unreal MCP Chat` and call:

```text
/tool unreal.editor_status {}
```

PASS: Chat returns a structured editor status response.

Apply the `fps_bootstrap` starter scaffold from `Tools/UnrealMcpToolScaffoldStarters/` through Chat, then call:

```text
/tool unreal.fps.bootstrap {"targetPath":"/Game/UEvolveFull/FPS","characterName":"BP_UEvolvePilotCharacter"}
```

PASS: the scaffold pipeline runs and `unreal.fps.bootstrap` creates a functional FPS character setup.

Run the functional input verifier:

```text
/tool unreal.simulation.verify_input_drives_pawn {"pawnName":"BP_UEvolvePilotCharacter"}
```

PASS: verification reports success. Finally, press Play in PIE and manually confirm WASD movement plus mouse rotation.

### Stage 2b-CPP: C++ first-launch rebuild path

Create a clean C++ `TP_Blank` project at `C:\Temp\UEvolveFullTestCPP` and keep its `Source/` tree. This path is expected to rebuild on first launch. After extracting the full-experience zip, run `<Project>\first_launch_build.cmd` from the project root, verify it succeeds, then open the editor and confirm the plugin loads.

```powershell
$projCpp = "C:\Temp\UEvolveFullTestCPP"
if (Test-Path $projCpp) { Remove-Item -Recurse -Force $projCpp }
Copy-Item -Recurse "C:\Program Files\Epic Games\UE_5.6\Templates\TP_Blank" $projCpp
Move-Item "$projCpp\TP_Blank.uproject" "$projCpp\UEvolveFullTestCPP.uproject"

$uprojCpp = "$projCpp\UEvolveFullTestCPP.uproject"
$jsonCpp = Get-Content $uprojCpp -Raw | ConvertFrom-Json
$jsonCpp.Modules[0].Name = "UEvolveFullTestCPP"
$jsonCpp | Add-Member -NotePropertyName Plugins -NotePropertyValue @(
    [pscustomobject]@{ Name = "PythonScriptPlugin"; Enabled = $true },
    [pscustomobject]@{ Name = "UnrealMcp"; Enabled = $true }
) -Force
$jsonCpp | ConvertTo-Json -Depth 10 | Set-Content $uprojCpp -Encoding UTF8

Move-Item "$projCpp\Source\TP_Blank" "$projCpp\Source\UEvolveFullTestCPP"
Get-ChildItem "$projCpp\Source\UEvolveFullTestCPP" -File | ForEach-Object {
    $newName = $_.Name -replace 'TP_Blank', 'UEvolveFullTestCPP'
    if ($_.Name -ne $newName) { Rename-Item $_.FullName $newName }
}
Move-Item "$projCpp\Source\TP_Blank.Target.cs" "$projCpp\Source\UEvolveFullTestCPP.Target.cs"
Move-Item "$projCpp\Source\TP_BlankEditor.Target.cs" "$projCpp\Source\UEvolveFullTestCPPEditor.Target.cs"
Get-ChildItem "$projCpp\Source" -Recurse -Include *.cs,*.cpp,*.h | ForEach-Object {
    $content = Get-Content $_.FullName -Raw
    $rewritten = $content -replace 'TP_Blank', 'UEvolveFullTestCPP'
    if ($content -ne $rewritten) { Set-Content $_.FullName -Value $rewritten -NoNewline }
}
Remove-Item "$projCpp\Config\TemplateDefs.ini" -ErrorAction SilentlyContinue
Get-ChildItem $projCpp -Recurse -File | Select-String -Pattern 'TP_Blank' -SimpleMatch | Select-Object -First 5

Expand-Archive -Path "C:\Work\UEvolve\Saved\UnrealMcp\Packages\UnrealMcp-v0.12.0-pilot-full-win-ue561.zip" `
    -DestinationPath $projCpp -Force
Test-Path "$projCpp\Source"
Test-Path "$projCpp\first_launch_build.cmd"
Test-Path "$projCpp\Plugins\UnrealMcp\Binaries\Win64\UnrealEditor-UnrealMcp.dll"
```

PASS: the `Select-String` returns nothing, `Source/` remains present, and the extracted top-level `first_launch_build.cmd` plus prebuilt plugin DLL exist.

Run the first-launch build. On a warm cache this is expected to take about 5 minutes:

```powershell
Push-Location $projCpp
try {
    & "$projCpp\first_launch_build.cmd"
    $firstLaunchExit = $LASTEXITCODE
} finally {
    Pop-Location
}
$firstLaunchLog = "$projCpp\Saved\Logs\first_launch_build.log"
$firstLaunchExit
Test-Path $firstLaunchLog
Select-String $firstLaunchLog -Pattern 'Build succeeded|Result: Succeeded' | Select-Object -Last 5
```

PASS: `$firstLaunchExit -eq 0`, the log exists, and the script prints `Build succeeded - log at <path>`.

Verify the rebuilt plugin modules manifest now matches the UE 5.6.1 engine BuildId:

```powershell
$engineModules = Get-Content "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.modules" -Raw | ConvertFrom-Json
$pluginModules = Get-Content "$projCpp\Plugins\UnrealMcp\Binaries\Win64\UnrealEditor.modules" -Raw | ConvertFrom-Json
$engineModules.BuildId
$pluginModules.BuildId
$engineModules.BuildId -eq $pluginModules.BuildId
```

PASS: the final comparison returns `True`. Then open `UEvolveFullTestCPP.uproject`; the editor should load the rebuilt plugin without another rebuild prompt.

### Stage 2b Report

Report these fields:

```text
FULL ZIP: PASS | FAIL — <zip path> — <SHA-256>
PHASE 1 BUILD: PASS | FAIL — <result line> — <log path>
PHASE 3 BRIDGE BUNDLE: PASS | FAIL — <bundle path>
PHASE 4 PACKAGE: PASS | FAIL — <zip path> — <SHA-256 sidecar match yes/no>
STAGE 2b-BP PURE UNZIP: PASS | FAIL — zero UBT activity: yes/no — plugin DLL as-is: yes/no — bridge 8766: yes/no — editor_status: yes/no
STAGE 2b-CPP FIRST-LAUNCH BUILD: PASS | FAIL — duration: <minutes> — BuildId match: yes/no — plugin loads: yes/no
FPS SCAFFOLD: PASS | FAIL
FPS BOOTSTRAP: PASS | FAIL
INPUT VERIFIER: PASS | FAIL
MANUAL PIE MOVEMENT: PASS | FAIL
```

REPORT (top of message)
```
PACKAGE: PASS | FAIL — <win zip path> — <SHA-256>
EXT BUILD: PASS | FAIL — <Result line> — <log path>
EXT SMOKE: PASS | FAIL — boot marker: "<line>" — UnrealMcp marker: "<line>" — tools/list count: <N> — exit: <how>
```

Then:
- The .ps1 stdout summary block (Zip path / Zip size / SHA-256 / Done line).
- Recomputed `Get-FileHash` value vs sidecar content — confirm match.
- The `Module.UnrealMcp.*.cpp` compile lines + the `Link ... UnrealEditor-UnrealMcp.dll` line from the build log.
- One quoted `LogUnrealMcp:` line from the smoke log.
- tools/list count + first 5 tool names.
- Confirmation that `C:\Temp\UEvolvePilotTest` is removed and `Get-Process -Name UnrealEditor` returns no rows tied to that path.
- Anything weird you noticed in the .ps1 behavior on Windows that the Mac-side author should fix in a follow-up (e.g. PowerShell strict-mode warnings, encoding mismatches, path-quote issues). Report as advisory, do NOT fix.

CONSTRAINTS
- READ-ONLY for the cloned repo's committed files. The test project lives entirely under `C:\Temp\UEvolvePilotTest\`. Do NOT touch the repo's own `Examples\` or `Plugins\`.
- Do NOT `git commit`, `git add`, `git checkout`, or `git push`. If the .ps1 reveals a real bug, file it in your final report — fixing is a separate task.
- If EXT BUILD fails: report first error line + final summary, then STOP. Do not iterate.
- If EXT SMOKE never reaches a boot marker in 10 min: kill the editor, report the last 50 lines of `$smokeLog`.
- If tools/list returns < 100 tools: FAIL with actual count + JSON head.
- The verified zip at `C:\Work\UEvolve\Saved\UnrealMcp\Packages\` is the artifact that the Mac-side PM will upload as a second release asset on GitHub. Do not delete it until told.

DONE when: three PASS lines reported with evidence, `C:\Temp\UEvolvePilotTest` is removed, no UE editor process tied to that path remains, the .ps1-produced zip is preserved at `C:\Work\UEvolve\Saved\UnrealMcp\Packages\` for upload.
