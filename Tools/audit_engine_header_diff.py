#!/usr/bin/env python3
"""List Unreal Engine headers present in a newer install but absent in an older one."""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import sys


DEFAULT_OLD = "/Users/Shared/Epic Games/UE_5.6"
DEFAULT_NEW = "/Users/Shared/Epic Games/UE_5.7"
DEFAULT_MODULES_ROOT = "Engine/Source/Runtime"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit new-only UE headers.")
    parser.add_argument("--old", default=DEFAULT_OLD, help="Older UE root path")
    parser.add_argument("--new", default=DEFAULT_NEW, help="Newer UE root path")
    parser.add_argument("--modules-root", default=DEFAULT_MODULES_ROOT)
    parser.add_argument("--format", choices=("text", "markdown"), default="text")
    return parser.parse_args()


def version_label(engine_root: Path) -> str:
    name = engine_root.name.rstrip("/\\")
    if name.startswith("UE_"):
        return name[3:].replace("_", ".")
    return name


def normalize_header(path: Path, root: Path) -> tuple[str, str]:
    rel = path.relative_to(root)
    parts = rel.parts
    tail = parts[1:]
    if tail and tail[0] == "Public":
        tail = tail[1:]
    return parts[0], Path(*tail).as_posix()


def collect_headers(engine_root: Path, modules_root: str) -> dict[tuple[str, str], Path]:
    scan_root = engine_root / modules_root
    if not scan_root.is_dir():
        raise RuntimeError(f"missing modules root: {scan_root}")

    headers = {
        normalize_header(path, scan_root): path
        for path in sorted(scan_root.rglob("*.h"))
        if path.is_file()
    }
    if not headers:
        raise RuntimeError(f"no headers found under: {scan_root}")
    return headers


def group_new_only(
    old_headers: dict[tuple[str, str], Path],
    new_headers: dict[tuple[str, str], Path],
) -> dict[str, list[str]]:
    grouped: dict[str, list[str]] = defaultdict(list)
    for module, rel_path in sorted(set(new_headers) - set(old_headers)):
        grouped[module].append(rel_path)
    return dict(sorted(grouped.items()))


def candidate_sort_key(item: tuple[str, str]) -> tuple[int, int, int, str, str]:
    module, rel_path = item
    parts = Path(rel_path).parts
    prefix = parts[:1]
    return (
        1 if prefix in {("Private",), ("Internal",)} else 0,
        0 if prefix in {("Misc",), ("Containers",), ("HAL",), ("Math",)} else 1,
        len(parts),
        len(rel_path),
        f"{module}/{rel_path}",
    )


def print_text(grouped: dict[str, list[str]]) -> None:
    for module, paths in grouped.items():
        for rel_path in paths:
            print(f"{module}\t{rel_path}")


def print_markdown(
    grouped: dict[str, list[str]],
    old_root: Path,
    new_root: Path,
    modules_root: str,
) -> None:
    old_version = version_label(old_root)
    new_version = version_label(new_root)
    all_items = [(module, path) for module, paths in grouped.items() for path in paths]
    print("# Unreal Engine Header Diff Audit")
    print()
    print(f"Old source: `{old_root / modules_root}`")
    print(f"New source: `{new_root / modules_root}`")
    print()
    print('"New-only" means the header exists under the new source tree but not')
    print("under the old source tree after normalizing module `Public/` paths.")
    print()
    for module, paths in grouped.items():
        print(f"### {module}")
        print()
        for rel_path in paths:
            print(f"- `{rel_path}`")
        print()

    print("## Suggested FORBIDDEN_PATTERNS candidates")
    print()
    for _, rel_path in sorted(all_items, key=candidate_sort_key)[:5]:
        escaped = rel_path.replace(".", r"\.")
        print("```python")
        print("{")
        print(f'    "pattern": r\'^\\s*#include\\s+"{escaped}"\\s*$\',')
        print(f'    "reason": "{rel_path} is {new_version}+. Include UnrealMcpEngineCompat.h instead.",')
        print(f'    "added_in": "{new_version}",')
        print('    "severity": "warning",')
        print('    "suppress_when_guarded": True,')
        print("},")
        print("```")
        print()


def main() -> int:
    args = parse_args()
    old_root = Path(args.old).expanduser().resolve()
    new_root = Path(args.new).expanduser().resolve()

    try:
        old_headers = collect_headers(old_root, args.modules_root)
        new_headers = collect_headers(new_root, args.modules_root)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    grouped = group_new_only(old_headers, new_headers)
    if args.format == "markdown":
        print_markdown(grouped, old_root, new_root, args.modules_root)
    else:
        print_text(grouped)

    count = sum(len(paths) for paths in grouped.values())
    print(f"audit: {count} new-only headers across {len(grouped)} modules", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
