#!/usr/bin/env bash
set -euo pipefail

die() {
  echo "Error: $*" >&2
  exit 1
}

usage() {
  cat <<'USAGE'
Usage: bash Tools/package_plugin.sh [--output <dir>] [--version <name>]
                                    [--full-experience]
                                    [--prebuilt-binaries-path <dir>]
                                    [--bridge-bundle-path <dir-or-archive>]
                                    [--engine-tag <tag>]

Builds a source-only project-root UnrealMcp zip for macOS UE 5.6/5.7 pilots.
With --full-experience, builds a project-root zip that includes plugin source,
prebuilt Win64 UE 5.6.1 binaries, Tools starters, docs, schemas, and the
offline Codex bridge bundle.
USAGE
}

copy_clean_dir() {
  src="$1"
  dest="$2"
  shift 2
  [ -d "$src" ] || die "Missing directory: $src"
  mkdir -p "$dest"
  rsync --archive --delete "$@" "$src/" "$dest/"
}

resolve_prebuilt_win64_dir() {
  [ -n "$prebuilt_binaries_path" ] || die "Full-experience packaging requires --prebuilt-binaries-path. Produce binaries on a Windows machine first; see Tools/bundle_prebuilt_binaries_win.md."
  [ -d "$prebuilt_binaries_path" ] || die "Prebuilt binaries path does not exist: $prebuilt_binaries_path"

  for candidate in \
    "$prebuilt_binaries_path/Plugins/UnrealMcp/Binaries/Win64" \
    "$prebuilt_binaries_path/Binaries/Win64" \
    "$prebuilt_binaries_path"; do
    if [ -f "$candidate/UnrealEditor-UnrealMcp.dll" ] && [ -f "$candidate/UnrealEditor.modules" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  die "Prebuilt binaries must contain Plugins/UnrealMcp/Binaries/Win64/UnrealEditor-UnrealMcp.dll and UnrealEditor.modules"
}

find_bridge_root() {
  search_root="$1"
  if [ -f "$search_root/start-bridge.cmd" ] && [ -f "$search_root/package.json" ]; then
    printf '%s\n' "$search_root"
    return 0
  fi
  start_bridge="$(find "$search_root" -type f -name start-bridge.cmd -print | head -n 1)"
  [ -n "$start_bridge" ] || return 1
  dirname "$start_bridge"
}

assert_bridge_bundle() {
  bridge_root="$1"
  [ -f "$bridge_root/start-bridge.cmd" ] || die "Bridge bundle missing start-bridge.cmd"
  [ -f "$bridge_root/package.json" ] || die "Bridge bundle missing package.json"
  [ -d "$bridge_root/node_modules" ] || die "Bridge bundle missing node_modules/"
  [ -f "$bridge_root/runtime/bun.exe" ] || die "Bridge bundle missing runtime/bun.exe"
}

resolve_bridge_root() {
  [ -n "$bridge_bundle_path" ] || die "Full-experience packaging requires --bridge-bundle-path"
  [ -e "$bridge_bundle_path" ] || die "Bridge bundle path does not exist: $bridge_bundle_path"

  if [ -d "$bridge_bundle_path" ]; then
    bridge_root="$(find_bridge_root "$bridge_bundle_path")" || die "Bridge bundle does not contain start-bridge.cmd"
    assert_bridge_bundle "$bridge_root"
    printf '%s\n' "$bridge_root"
    return 0
  fi

  bridge_extract_dir="$stage_parent/bridge-bundle-extract"
  mkdir -p "$bridge_extract_dir"
  case "$bridge_bundle_path" in
    *.zip)
      command -v unzip >/dev/null 2>&1 || die "Bridge bundle zip extraction requires unzip"
      unzip -q "$bridge_bundle_path" -d "$bridge_extract_dir"
      ;;
    *)
      tar -xf "$bridge_bundle_path" -C "$bridge_extract_dir"
      ;;
  esac
  bridge_root="$(find_bridge_root "$bridge_extract_dir")" || die "Bridge bundle does not contain start-bridge.cmd"
  assert_bridge_bundle "$bridge_root"
  printf '%s\n' "$bridge_root"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

