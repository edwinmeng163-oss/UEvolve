# Deployment Troubleshooting

This page is versioned local knowledge for UEvolve RAG. It captures issues seen when installing the Unreal MCP plugin into another Unreal project.

## Project-Level Install

The recommended install path is a project-level plugin:

```text
<YourProject>/Plugins/UnrealMcp
<YourProject>/Tools
<YourProject>/Schemas
<YourProject>/Docs
```

Do not install a second copy under the Unreal Engine installation while also using a project copy. Duplicate plugin copies can make Unreal load the wrong binary or compile against stale files.

## Required Project Plugins

The target `.uproject` should enable:

```json
{
  "Name": "PythonScriptPlugin",
  "Enabled": true
},
{
  "Name": "UnrealMcp",
  "Enabled": true
}
```

PythonScriptPlugin is needed because several high-level tools rely on Unreal Editor Python for safe asset and level automation.

## Build On Windows

Use the engine Build script with the target project:

```powershell
E:\UE_5.7\Engine\Build\BatchFiles\Build.bat UnrealEditor Win64 Development -Project=E:\UE5_P\YourProject\YourProject.uproject -WaitMutex
```

Before running the build:

- Close Unreal Editor.
- Disable Live Coding for this build session.
- Install Visual Studio 2022 Build Tools, MSVC v143, Windows SDK, and .NET Framework Developer Pack / NetFxSDK.

Common Windows build failures:

- `Unable to build while Live Coding is active`: close Unreal Editor or disable Live Coding.
- `Win64 is not a valid platform to build`: Visual Studio C++ workload or Windows SDK is incomplete.
- `Could not find NetFxSDK install dir`: install .NET Framework Developer Pack 4.8 or 4.8.1.

## Build On macOS

Use the UE 5.7 Mac Build script:

```bash
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" UEvolveEditor Mac Development -Project="/path/to/UEvolve.uproject" -WaitMutex
```

When launching from scripts on macOS, prefer the app launcher route:

```bash
open -a "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app" --args "/path/to/UEvolve.uproject" -NoSplash
```

Calling the raw `UnrealEditor` binary can immediately hand off to the `.app` executable and exit, which can confuse supervisor scripts unless they wait for the endpoint instead of only tracking the first process id.

## Endpoint Verification

The MCP endpoint is not a standalone background service. It responds only after:

- Unreal Editor is open.
- The target project loaded successfully.
- UnrealMcp is enabled and loaded.
- Port `8765` is not occupied by another process.

Smoke test:

```bash
curl http://127.0.0.1:8765/mcp
```

For JSON-RPC testing, prefer a generated request file or PowerShell `ConvertTo-Json`; raw PowerShell quoting often corrupts nested JSON.

## Workbench Health

`unreal.mcp_workbench_status` is healthy when functional checks pass:

- tools/list visible tool count is non-zero.
- schema incompatible count is zero.
- missing handler count is zero.
- documentation warnings do not block functional health for project-level installs when docs live outside the target project root.

## Log Reading

`unreal.tail_log` should normalize the path returned by Unreal before reading it. On some UE 5.7 Windows setups, Unreal can return a path that looks absolute but contains `../../..` segments. Always convert to a full normalized path before calling file read APIs.

## Safe Recovery

If an imported tool or self-extension fails:

1. Run `unreal.mcp_classify_error` on the build/test/log text.
2. Run `unreal.mcp_diff_last_apply`.
3. Use `unreal.mcp_rollback_last_extension` or `unreal.mcp_rollback_to_manifest`.
4. Rebuild with the Editor closed.
5. Reopen Editor and run `unreal.mcp_workbench_status`.

