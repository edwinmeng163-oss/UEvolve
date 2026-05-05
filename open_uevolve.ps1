Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectPath = Join-Path $PSScriptRoot "UEvolve.uproject"
if (-not (Test-Path -LiteralPath $ProjectPath)) {
    throw "UEvolve.uproject was not found next to this script: $ProjectPath"
}

Start-Process -FilePath $ProjectPath
