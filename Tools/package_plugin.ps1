<#
Builds a source-only UnrealMcp plugin zip for Windows UE 5.6/5.7 pilots.

This PowerShell port is syntax-only verified on macOS. Stage 2 real Win64
verification on collaborator hardware is pending; track the pilot release at:
https://github.com/edwinmeng163-oss/UEvolve/releases/tag/v0.12.0-pilot-mac-ue56-ue57
#>

param([string]$Output = "Saved/UnrealMcp/Packages", [string]$Version = "")

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
        & $command.Source $ScriptPath
        if ($LASTEXITCODE -eq 0) {
            return $true
        }
    }
    return $false
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDir "..")).Path
if ([System.IO.Path]::IsPathRooted($Output)) {
    $outputDir = $Output
} else {
    $outputDir = Join-Path $repoRoot $Output
}
$pluginDir = Join-Path $repoRoot "Plugins/UnrealMcp"
$uplugin = Join-Path $pluginDir "UnrealMcp.uplugin"
$canonicalRegistry = Join-Path $repoRoot "Tools/UnrealMcpToolRegistry/tools.json"
$mirrorRegistry = Join-Path $pluginDir "Resources/ToolRegistry/tools.json"
$installResource = Join-Path $repoRoot "Tools/PackagingResources/INSTALL.md"
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

    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $stageParent = Join-Path $outputDir ".stage-$timestamp"
    if (Test-Path -LiteralPath $stageParent) {
        Die "Staging directory already exists: $stageParent"
    }
    New-Item -ItemType Directory -Path $stageParent | Out-Null
    $stagePlugin = Join-Path $stageParent "UnrealMcp"

    # Stage a clean source-only plugin tree. Automation tests and generated build
    # products stay out of the pilot zip.
    Copy-Item -LiteralPath $pluginDir -Destination $stageParent -Recurse -Force `
        -Exclude "Binaries", "Intermediate", "Saved", "DerivedDataCache", ".DS_Store", "Tests"

    $stageTests = Join-Path $stagePlugin "Source/UnrealMcp/Private/Tests"
    if (Test-Path -LiteralPath $stageTests) {
        Remove-Item -LiteralPath $stageTests -Recurse -Force
    }

    Copy-Item -LiteralPath $installResource -Destination (Join-Path $stagePlugin "INSTALL.md") -Force

    # Verify the staged tree rather than trusting Copy-Item excludes blindly.
    if (-not (Test-Path -LiteralPath (Join-Path $stagePlugin "UnrealMcp.uplugin") -PathType Leaf)) {
        Die "Staging integrity failure: missing UnrealMcp/UnrealMcp.uplugin"
    }
    Assert-PlainFile (Join-Path $stagePlugin "Resources/ToolRegistry/tools.json") `
        "Staging integrity failure: missing UnrealMcp/Resources/ToolRegistry/tools.json" `
        "Staging integrity failure: staged registry is a symlink"
    Assert-SameFileHash (Join-Path $stagePlugin "Resources/ToolRegistry/tools.json") `
        $canonicalRegistry "Staging integrity failure: staged registry differs from canonical registry"
    if (-not (Test-Path -LiteralPath (Join-Path $stagePlugin "INSTALL.md") -PathType Leaf)) {
        Die "Staging integrity failure: missing UnrealMcp/INSTALL.md"
    }

    $excludedNames = @("Binaries", "Intermediate", "Saved", "DerivedDataCache", ".DS_Store")
    $excludedPath = Get-ChildItem -LiteralPath $stagePlugin -Force -Recurse |
        Where-Object { $excludedNames -contains $_.Name } |
        Select-Object -First 1
    if ($null -ne $excludedPath) {
        Die "Staging integrity failure: excluded path present: $($excludedPath.FullName)"
    }
    if (Test-Path -LiteralPath $stageTests) {
        Die "Staging integrity failure: excluded path present: UnrealMcp/Source/UnrealMcp/Private/Tests"
    }

    $zipName = "UnrealMcp-v$Version-win-ue56-ue57-source.zip"
    $zipPath = Join-Path $outputDir $zipName
    $shaPath = "$zipPath.sha256"

    Remove-Item -LiteralPath $zipPath, $shaPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path $stagePlugin -DestinationPath $zipPath -CompressionLevel Optimal

    # The sidecar uses the same two-space format produced by shasum.
    $shaValue = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath $shaPath -Encoding ASCII -Value "$shaValue  $zipName"
    $sizeMiB = "{0:N2}" -f ((Get-Item -LiteralPath $zipPath).Length / 1MB)

    Write-Host "Zip path: $zipPath"
    Write-Host "Zip size: $sizeMiB MiB"
    Write-Host "SHA-256: $shaValue"
    Write-Host "Done. Next: drop the zip into a pilot user's <UserProject>/Plugins/ or <UE Install>/Engine/Plugins/. See INSTALL.md inside the zip."
} catch {
    Write-Error -Message "Error: $($_.Exception.Message)" -ErrorAction Continue
    exit 1
} finally {
    if ((-not [string]::IsNullOrWhiteSpace($stageParent)) -and (Test-Path -LiteralPath $stageParent)) {
        Remove-Item -LiteralPath $stageParent -Recurse -Force -ErrorAction SilentlyContinue
    }
}
