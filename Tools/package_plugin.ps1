<#
Builds a source-only project-root UnrealMcp zip for Windows UE 5.6/5.7 pilots, or a
project-root full-experience zip with prebuilt UE 5.6.1 Win64 binaries.

This PowerShell port is syntax-only verified on macOS. Stage 2 real Win64
verification on collaborator hardware is pending; track the pilot release at:
https://github.com/edwinmeng163-oss/UEvolve/releases/tag/v0.12.0-pilot-mac-ue56-ue57
#>

param(
    [string]$Output = "Saved/UnrealMcp/Packages",
    [string]$Version = "",
    [switch]$FullExperience,
    [string]$PrebuiltBinariesPath = "",
    [string]$BridgeBundlePath = "",
    [string]$EngineTag = "ue561"
)

$ErrorActionPreference = "Stop"

function Die {
    param([string]$Message)
    Write-Error -Message "Error: $Message" -ErrorAction Continue
    exit 1
}

function Assert-PlainFile {
    param([string]$Path, [string]$MissingMessage, [string]$ReparseMessage)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Die $MissingMessage
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Die $ReparseMessage
    }
}

function Assert-SameFileHash {
    param([string]$Left, [string]$Right, [string]$Message)
    $leftHash = (Get-FileHash -LiteralPath $Left -Algorithm SHA256).Hash
    $rightHash = (Get-FileHash -LiteralPath $Right -Algorithm SHA256).Hash
    if ($leftHash -ne $rightHash) {
        Die $Message
    }
}

function Invoke-PythonScript {
    param([string]$ScriptPath)
    foreach ($pythonCommand in @("python", "python3")) {
        $command = Get-Command $pythonCommand -ErrorAction SilentlyContinue
        if ($null -eq $command) {
            continue
        }
        # Merge stderr into stdout so PowerShell's Stop-on-stderr policy doesn't
        # wrap legitimate validator messages as NativeCommandError records.
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & $command.Source $ScriptPath 2>&1 | ForEach-Object { Write-Host $_ }
        } finally {
            $ErrorActionPreference = $prevEAP
        }
        if ($LASTEXITCODE -eq 0) {
            return $true
        }
    }
    return $false
}

function Copy-CleanDirectory {
    param([string]$Source, [string]$Destination, [string[]]$ExcludeNames)
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        Die "Missing directory: $Source"
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force -Exclude $ExcludeNames
    if ($null -ne $ExcludeNames -and $ExcludeNames.Count -gt 0) {
        Get-ChildItem -LiteralPath $Destination -Force -Recurse |
            Where-Object {
                $itemName = $_.Name
                $null -ne ($ExcludeNames | Where-Object { $itemName -like $_ } | Select-Object -First 1)
            } |
            Sort-Object -Property FullName -Descending |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Assert-RequiredDirectory {
    param([string]$Path, [string]$Message)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        Die $Message
    }
}

function Resolve-PrebuiltWin64Directory {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        Die "Full-experience packaging requires -PrebuiltBinariesPath"
    }
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        Die "Prebuilt binaries path does not exist: $Path"
    }
    $root = $resolved.Path
    $candidates = @(
        (Join-Path $root "Plugins/UnrealMcp/Binaries/Win64"),
        (Join-Path $root "Binaries/Win64"),
        $root
    )
    foreach ($candidate in $candidates) {
        $dllPath = Join-Path $candidate "UnrealEditor-UnrealMcp.dll"
        $modulesPath = Join-Path $candidate "UnrealEditor.modules"
        if ((Test-Path -LiteralPath $dllPath -PathType Leaf) -and
            (Test-Path -LiteralPath $modulesPath -PathType Leaf)) {
            return $candidate
        }
    }
    Die "Prebuilt binaries must contain Plugins/UnrealMcp/Binaries/Win64/UnrealEditor-UnrealMcp.dll and UnrealEditor.modules"
}

function Find-BridgeRoot {
    param([string]$SearchRoot)
    if ((Test-Path -LiteralPath (Join-Path $SearchRoot "start-bridge.cmd") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $SearchRoot "package.json") -PathType Leaf)) {
        return $SearchRoot
    }
    $startBridge = Get-ChildItem -LiteralPath $SearchRoot -Filter "start-bridge.cmd" -File -Recurse |
        Select-Object -First 1
    if ($null -eq $startBridge) {
        return ""
    }
    return $startBridge.Directory.FullName
}

