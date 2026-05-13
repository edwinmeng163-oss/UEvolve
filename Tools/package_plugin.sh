#!/usr/bin/env bash
set -euo pipefail

die() {
  echo "Error: $*" >&2
  exit 1
}

usage() {
  cat <<'USAGE'
Usage: bash Tools/package_plugin.sh [--output <dir>] [--version <name>]

Builds a source-only UnrealMcp plugin zip for macOS UE 5.6/5.7 pilots.
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

output_dir="Saved/UnrealMcp/Packages"
version_name=""
stage_parent=""

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
uplugin="$plugin_dir/UnrealMcp.uplugin"
canonical_registry="$repo_root/Tools/UnrealMcpToolRegistry/tools.json"
mirror_registry="$plugin_dir/Resources/ToolRegistry/tools.json"
install_resource="$repo_root/Tools/PackagingResources/INSTALL.md"

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
cmp -s "$mirror_registry" "$canonical_registry" || die 'Registry mirror mismatch; run `python3 Tools/validate_tool_registry.py` and resync'

if ! (cd "$repo_root" && python3 Tools/validate_tool_registry.py); then
  die "Registry validator failure; fix before packaging"
fi

if ! (cd "$repo_root" && python3 Tools/check_ue56_compat.py); then
  die "UE 5.6 compatibility check failure; fix before packaging"
fi

[ -f "$install_resource" ] || die "Missing install resource: Tools/PackagingResources/INSTALL.md"

mkdir -p "$output_dir"
stage_parent="$(mktemp -d "$output_dir/.stage-$(date +%Y%m%d-%H%M%S).XXXXXX")"
stage_plugin="$stage_parent/UnrealMcp"

# Stage a clean source-only plugin tree. Automation tests and generated build
# products stay out of the pilot zip.
rsync --archive --delete \
  --exclude 'Binaries/' \
  --exclude 'Intermediate/' \
  --exclude 'Saved/' \
  --exclude 'DerivedDataCache/' \
  --exclude '.DS_Store' \
  --exclude 'Source/UnrealMcp/Private/Tests/' \
  "$plugin_dir/" "$stage_plugin/"

cp "$install_resource" "$stage_plugin/INSTALL.md"

# Verify the staged tree rather than trusting rsync excludes blindly.
[ -f "$stage_plugin/UnrealMcp.uplugin" ] || die "Staging integrity failure: missing UnrealMcp/UnrealMcp.uplugin"
[ -f "$stage_plugin/Resources/ToolRegistry/tools.json" ] || die "Staging integrity failure: missing UnrealMcp/Resources/ToolRegistry/tools.json"
[ ! -L "$stage_plugin/Resources/ToolRegistry/tools.json" ] || die "Staging integrity failure: staged registry is a symlink"
cmp -s "$stage_plugin/Resources/ToolRegistry/tools.json" "$canonical_registry" || die "Staging integrity failure: staged registry differs from canonical registry"
[ -f "$stage_plugin/INSTALL.md" ] || die "Staging integrity failure: missing UnrealMcp/INSTALL.md"

excluded_paths="$(find "$stage_plugin" \
  \( -name Binaries -o -name Intermediate -o -name Saved -o -name DerivedDataCache -o -name .DS_Store \) \
  -print)"
[ -z "$excluded_paths" ] || die "Staging integrity failure: excluded path present: $(printf '%s' "$excluded_paths" | head -n 1)"
[ ! -e "$stage_plugin/Source/UnrealMcp/Private/Tests" ] || die "Staging integrity failure: excluded path present: UnrealMcp/Source/UnrealMcp/Private/Tests"

zip_name="UnrealMcp-v${version_name}-mac-ue56-ue57-source.zip"
zip_path="$output_dir/$zip_name"
sha_path="$zip_path.sha256"

rm -f "$zip_path" "$sha_path"
(
  cd "$stage_parent"
  zip -X "$zip_path" UnrealMcp/ >/dev/null
  {
    printf '%s\n' 'UnrealMcp/UnrealMcp.uplugin' 'UnrealMcp/INSTALL.md'
    find UnrealMcp -mindepth 1 -maxdepth 1 \
      ! -path 'UnrealMcp/UnrealMcp.uplugin' \
      ! -path 'UnrealMcp/INSTALL.md' \
      -print | LC_ALL=C sort
  } | zip -r -X "$zip_path" -@ >/dev/null
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
printf "Done. Next: drop the zip into a pilot user's <UserProject>/Plugins/ or <UE Install>/Engine/Plugins/. See INSTALL.md inside the zip.\n"
