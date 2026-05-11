#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
git config --local core.hooksPath Tools/git-hooks
echo "Installed pre-commit hook via core.hooksPath. Existing .git/hooks/ is now ignored."
