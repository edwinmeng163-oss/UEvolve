param([string]$ProjectDir = "")

$ErrorActionPreference = "Stop"

function Die {
    param([string]$Message)
    Write-Error -Message "Error: $Message" -ErrorAction Continue
    exit 1
}

if (-not [string]::IsNullOrWhiteSpace($ProjectDir)) {
    $resolvedProjectDir = Resolve-Path -LiteralPath $ProjectDir -ErrorAction SilentlyContinue
    if ($null -eq $resolvedProjectDir) { Die "Project directory does not exist: $ProjectDir" }
    $projectRoot = $resolvedProjectDir.Path
} else {
    $projectRoot = ""
    $candidateRoots = @((Get-Location).Path, $PSScriptRoot) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique
    foreach ($candidateRoot in $candidateRoots) {
        $candidateUProjects = @(Get-ChildItem -LiteralPath $candidateRoot -Filter "*.uproject" -File -ErrorAction SilentlyContinue)
        if ($candidateUProjects.Count -gt 0) {
            $projectRoot = (Resolve-Path -LiteralPath $candidateRoot).Path
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($projectRoot)) {
        $projectRoot = (Resolve-Path -LiteralPath (Get-Location).Path).Path
    }
}

$uprojects = @(Get-ChildItem -LiteralPath $projectRoot -Filter "*.uproject" -File -ErrorAction SilentlyContinue)
if ($uprojects.Count -eq 0) { Die "No .uproject file found in $projectRoot. Run this from your Unreal project root." }
if ($uprojects.Count -gt 1) {
    $names = ($uprojects | ForEach-Object { $_.Name }) -join ", "
    Die "Multiple .uproject files found in $projectRoot: $names"
}
$uprojectPath = $uprojects[0].FullName
$projectStem = [IO.Path]::GetFileNameWithoutExtension($uprojectPath)

try {
    $projectJson = Get-Content -LiteralPath $uprojectPath -Raw | ConvertFrom-Json
} catch {
    Die "Could not parse $uprojectPath as JSON: $($_.Exception.Message)"
}

Write-Host "[1/5] Project: $uprojectPath"
$sourceDir = Join-Path $projectRoot "Source"
$hasSource = Test-Path -LiteralPath $sourceDir -PathType Container
$hasModules = $false
if (($projectJson.PSObject.Properties.Name -contains "Modules") -and ($null -ne $projectJson.Modules)) {
    $hasModules = @($projectJson.Modules).Count -gt 0
}
if ((-not $hasSource) -and (-not $hasModules)) {
    Write-Host "Blueprint-only project detected; no build needed"
    exit 0
}

$targetFiles = @()
if ($hasSource) {
    $targetFiles = @(Get-ChildItem -LiteralPath $sourceDir -Filter "*Editor.Target.cs" -File -ErrorAction SilentlyContinue)
}
$targetNames = @($targetFiles | ForEach-Object { $_.Name -replace "\.Target\.cs$", "" })
if ($targetNames.Count -eq 1) {
    $targetName = $targetNames[0]
} elseif ($targetNames.Count -gt 1) {
    $preferredTarget = "${projectStem}Editor"
    if ($targetNames -contains $preferredTarget) {
        $targetName = $preferredTarget
    } else {
        Die "multiple editor targets found in $sourceDir: $($targetNames -join ', ')"
    }
} else {
    $targetName = "${projectStem}Editor"
}
Write-Host "[2/5] Project type: C++/first-launch build path"
Write-Host "[3/5] Editor target: $targetName"

$ueCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($env:UE_5_6_PATH)) { $ueCandidates += $env:UE_5_6_PATH }
$ueCandidates += "C:\Program Files\Epic Games\UE_5.6"
$ueRoot = ""
foreach ($ueCandidate in $ueCandidates) {
    $candidateBuildBat = Join-Path $ueCandidate "Engine\Build\BatchFiles\Build.bat"
    if ((Test-Path -LiteralPath $ueCandidate -PathType Container) -and (Test-Path -LiteralPath $candidateBuildBat -PathType Leaf)) {
        $ueRoot = (Resolve-Path -LiteralPath $ueCandidate).Path
        break
    }
}
if ([string]::IsNullOrWhiteSpace($ueRoot)) {
    Die "Set UE_5_6_PATH env var to your UE 5.6 root and rerun (expected Engine\Build\BatchFiles\Build.bat)."
}
$buildBat = Join-Path $ueRoot "Engine\Build\BatchFiles\Build.bat"
Write-Host "[4/5] Unreal Engine root: $ueRoot"

$logDir = Join-Path $projectRoot "Saved\Logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$buildLog = Join-Path $logDir "first_launch_build.log"
Remove-Item -LiteralPath $buildLog -Force -ErrorAction SilentlyContinue
$buildArgs = @($targetName, "Win64", "Development", "-Project=$uprojectPath", "-waitmutex")

Write-Host "[5/5] Running Build.bat; log: $buildLog"
$buildExitCode = 1
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    & {
        & $buildBat @buildArgs 2>&1
        # Capture before downstream Tee/logging can obscure Build.bat's exit code.
        $script:buildExitCode = $LASTEXITCODE
    } | Tee-Object -FilePath $buildLog
} finally {
    $ErrorActionPreference = $prevEAP
}
if ($buildExitCode -eq 0) {
    Write-Host "Build succeeded - log at $buildLog"
    exit 0
}
Write-Host "Build failed - log at $buildLog"
exit $buildExitCode
