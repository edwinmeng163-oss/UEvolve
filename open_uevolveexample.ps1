Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Opens Examples/UEvolveExample (UE 5.6.1 variant).
$ProjectPath = Join-Path $PSScriptRoot "Examples\UEvolveExample\UEvolveExample.uproject"
if (-not (Test-Path -LiteralPath $ProjectPath)) {
    throw "UEvolveExample.uproject was not found at: $ProjectPath"
}

Start-Process -FilePath $ProjectPath
