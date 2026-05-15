param([string]$Output = "Saved/UnrealMcp/Packages/UnrealMcp-CodexBridge-win-bundle.tar")

$ErrorActionPreference = "Stop"

function Die {
    param([string]$Message)
    Write-Error -Message "Error: $Message" -ErrorAction Continue
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDir "..")).Path
$bridgeDir = Join-Path $repoRoot "Tools/UnrealMcpCodexBridge"
if (-not (Test-Path -LiteralPath (Join-Path $bridgeDir "package.json") -PathType Leaf)) {
    Die "Missing bridge package.json: Tools/UnrealMcpCodexBridge/package.json"
}

if ([System.IO.Path]::IsPathRooted($Output)) {
    $outputPath = $Output
} else {
    $outputPath = Join-Path $repoRoot $Output
}
$outputDir = Split-Path -Parent $outputPath
$workRoot = Join-Path $outputDir ".bridge-bundle-work"
$stageBridge = Join-Path $workRoot "UnrealMcpCodexBridge"

Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $outputDir, $workRoot | Out-Null

$bunCommand = Get-Command "bun" -ErrorAction SilentlyContinue
if ($null -ne $bunCommand -and (Test-Path -LiteralPath $bunCommand.Source -PathType Leaf)) {
    $bunExe = $bunCommand.Source
} else {
    $runtimeDownload = Join-Path $workRoot "runtime-download"
    $bunZip = Join-Path $runtimeDownload "bun-windows-x64.zip"
    New-Item -ItemType Directory -Force -Path $runtimeDownload | Out-Null
    $bunUrl = "https://github.com/oven-sh/bun/releases/latest/download/bun-windows-x64.zip"
    Write-Host "Downloading Bun runtime from $bunUrl"
    Invoke-WebRequest -Uri $bunUrl -OutFile $bunZip
    # TODO: Verify SHA-256 when Bun publishes an official checksum sidecar for this asset.
    Expand-Archive -LiteralPath $bunZip -DestinationPath $runtimeDownload -Force
    $bunExeItem = Get-ChildItem -LiteralPath $runtimeDownload -Filter "bun.exe" -Recurse |
        Select-Object -First 1
    if ($null -eq $bunExeItem) {
        Die "Downloaded Bun archive did not contain bun.exe"
    }
    $bunExe = $bunExeItem.FullName
}

Push-Location $bridgeDir
try {
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $bunExe install 2>&1 | ForEach-Object { Write-Host $_ }
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if ($LASTEXITCODE -ne 0) {
        Die "bun install failed in Tools/UnrealMcpCodexBridge"
    }
} finally {
    Pop-Location
}

Copy-Item -LiteralPath $bridgeDir -Destination $stageBridge -Recurse -Force `
    -Exclude ".DS_Store", "Saved", "Intermediate", "DerivedDataCache"
New-Item -ItemType Directory -Force -Path (Join-Path $stageBridge "runtime") | Out-Null
Copy-Item -LiteralPath $bunExe -Destination (Join-Path $stageBridge "runtime/bun.exe") -Force

if (-not (Test-Path -LiteralPath (Join-Path $stageBridge "start-bridge.cmd") -PathType Leaf)) {
    Die "Bridge bundle staging missing start-bridge.cmd"
}
if (-not (Test-Path -LiteralPath (Join-Path $stageBridge "node_modules") -PathType Container)) {
    Die "Bridge bundle staging missing node_modules/"
}

Remove-Item -LiteralPath $outputPath -Force -ErrorAction SilentlyContinue
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    & tar -cf $outputPath -C $workRoot "UnrealMcpCodexBridge" 2>&1 | ForEach-Object { Write-Host $_ }
} finally {
    $ErrorActionPreference = $prevEAP
}
if ($LASTEXITCODE -ne 0) {
    Die "tar failed while writing bridge bundle"
}

Write-Host "Bridge bundle: $outputPath"
Write-Host "Next: pass this path to Tools/package_plugin.ps1 -FullExperience -BridgeBundlePath"
