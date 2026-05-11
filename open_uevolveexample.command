#!/bin/zsh
set -euo pipefail

# Opens Examples/UEvolveExample (UE 5.6.1 variant).
SCRIPT_DIR="${0:A:h}"
open "$SCRIPT_DIR/Examples/UEvolveExample/UEvolveExample.uproject"
