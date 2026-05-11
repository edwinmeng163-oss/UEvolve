Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Opens Examples/UEvolveExample57 (UE 5.7.4 variant).
$ProjectPath = Join-Path $PSScriptRoot "Examples\UEvolveExample57\UEvolveExample57.uproject"
if (-not (Test-Path -LiteralPath $ProjectPath)) {
    throw "UEvolveExample57.uproject was not found at: $ProjectPath"
}

Start-Process -FilePath $ProjectPath
