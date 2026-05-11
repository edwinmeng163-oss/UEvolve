$ErrorActionPreference = "Stop"

$RepoRoot = git rev-parse --show-toplevel
Set-Location $RepoRoot
git config --local core.hooksPath Tools/git-hooks
Write-Host "Installed pre-commit hook via core.hooksPath. Existing .git/hooks/ is now ignored."