function Assert-BridgeBundle {
    param([string]$BridgeRoot)
    if ([string]::IsNullOrWhiteSpace($BridgeRoot)) {
        Die "Bridge bundle does not contain start-bridge.cmd"
    }
    Assert-PlainFile (Join-Path $BridgeRoot "start-bridge.cmd") `
        "Bridge bundle missing start-bridge.cmd" `
        "Bridge bundle start-bridge.cmd must be a real file"
    Assert-PlainFile (Join-Path $BridgeRoot "package.json") `
        "Bridge bundle missing package.json" `
        "Bridge bundle package.json must be a real file"
    Assert-RequiredDirectory (Join-Path $BridgeRoot "node_modules") `
        "Bridge bundle missing node_modules/"
    Assert-PlainFile (Join-Path $BridgeRoot "runtime/bun.exe") `
        "Bridge bundle missing runtime/bun.exe" `
        "Bridge bundle runtime/bun.exe must be a real file"
}

function Resolve-BridgeBundleRoot {
    param([string]$Path, [string]$ExtractParent)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        Die "Full-experience packaging requires -BridgeBundlePath"
    }
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        Die "Bridge bundle path does not exist: $Path"
    }
    if (Test-Path -LiteralPath $resolved.Path -PathType Container) {
        $bridgeRoot = Find-BridgeRoot -SearchRoot $resolved.Path
        Assert-BridgeBundle -BridgeRoot $bridgeRoot
        return $bridgeRoot
    }

    $extractDir = Join-Path $ExtractParent "bridge-bundle-extract"
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
    if ($resolved.Path.ToLowerInvariant().EndsWith(".zip")) {
        Expand-Archive -LiteralPath $resolved.Path -DestinationPath $extractDir -Force
    } else {
        $tarCommand = Get-Command "tar" -ErrorAction SilentlyContinue
        if ($null -eq $tarCommand) {
            Die "Bridge bundle tar extraction requires tar.exe in PATH"
        }
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & $tarCommand.Source -xf $resolved.Path -C $extractDir 2>&1 | ForEach-Object { Write-Host $_ }
        } finally {
            $ErrorActionPreference = $prevEAP
        }
        if ($LASTEXITCODE -ne 0) {
            Die "Could not extract bridge bundle tarball: $($resolved.Path)"
        }
    }
    $bridgeRoot = Find-BridgeRoot -SearchRoot $extractDir
    Assert-BridgeBundle -BridgeRoot $bridgeRoot
    return $bridgeRoot
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDir "..")).Path
if ([System.IO.Path]::IsPathRooted($Output)) {
    $outputDir = $Output
} else {
    $outputDir = Join-Path $repoRoot $Output
}
$pluginDir = Join-Path $repoRoot "Plugins/UnrealMcp"
$pythonToolsDir = Join-Path $repoRoot "Tools/UnrealMcpPyTools"
$uplugin = Join-Path $pluginDir "UnrealMcp.uplugin"
$canonicalRegistry = Join-Path $repoRoot "Tools/UnrealMcpToolRegistry/tools.json"
$canonicalRegistrySchema = Join-Path $repoRoot "Tools/UnrealMcpToolRegistry/schema.json"
$mirrorRegistry = Join-Path $pluginDir "Resources/ToolRegistry/tools.json"
$installResource = Join-Path $repoRoot "Tools/PackagingResources/INSTALL.md"
$firstLaunchDoc = Join-Path $repoRoot "Docs/FIRST_LAUNCH.md"
$releaseDoc = Join-Path $repoRoot "Docs/Release-2026-05.md"
$stage2Doc = Join-Path $repoRoot "Docs/Stage2WindowsVerify.md"
$schemasDir = Join-Path $repoRoot "Schemas"
$stageParent = ""

try {
    if (-not (Test-Path -LiteralPath $uplugin -PathType Leaf)) {
        Die "Missing plugin descriptor: Plugins/UnrealMcp/UnrealMcp.uplugin"
    }

    # Pre-flight: fail before staging if the descriptor, registry mirror, or
    # compatibility validators are not in the exact state expected for release.
    $descriptor = Get-Content -LiteralPath $uplugin -Raw | ConvertFrom-Json
    $parsedVersion = $descriptor.VersionName
    if ([string]::IsNullOrWhiteSpace($parsedVersion)) {
        Die "Could not parse VersionName from Plugins/UnrealMcp/UnrealMcp.uplugin"
    }

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = $parsedVersion
    }

    Assert-PlainFile $mirrorRegistry `
        "Missing plugin registry mirror: Plugins/UnrealMcp/Resources/ToolRegistry/tools.json" `
        "Phase 1 fix not applied: see commit 00fbf5e"
    if (-not (Test-Path -LiteralPath $canonicalRegistry -PathType Leaf)) {
        Die "Missing canonical registry: Tools/UnrealMcpToolRegistry/tools.json"
    }
    if (-not (Test-Path -LiteralPath $canonicalRegistrySchema -PathType Leaf)) {
        Die "Missing canonical registry schema: Tools/UnrealMcpToolRegistry/schema.json"
    }
    if (-not (Test-Path -LiteralPath $pythonToolsDir -PathType Container)) {
        Die "Missing Python tool handlers: Tools/UnrealMcpPyTools"
    }
    Assert-SameFileHash $mirrorRegistry $canonicalRegistry `
        'Registry mirror mismatch; run `python3 Tools/validate_tool_registry.py` and resync'

    Push-Location $repoRoot
    try {
        if (-not (Invoke-PythonScript -ScriptPath "Tools/validate_tool_registry.py")) {
            Die "Registry validator failure; fix before packaging"
        }

        if (-not (Invoke-PythonScript -ScriptPath "Tools/check_ue56_compat.py")) {
            Die "UE 5.6 compatibility check failure; fix before packaging"
        }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path -LiteralPath $installResource -PathType Leaf)) {
        Die "Missing install resource: Tools/PackagingResources/INSTALL.md"
    }
    if (-not (Test-Path -LiteralPath $firstLaunchDoc -PathType Leaf)) {
        Die "Missing first-launch doc: Docs/FIRST_LAUNCH.md"
    }

    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $stageParent = Join-Path $outputDir ".stage-$timestamp"
    if (Test-Path -LiteralPath $stageParent) {
        Die "Staging directory already exists: $stageParent"
    }
    New-Item -ItemType Directory -Path $stageParent | Out-Null

    if ($FullExperience) {
        $prebuiltWin64 = Resolve-PrebuiltWin64Directory -Path $PrebuiltBinariesPath
        $bridgeRoot = Resolve-BridgeBundleRoot -Path $BridgeBundlePath -ExtractParent $stageParent

        $stagePlugin = Join-Path $stageParent "Plugins/UnrealMcp"
        Copy-CleanDirectory $pluginDir $stagePlugin @("Binaries", "Intermediate", "Saved", "DerivedDataCache", ".DS_Store")
        Copy-CleanDirectory $prebuiltWin64 (Join-Path $stagePlugin "Binaries/Win64") @("Intermediate", "Saved", "DerivedDataCache", ".DS_Store")
        Copy-Item -LiteralPath $installResource -Destination (Join-Path $stagePlugin "INSTALL.md") -Force

        $stageTools = Join-Path $stageParent "Tools"
        New-Item -ItemType Directory -Force -Path $stageTools | Out-Null
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolRegistry") (Join-Path $stageTools "UnrealMcpToolRegistry") @(".DS_Store", "Saved")
        Copy-CleanDirectory $pythonToolsDir (Join-Path $stageTools "UnrealMcpPyTools") @("__pycache__", "*.pyc", ".DS_Store")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpSkills/mcp-self-extension") (Join-Path $stageTools "UnrealMcpSkills/mcp-self-extension") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpKnowledge/Sources") (Join-Path $stageTools "UnrealMcpKnowledge/Sources") @(".DS_Store", "Saved")
        New-Item -ItemType Directory -Force -Path (Join-Path $stageTools "UnrealMcpKnowledge/Evals") | Out-Null
        Copy-Item -LiteralPath (Join-Path $repoRoot "Tools/UnrealMcpKnowledge/Evals/core_rag_eval.json") `
            -Destination (Join-Path $stageTools "UnrealMcpKnowledge/Evals/core_rag_eval.json") -Force
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpTests/Core") (Join-Path $stageTools "UnrealMcpTests/Core") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpTests/SelfExtension") (Join-Path $stageTools "UnrealMcpTests/SelfExtension") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpTests/Knowledge/closed_loop") (Join-Path $stageTools "UnrealMcpTests/Knowledge/closed_loop") @(".DS_Store", "Saved")
        Copy-CleanDirectory $bridgeRoot (Join-Path $stageTools "UnrealMcpCodexBridge") @("Intermediate", "Saved", "DerivedDataCache", ".DS_Store")
        $bridgeExtractDir = Join-Path $stageParent "bridge-bundle-extract"
        if (Test-Path -LiteralPath $bridgeExtractDir) {
            Remove-Item -LiteralPath $bridgeExtractDir -Recurse -Force
        }
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolScaffoldStarters") (Join-Path $stageTools "UnrealMcpToolScaffoldStarters") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolScaffolds/fps_bootstrap") (Join-Path $stageTools "UnrealMcpToolScaffolds/fps_bootstrap") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn") (Join-Path $stageTools "UnrealMcpToolScaffolds/verify_input_drives_pawn") @(".DS_Store", "Saved")

        $stageDocs = Join-Path $stageParent "Docs"
        New-Item -ItemType Directory -Force -Path $stageDocs | Out-Null
        Copy-Item -LiteralPath $firstLaunchDoc -Destination (Join-Path $stageDocs "FIRST_LAUNCH.md") -Force
        if (Test-Path -LiteralPath $releaseDoc -PathType Leaf) {
            Copy-Item -LiteralPath $releaseDoc -Destination (Join-Path $stageDocs "Release-2026-05.md") -Force
        }
        Copy-Item -LiteralPath $stage2Doc -Destination (Join-Path $stageDocs "Stage2WindowsVerify.md") -Force
        if (Test-Path -LiteralPath $schemasDir -PathType Container) {
            Copy-CleanDirectory $schemasDir (Join-Path $stageParent "Schemas") @(".DS_Store", "Saved")
        }
        Copy-Item -LiteralPath $installResource -Destination (Join-Path $stageParent "README-FULL.md") -Force
        Copy-Item -LiteralPath $installResource -Destination (Join-Path $stageParent "INSTALL.md") -Force
        Copy-Item -LiteralPath @(
            (Join-Path $repoRoot "Tools/first_launch_build.ps1"),
            (Join-Path $repoRoot "Tools/first_launch_build.cmd")
        ) -Destination $stageParent -Force

        Assert-PlainFile (Join-Path $stagePlugin "UnrealMcp.uplugin") `
            "Staging integrity failure: missing Plugins/UnrealMcp/UnrealMcp.uplugin" `
            "Staging integrity failure: staged uplugin is a symlink"
        Assert-PlainFile (Join-Path $stagePlugin "Binaries/Win64/UnrealEditor-UnrealMcp.dll") `
            "Staging integrity failure: missing bundled UnrealEditor-UnrealMcp.dll" `
            "Staging integrity failure: bundled UnrealEditor-UnrealMcp.dll is a symlink"
        Assert-PlainFile (Join-Path $stagePlugin "Binaries/Win64/UnrealEditor.modules") `
            "Staging integrity failure: missing bundled UnrealEditor.modules" `
            "Staging integrity failure: bundled UnrealEditor.modules is a symlink"
        Assert-SameFileHash (Join-Path $stagePlugin "Resources/ToolRegistry/tools.json") `
            $canonicalRegistry "Staging integrity failure: staged plugin registry differs from canonical registry"
        Assert-SameFileHash (Join-Path $stageTools "UnrealMcpToolRegistry/tools.json") `
            $canonicalRegistry "Staging integrity failure: staged Tools registry differs from canonical registry"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpPyTools/editor_python_runtime_info/main.py") `
            "Staging integrity failure: missing Tools/UnrealMcpPyTools/editor_python_runtime_info/main.py" `
            "Staging integrity failure: staged Python handler is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json") `
            "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json" `
            "Staging integrity failure: staged fps_bootstrap scaffold metadata is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json") `
            "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json" `
            "Staging integrity failure: staged verify_input_drives_pawn scaffold metadata is a symlink"
        Assert-BridgeBundle -BridgeRoot (Join-Path $stageTools "UnrealMcpCodexBridge")

        $excludedPath = Get-ChildItem -LiteralPath $stageParent -Force -Recurse |
            Where-Object { (@("Intermediate", "Saved", "DerivedDataCache", ".DS_Store", "__pycache__") -contains $_.Name) -or ($_.Name -like "*.pyc") } |
            Select-Object -First 1
        if ($null -ne $excludedPath) {
            Die "Staging integrity failure: excluded path present: $($excludedPath.FullName)"
        }

        $zipName = "UnrealMcp-v$Version-full-win-$EngineTag.zip"
    } else {
        $stagePlugin = Join-Path $stageParent "Plugins/UnrealMcp"
        $stageTools = Join-Path $stageParent "Tools"
        $stagePyTools = Join-Path $stageTools "UnrealMcpPyTools"
        $stageScaffoldStarters = Join-Path $stageTools "UnrealMcpToolScaffoldStarters"
        $stageDocs = Join-Path $stageParent "Docs"

        # Stage a clean source-only project-root overlay. Automation tests and
        # generated build products stay out of the pilot zip.
        Copy-CleanDirectory $pluginDir $stagePlugin @("Binaries", "Intermediate", "Saved", "DerivedDataCache", ".DS_Store")

        $stageTests = Join-Path $stagePlugin "Source/UnrealMcp/Private/Tests"
        if (Test-Path -LiteralPath $stageTests) {
            Remove-Item -LiteralPath $stageTests -Recurse -Force
        }

        # Python handlers are resolved at runtime from
        # <ProjectDir>/Tools/UnrealMcpPyTools/<handlerId>/main.py, so source-only
        # pilots must include this project-root Tools tree alongside the plugin.
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolRegistry") (Join-Path $stageTools "UnrealMcpToolRegistry") @(".DS_Store", "Saved")
        Copy-CleanDirectory $pythonToolsDir $stagePyTools @("__pycache__", "*.pyc", ".DS_Store")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpSkills") (Join-Path $stageTools "UnrealMcpSkills") @(".DS_Store", ".Rhistory", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpKnowledge") (Join-Path $stageTools "UnrealMcpKnowledge") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpTests") (Join-Path $stageTools "UnrealMcpTests") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpCodexBridge") (Join-Path $stageTools "UnrealMcpCodexBridge") @("node_modules", "runtime", "Intermediate", "Saved", "DerivedDataCache", ".DS_Store")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolScaffoldStarters") $stageScaffoldStarters @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolScaffolds/fps_bootstrap") (Join-Path $stageTools "UnrealMcpToolScaffolds/fps_bootstrap") @(".DS_Store", "Saved")
        Copy-CleanDirectory (Join-Path $repoRoot "Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn") (Join-Path $stageTools "UnrealMcpToolScaffolds/verify_input_drives_pawn") @(".DS_Store", "Saved")

        Copy-Item -LiteralPath $installResource -Destination (Join-Path $stagePlugin "INSTALL.md") -Force
        New-Item -ItemType Directory -Force -Path $stageDocs | Out-Null
        Copy-Item -LiteralPath $firstLaunchDoc -Destination (Join-Path $stageDocs "FIRST_LAUNCH.md") -Force

        # Verify the staged tree rather than trusting copy excludes blindly.
        if (-not (Test-Path -LiteralPath (Join-Path $stagePlugin "UnrealMcp.uplugin") -PathType Leaf)) {
            Die "Staging integrity failure: missing Plugins/UnrealMcp/UnrealMcp.uplugin"
        }
        Assert-PlainFile (Join-Path $stagePlugin "Resources/ToolRegistry/tools.json") `
            "Staging integrity failure: missing Plugins/UnrealMcp/Resources/ToolRegistry/tools.json" `
            "Staging integrity failure: staged registry is a symlink"
        Assert-SameFileHash (Join-Path $stagePlugin "Resources/ToolRegistry/tools.json") `
            $canonicalRegistry "Staging integrity failure: staged registry differs from canonical registry"
        if (-not (Test-Path -LiteralPath (Join-Path $stagePlugin "INSTALL.md") -PathType Leaf)) {
            Die "Staging integrity failure: missing Plugins/UnrealMcp/INSTALL.md"
        }
        Assert-SameFileHash (Join-Path $stageTools "UnrealMcpToolRegistry/tools.json") `
            $canonicalRegistry "Staging integrity failure: staged Tools registry differs from canonical registry"
        Assert-SameFileHash (Join-Path $stageTools "UnrealMcpToolRegistry/schema.json") `
            $canonicalRegistrySchema "Staging integrity failure: staged Tools registry schema differs from canonical schema"
        Assert-PlainFile (Join-Path $stagePyTools "editor_python_runtime_info/main.py") `
            "Staging integrity failure: missing Tools/UnrealMcpPyTools/editor_python_runtime_info/main.py" `
            "Staging integrity failure: staged Python handler is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpSkills/mcp-self-extension/SKILL.md") `
            "Staging integrity failure: missing Tools/UnrealMcpSkills/mcp-self-extension/SKILL.md" `
            "Staging integrity failure: staged skill file is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpKnowledge/Evals/core_rag_eval.json") `
            "Staging integrity failure: missing Tools/UnrealMcpKnowledge/Evals/core_rag_eval.json" `
            "Staging integrity failure: staged RAG eval is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpTests/Core/editor_status_valid.json") `
            "Staging integrity failure: missing Tools/UnrealMcpTests/Core/editor_status_valid.json" `
            "Staging integrity failure: staged core test fixture is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpCodexBridge/package.json") `
            "Staging integrity failure: missing Tools/UnrealMcpCodexBridge/package.json" `
            "Staging integrity failure: staged bridge package.json is a symlink"
        if (Test-Path -LiteralPath (Join-Path $stageTools "UnrealMcpCodexBridge/node_modules")) {
            Die "Staging integrity failure: source-only bridge includes node_modules/"
        }
        if (Test-Path -LiteralPath (Join-Path $stageTools "UnrealMcpCodexBridge/runtime")) {
            Die "Staging integrity failure: source-only bridge includes runtime/"
        }
        Assert-PlainFile (Join-Path $stageScaffoldStarters "README.md") `
            "Staging integrity failure: missing Tools/UnrealMcpToolScaffoldStarters/README.md" `
            "Staging integrity failure: staged scaffold starters README is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json") `
            "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json" `
            "Staging integrity failure: staged fps_bootstrap scaffold metadata is a symlink"
        Assert-PlainFile (Join-Path $stageTools "UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json") `
            "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json" `
            "Staging integrity failure: staged verify_input_drives_pawn scaffold metadata is a symlink"
        Assert-PlainFile (Join-Path $stageDocs "FIRST_LAUNCH.md") `
            "Staging integrity failure: missing Docs/FIRST_LAUNCH.md" `
            "Staging integrity failure: staged first-launch doc is a symlink"

        $excludedNames = @("Binaries", "Intermediate", "Saved", "DerivedDataCache", ".DS_Store", ".Rhistory", "node_modules", "runtime")
        $excludedPath = Get-ChildItem -LiteralPath $stageParent -Force -Recurse |
            Where-Object { ($excludedNames -contains $_.Name) -or ($_.Name -eq "__pycache__") -or ($_.Name -like "*.pyc") } |
            Select-Object -First 1
        if ($null -ne $excludedPath) {
            Die "Staging integrity failure: excluded path present: $($excludedPath.FullName)"
        }
        if (Test-Path -LiteralPath $stageTests) {
            Die "Staging integrity failure: excluded path present: Plugins/UnrealMcp/Source/UnrealMcp/Private/Tests"
        }

        $zipName = "UnrealMcp-v$Version-win-ue56-ue57-projectroot.zip"
    }
    $zipPath = Join-Path $outputDir $zipName
    $shaPath = "$zipPath.sha256"

    Remove-Item -LiteralPath $zipPath, $shaPath -Force -ErrorAction SilentlyContinue
    if ($FullExperience) {
        Compress-Archive -Path (Join-Path $stageParent "*") -DestinationPath $zipPath -CompressionLevel Optimal
    } else {
        Compress-Archive -Path (Join-Path $stageParent "*") -DestinationPath $zipPath -CompressionLevel Optimal
    }

    # The sidecar uses the same two-space format produced by shasum.
    $shaValue = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath $shaPath -Encoding ASCII -Value "$shaValue  $zipName"
    $sizeMiB = "{0:N2}" -f ((Get-Item -LiteralPath $zipPath).Length / 1MB)

    Write-Host "Zip path: $zipPath"
    Write-Host "Zip size: $sizeMiB MiB"
    Write-Host "SHA-256: $shaValue"
    if ($FullExperience) {
        Write-Host "Done. Next: open this on a clean Windows UE 5.6.1 project; see Docs/FIRST_LAUNCH.md."
    } else {
        Write-Host "Done. Next: extract the zip into a pilot user's <UserProject> root, next to the .uproject; do not extract it under Plugins/. See Plugins/UnrealMcp/INSTALL.md inside the zip."
    }
} catch {
    Write-Error -Message "Error: $($_.Exception.Message)" -ErrorAction Continue
    if ($null -ne $_.InvocationInfo -and -not [string]::IsNullOrWhiteSpace($_.InvocationInfo.PositionMessage)) {
        Write-Error -Message $_.InvocationInfo.PositionMessage -ErrorAction Continue
    }
    if (-not [string]::IsNullOrWhiteSpace($_.ScriptStackTrace)) {
        Write-Error -Message $_.ScriptStackTrace -ErrorAction Continue
    }
    exit 1
} finally {
    if ((-not [string]::IsNullOrWhiteSpace($stageParent)) -and (Test-Path -LiteralPath $stageParent)) {
        Remove-Item -LiteralPath $stageParent -Recurse -Force -ErrorAction SilentlyContinue
    }
}