output_dir="Saved/UnrealMcp/Packages"
version_name=""
stage_parent=""
full_experience=0
prebuilt_binaries_path=""
bridge_bundle_path=""
engine_tag="ue561"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --output)
      [ "$#" -ge 2 ] || die "--output requires a directory"
      output_dir="$2"
      shift 2
      ;;
    --version)
      [ "$#" -ge 2 ] || die "--version requires a name"
      version_name="$2"
      shift 2
      ;;
    --full-experience)
      full_experience=1
      shift
      ;;
    --prebuilt-binaries-path)
      [ "$#" -ge 2 ] || die "--prebuilt-binaries-path requires a directory"
      prebuilt_binaries_path="$2"
      shift 2
      ;;
    --bridge-bundle-path)
      [ "$#" -ge 2 ] || die "--bridge-bundle-path requires a directory or archive"
      bridge_bundle_path="$2"
      shift 2
      ;;
    --engine-tag)
      [ "$#" -ge 2 ] || die "--engine-tag requires a tag"
      engine_tag="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
done

case "$output_dir" in
  /*) ;;
  *) output_dir="$repo_root/$output_dir" ;;
esac

plugin_dir="$repo_root/Plugins/UnrealMcp"
python_tools_dir="$repo_root/Tools/UnrealMcpPyTools"
uplugin="$plugin_dir/UnrealMcp.uplugin"
canonical_registry="$repo_root/Tools/UnrealMcpToolRegistry/tools.json"
canonical_registry_schema="$repo_root/Tools/UnrealMcpToolRegistry/schema.json"
mirror_registry="$plugin_dir/Resources/ToolRegistry/tools.json"
install_resource="$repo_root/Tools/PackagingResources/INSTALL.md"
first_launch_doc="$repo_root/Docs/FIRST_LAUNCH.md"
release_doc="$repo_root/Docs/Release-2026-05.md"
stage2_doc="$repo_root/Docs/Stage2WindowsVerify.md"
schemas_dir="$repo_root/Schemas"

cleanup() {
  if [ -n "$stage_parent" ] && [ -d "$stage_parent" ]; then
    rm -rf "$stage_parent"
  fi
}
trap cleanup EXIT

[ -f "$uplugin" ] || die "Missing plugin descriptor: Plugins/UnrealMcp/UnrealMcp.uplugin"

# Pre-flight: fail before staging if the descriptor, registry mirror, or
# compatibility validators are not in the exact state expected for release.
parsed_version="$(sed -n 's/^[[:space:]]*"VersionName"[[:space:]]*:[[:space:]]*"\([^"]*\)".*$/\1/p' "$uplugin" | head -n 1)"
[ -n "$parsed_version" ] || die "Could not parse VersionName from Plugins/UnrealMcp/UnrealMcp.uplugin"

if [ -z "$version_name" ]; then
  version_name="$parsed_version"
fi

[ -f "$mirror_registry" ] || die "Missing plugin registry mirror: Plugins/UnrealMcp/Resources/ToolRegistry/tools.json"
[ ! -L "$mirror_registry" ] || die "Phase 1 fix not applied: see commit 00fbf5e"
[ -f "$canonical_registry" ] || die "Missing canonical registry: Tools/UnrealMcpToolRegistry/tools.json"
[ -f "$canonical_registry_schema" ] || die "Missing canonical registry schema: Tools/UnrealMcpToolRegistry/schema.json"
[ -d "$python_tools_dir" ] || die "Missing Python tool handlers: Tools/UnrealMcpPyTools"
cmp -s "$mirror_registry" "$canonical_registry" || die 'Registry mirror mismatch; run `python3 Tools/validate_tool_registry.py` and resync'

if ! (cd "$repo_root" && python3 Tools/validate_tool_registry.py); then
  die "Registry validator failure; fix before packaging"
fi

if ! (cd "$repo_root" && python3 Tools/check_ue56_compat.py); then
  die "UE 5.6 compatibility check failure; fix before packaging"
fi

[ -f "$install_resource" ] || die "Missing install resource: Tools/PackagingResources/INSTALL.md"
[ -f "$first_launch_doc" ] || die "Missing first-launch doc: Docs/FIRST_LAUNCH.md"
if [ "$full_experience" -eq 1 ]; then
  if [ -z "$prebuilt_binaries_path" ]; then
    die "Full-experience packaging requires --prebuilt-binaries-path. Produce binaries on a Windows machine first; see Tools/bundle_prebuilt_binaries_win.md."
  fi
fi

mkdir -p "$output_dir"
stage_parent="$(mktemp -d "$output_dir/.stage-$(date +%Y%m%d-%H%M%S).XXXXXX")"

if [ "$full_experience" -eq 1 ]; then
  prebuilt_win64="$(resolve_prebuilt_win64_dir)"
  bridge_root="$(resolve_bridge_root)"
  stage_plugin="$stage_parent/Plugins/UnrealMcp"
  stage_tools="$stage_parent/Tools"

  copy_clean_dir "$plugin_dir" "$stage_plugin" \
    --exclude 'Binaries/' --exclude 'Intermediate/' --exclude 'Saved/' \
    --exclude 'DerivedDataCache/' --exclude '.DS_Store'
  copy_clean_dir "$prebuilt_win64" "$stage_plugin/Binaries/Win64" \
    --exclude 'Intermediate/' --exclude 'Saved/' --exclude 'DerivedDataCache/' --exclude '.DS_Store'
  cp "$install_resource" "$stage_plugin/INSTALL.md"

  copy_clean_dir "$repo_root/Tools/UnrealMcpToolRegistry" "$stage_tools/UnrealMcpToolRegistry" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$python_tools_dir" "$stage_tools/UnrealMcpPyTools" \
    --exclude '__pycache__/' --exclude '*.pyc' --exclude '.DS_Store'
  copy_clean_dir "$repo_root/Tools/UnrealMcpSkills/mcp-self-extension" "$stage_tools/UnrealMcpSkills/mcp-self-extension" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpKnowledge/Sources" "$stage_tools/UnrealMcpKnowledge/Sources" --exclude '.DS_Store' --exclude 'Saved/'
  mkdir -p "$stage_tools/UnrealMcpKnowledge/Evals"
  cp "$repo_root/Tools/UnrealMcpKnowledge/Evals/core_rag_eval.json" "$stage_tools/UnrealMcpKnowledge/Evals/core_rag_eval.json"
  copy_clean_dir "$repo_root/Tools/UnrealMcpTests/Core" "$stage_tools/UnrealMcpTests/Core" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpTests/SelfExtension" "$stage_tools/UnrealMcpTests/SelfExtension" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpTests/Knowledge/closed_loop" "$stage_tools/UnrealMcpTests/Knowledge/closed_loop" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$bridge_root" "$stage_tools/UnrealMcpCodexBridge" \
    --exclude 'Intermediate/' --exclude 'Saved/' --exclude 'DerivedDataCache/' --exclude '.DS_Store'
  rm -rf "$stage_parent/bridge-bundle-extract"
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolScaffoldStarters" "$stage_tools/UnrealMcpToolScaffoldStarters" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolScaffolds/fps_bootstrap" "$stage_tools/UnrealMcpToolScaffolds/fps_bootstrap" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn" "$stage_tools/UnrealMcpToolScaffolds/verify_input_drives_pawn" --exclude '.DS_Store' --exclude 'Saved/'

  mkdir -p "$stage_parent/Docs"
  cp "$first_launch_doc" "$stage_parent/Docs/FIRST_LAUNCH.md"
  [ ! -f "$release_doc" ] || cp "$release_doc" "$stage_parent/Docs/Release-2026-05.md"
  cp "$stage2_doc" "$stage_parent/Docs/Stage2WindowsVerify.md"
  [ ! -d "$schemas_dir" ] || copy_clean_dir "$schemas_dir" "$stage_parent/Schemas" --exclude '.DS_Store' --exclude 'Saved/'
  cp "$install_resource" "$stage_parent/README-FULL.md"
  cp "$install_resource" "$stage_parent/INSTALL.md"

  [ -f "$stage_plugin/UnrealMcp.uplugin" ] || die "Staging integrity failure: missing Plugins/UnrealMcp/UnrealMcp.uplugin"
  [ -f "$stage_plugin/Binaries/Win64/UnrealEditor-UnrealMcp.dll" ] || die "Staging integrity failure: missing bundled UnrealEditor-UnrealMcp.dll"
  [ -f "$stage_plugin/Binaries/Win64/UnrealEditor.modules" ] || die "Staging integrity failure: missing bundled UnrealEditor.modules"
  cmp -s "$stage_plugin/Resources/ToolRegistry/tools.json" "$canonical_registry" || die "Staging integrity failure: staged plugin registry differs from canonical registry"
  cmp -s "$stage_tools/UnrealMcpToolRegistry/tools.json" "$canonical_registry" || die "Staging integrity failure: staged Tools registry differs from canonical registry"
  [ -f "$stage_tools/UnrealMcpPyTools/editor_python_runtime_info/main.py" ] || die "Staging integrity failure: missing Tools/UnrealMcpPyTools/editor_python_runtime_info/main.py"
  [ -f "$stage_tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json"
  [ -f "$stage_tools/UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json"
  assert_bridge_bundle "$stage_tools/UnrealMcpCodexBridge"

  excluded_paths="$(find "$stage_parent" \
    \( -name Intermediate -o -name Saved -o -name DerivedDataCache -o -name .DS_Store -o -name __pycache__ -o -name '*.pyc' \) \
    -print)"
  [ -z "$excluded_paths" ] || die "Staging integrity failure: excluded path present: $(printf '%s' "$excluded_paths" | head -n 1)"

  zip_name="UnrealMcp-v${version_name}-full-win-${engine_tag}.zip"
else
  stage_plugin="$stage_parent/Plugins/UnrealMcp"
  stage_tools="$stage_parent/Tools"
  stage_py_tools="$stage_tools/UnrealMcpPyTools"
  stage_scaffold_starters="$stage_tools/UnrealMcpToolScaffoldStarters"
  stage_docs="$stage_parent/Docs"
  mkdir -p "$(dirname "$stage_plugin")" "$stage_tools" "$stage_docs"

  # Stage a clean source-only project-root overlay. Automation tests and
  # generated build products stay out of the pilot zip.
  rsync --archive --delete \
    --exclude 'Binaries/' \
    --exclude 'Intermediate/' \
    --exclude 'Saved/' \
    --exclude 'DerivedDataCache/' \
    --exclude '.DS_Store' \
    --exclude 'Source/UnrealMcp/Private/Tests/' \
    "$plugin_dir/" "$stage_plugin/"

  # Python handlers are resolved at runtime from
  # <ProjectDir>/Tools/UnrealMcpPyTools/<handlerId>/main.py, so source-only
  # pilots must include this project-root Tools tree alongside the plugin.
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolRegistry" "$stage_tools/UnrealMcpToolRegistry" \
    --exclude '.DS_Store' --exclude 'Saved/'
  rsync --archive --delete \
    --exclude '__pycache__/' \
    --exclude '*.pyc' \
    --exclude '.DS_Store' \
    "$python_tools_dir/" "$stage_py_tools/"
  copy_clean_dir "$repo_root/Tools/UnrealMcpSkills" "$stage_tools/UnrealMcpSkills" \
    --exclude '.DS_Store' --exclude '.Rhistory' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpKnowledge" "$stage_tools/UnrealMcpKnowledge" \
    --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpTests" "$stage_tools/UnrealMcpTests" \
    --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpCodexBridge" "$stage_tools/UnrealMcpCodexBridge" \
    --exclude 'node_modules/' --exclude 'runtime/' --exclude 'Intermediate/' \
    --exclude 'Saved/' --exclude 'DerivedDataCache/' --exclude '.DS_Store'
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolScaffoldStarters" "$stage_scaffold_starters" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolScaffolds/fps_bootstrap" "$stage_tools/UnrealMcpToolScaffolds/fps_bootstrap" --exclude '.DS_Store' --exclude 'Saved/'
  copy_clean_dir "$repo_root/Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn" "$stage_tools/UnrealMcpToolScaffolds/verify_input_drives_pawn" --exclude '.DS_Store' --exclude 'Saved/'

  cp "$install_resource" "$stage_plugin/INSTALL.md"
  cp "$first_launch_doc" "$stage_docs/FIRST_LAUNCH.md"

  # Verify the staged tree rather than trusting rsync excludes blindly.
  [ -f "$stage_plugin/UnrealMcp.uplugin" ] || die "Staging integrity failure: missing Plugins/UnrealMcp/UnrealMcp.uplugin"
  [ -f "$stage_plugin/Resources/ToolRegistry/tools.json" ] || die "Staging integrity failure: missing Plugins/UnrealMcp/Resources/ToolRegistry/tools.json"
  [ ! -L "$stage_plugin/Resources/ToolRegistry/tools.json" ] || die "Staging integrity failure: staged registry is a symlink"
  cmp -s "$stage_plugin/Resources/ToolRegistry/tools.json" "$canonical_registry" || die "Staging integrity failure: staged registry differs from canonical registry"
  [ -f "$stage_plugin/INSTALL.md" ] || die "Staging integrity failure: missing Plugins/UnrealMcp/INSTALL.md"
  cmp -s "$stage_tools/UnrealMcpToolRegistry/tools.json" "$canonical_registry" || die "Staging integrity failure: staged Tools registry differs from canonical registry"
  cmp -s "$stage_tools/UnrealMcpToolRegistry/schema.json" "$canonical_registry_schema" || die "Staging integrity failure: staged Tools registry schema differs from canonical schema"
  [ -f "$stage_py_tools/editor_python_runtime_info/main.py" ] || die "Staging integrity failure: missing Tools/UnrealMcpPyTools/editor_python_runtime_info/main.py"
  [ -f "$stage_tools/UnrealMcpSkills/mcp-self-extension/SKILL.md" ] || die "Staging integrity failure: missing Tools/UnrealMcpSkills/mcp-self-extension/SKILL.md"
  [ -f "$stage_tools/UnrealMcpKnowledge/Evals/core_rag_eval.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpKnowledge/Evals/core_rag_eval.json"
  [ -f "$stage_tools/UnrealMcpTests/Core/editor_status_valid.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpTests/Core/editor_status_valid.json"
  [ -f "$stage_tools/UnrealMcpCodexBridge/package.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpCodexBridge/package.json"
  [ ! -e "$stage_tools/UnrealMcpCodexBridge/node_modules" ] || die "Staging integrity failure: source-only bridge includes node_modules/"
  [ ! -e "$stage_tools/UnrealMcpCodexBridge/runtime" ] || die "Staging integrity failure: source-only bridge includes runtime/"
  [ -f "$stage_scaffold_starters/README.md" ] || die "Staging integrity failure: missing Tools/UnrealMcpToolScaffoldStarters/README.md"
  [ -f "$stage_tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/fps_bootstrap/ScaffoldMetadata.json"
  [ -f "$stage_tools/UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json" ] || die "Staging integrity failure: missing Tools/UnrealMcpToolScaffolds/verify_input_drives_pawn/ScaffoldMetadata.json"
  [ -f "$stage_docs/FIRST_LAUNCH.md" ] || die "Staging integrity failure: missing Docs/FIRST_LAUNCH.md"

  excluded_paths="$(find "$stage_parent" \
    \( -name Binaries -o -name Intermediate -o -name Saved -o -name DerivedDataCache -o -name .DS_Store -o -name .Rhistory -o -name __pycache__ -o -name '*.pyc' -o -name node_modules -o -name runtime \) \
    -print)"
  [ -z "$excluded_paths" ] || die "Staging integrity failure: excluded path present: $(printf '%s' "$excluded_paths" | head -n 1)"
  [ ! -e "$stage_plugin/Source/UnrealMcp/Private/Tests" ] || die "Staging integrity failure: excluded path present: Plugins/UnrealMcp/Source/UnrealMcp/Private/Tests"

  zip_name="UnrealMcp-v${version_name}-mac-ue56-ue57-projectroot.zip"
fi
zip_path="$output_dir/$zip_name"
sha_path="$zip_path.sha256"

rm -f "$zip_path" "$sha_path"
(
  cd "$stage_parent"
  if [ "$full_experience" -eq 1 ]; then
    find . -mindepth 1 -maxdepth 1 -print | sed 's#^\./##' | LC_ALL=C sort |
      zip -r -X "$zip_path" -@ >/dev/null
  else
    find . -mindepth 1 -maxdepth 1 -print | sed 's#^\./##' | LC_ALL=C sort |
      zip -r -X "$zip_path" -@ >/dev/null
  fi
)

# The sidecar uses the same two-space format produced by shasum.
sha_line="$(cd "$output_dir" && shasum -a 256 "$zip_name")"
printf '%s\n' "$sha_line" > "$sha_path"
sha_value="$(printf '%s\n' "$sha_line" | awk '{print $1}')"
size_mib="$(du -m "$zip_path" | awk '{print $1}')"

rm -rf "$stage_parent"
stage_parent=""

printf 'Zip path: %s\n' "$zip_path"
printf 'Zip size: %s MiB\n' "$size_mib"
printf 'SHA-256: %s\n' "$sha_value"
if [ "$full_experience" -eq 1 ]; then
  printf 'Done. Next: open this on a clean Windows UE 5.6.1 project; see Docs/FIRST_LAUNCH.md.\n'
else
  printf "Done. Next: extract the zip into a pilot user's <UserProject>/ root, next to the .uproject; do not extract it under Plugins/. See Plugins/UnrealMcp/INSTALL.md inside the zip.\n"
fi
