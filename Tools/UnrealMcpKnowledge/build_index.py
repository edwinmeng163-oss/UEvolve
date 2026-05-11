#!/usr/bin/env python3
"""Build UEvolve KnowledgeCard JSONL from versioned sources.

This mirrors the C++ knowledge_index_refresh path for CI/offline checks. It
intentionally ignores Saved/* so a clean checkout can validate the baseline RAG
index without Unreal Editor runtime state.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


DEFAULT_OUTPUT = Path("Saved/UnrealMcp/KnowledgeIndex/cards.jsonl")
DEFAULT_MAX_CHUNK_CHARS = 1800
DEFAULT_OVERLAP_CHARS = 160


def sanitize_knowledge_id(value: str) -> str:
    output = []
    for char in value.lower():
        if char.isalnum() or char in "_-.":
            output.append(char)
        else:
            output.append("-")
    sanitized = "".join(output)
    while "--" in sanitized:
        sanitized = sanitized.replace("--", "-")
    sanitized = sanitized.strip().strip("-")
    return sanitized or "knowledge-card"


def source_weight_for_kind(source_kind: str, category: str) -> float:
    if source_kind == "tool-registry":
        return 2.2
    if source_kind == "versioned-doc":
        return 1.7 if category == "uevolve-docs" else 1.45
    if source_kind == "skill":
        return 1.8
    if source_kind == "official-docs":
        return 1.15
    if source_kind == "runtime-memory":
        return 1.25
    return 1.0


def confidence_for_kind(source_kind: str) -> float:
    if source_kind == "tool-registry":
        return 0.95
    if source_kind == "versioned-doc":
        return 0.88
    if source_kind == "skill":
        return 0.86
    if source_kind == "official-docs":
        return 0.82
    return 0.72


def relative_path(project_dir: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(project_dir.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def flush_section(base_title: str, heading_stack: list[str], lines: list[str], sections: list[dict[str, Any]]) -> None:
    text = "\n".join(lines).strip()
    if not text:
        return
    title = heading_stack[-1] if heading_stack else base_title
    path = " > ".join(heading_stack) if heading_stack else base_title
    sections.append(
        {
            "title": title,
            "path": path,
            "text": text,
            "sectionIndex": len(sections),
        }
    )


def split_text_into_sections(base_title: str, text: str) -> list[dict[str, Any]]:
    sections: list[dict[str, Any]] = []
    heading_stack: list[str] = []
    current_lines: list[str] = []

    for line in text.splitlines():
        trimmed = line.strip()
        match = re.match(r"^(#{1,6})\s+(.+?)\s*$", trimmed)
        if match:
            flush_section(base_title, heading_stack, current_lines, sections)
            current_lines = []
            level = len(match.group(1))
            heading = match.group(2).strip() or base_title
            while len(heading_stack) >= level:
                heading_stack.pop()
            heading_stack.append(heading)
            current_lines.append(trimmed)
            continue
        current_lines.append(line)

    flush_section(base_title, heading_stack, current_lines, sections)
    if not sections and text.strip():
        sections.append({"title": base_title, "path": base_title, "text": text.strip(), "sectionIndex": 0})
    return sections


def add_cards_from_text(
    cards: list[dict[str, Any]],
    *,
    source_id: str,
    title: str,
    category: str,
    tags: list[str],
    source_kind: str,
    source_path: str,
    url: str,
    text: str,
    max_chunk_chars: int,
    overlap_chars: int,
    updated_at: str,
) -> None:
    clean_text = text.strip()
    if not clean_text:
        return

    safe_max = max(400, max_chunk_chars)
    safe_overlap = min(max(overlap_chars, 0), safe_max // 2)
    source_weight = source_weight_for_kind(source_kind, category)
    confidence = confidence_for_kind(source_kind)

    for section in split_text_into_sections(title, clean_text):
        section_text = section["text"].strip()
        offset = 0
        chunk_index = 0
        while offset < len(section_text):
            chunk_text = section_text[offset : offset + safe_max].strip()
            if chunk_text:
                section_title = section["title"] or title
                card_title = title if section_title == title else f"{title} / {section_title}"
                cards.append(
                    {
                        "cardId": f"{sanitize_knowledge_id(source_id)}:{section['sectionIndex']:03d}:{chunk_index:03d}",
                        "sourceId": source_id,
                        "title": card_title,
                        "sectionTitle": section_title,
                        "sectionPath": section["path"] or section_title,
                        "category": category,
                        "tags": tags,
                        "sourceKind": source_kind,
                        "sourcePath": source_path,
                        "url": url,
                        "text": chunk_text,
                        "chunkIndex": chunk_index,
                        "textLength": len(chunk_text),
                        "sourceWeight": source_weight,
                        "confidence": confidence,
                        "updatedAt": updated_at,
                    }
                )
            chunk_index += 1
            if offset + safe_max >= len(section_text):
                break
            offset += max(1, safe_max - safe_overlap)


def add_markdown_cards(
    cards: list[dict[str, Any]],
    project_dir: Path,
    file_path: Path,
    max_chunk_chars: int,
    overlap_chars: int,
    updated_at: str,
) -> None:
    text = file_path.read_text(encoding="utf-8")
    rel_path = relative_path(project_dir, file_path)
    add_cards_from_text(
        cards,
        source_id=sanitize_knowledge_id(rel_path),
        title=file_path.stem,
        category="uevolve-docs",
        tags=["uevolve", "docs"],
        source_kind="versioned-doc",
        source_path=rel_path,
        url="",
        text=text,
        max_chunk_chars=max_chunk_chars,
        overlap_chars=overlap_chars,
        updated_at=updated_at,
    )


def iter_versioned_markdown(project_dir: Path) -> Iterable[Path]:
    root_docs = [
        project_dir / "README.md",
        project_dir / "Plugins/UnrealMcp/README.md",
    ]
    for path in root_docs:
        if path.exists():
            yield path

    docs_dir = project_dir / "Docs"
    if docs_dir.exists():
        yield from sorted(docs_dir.glob("**/*.md"))


def tool_text(tool: dict[str, Any]) -> str:
    bool_fields = {
        "Requires write": "requiresWrite",
        "Requires build": "requiresBuild",
        "Requires external process": "requiresExternalProcess",
        "Dry-run support": "dryRunSupport",
        "Preflight support": "preflightSupport",
        "Postcheck support": "postcheckSupport",
    }
    lines = [
        f"Tool: {tool.get('name', '')}",
        f"Category: {tool.get('category', '')}",
        f"Risk: {tool.get('riskLevel', '')}",
    ]
    for label, key in bool_fields.items():
        lines.append(f"{label}: {'true' if tool.get(key) else 'false'}")
    lines.extend(
        [
            f"Description: {tool.get('reason', '')}",
            f"Reason: {tool.get('reason', '')}",
            f"Notes: {tool.get('notes', '')}",
        ]
    )
    return "\n".join(lines)


def add_tool_registry_cards(
    cards: list[dict[str, Any]],
    project_dir: Path,
    max_chunk_chars: int,
    overlap_chars: int,
    updated_at: str,
) -> None:
    registry_path = project_dir / "Tools/UnrealMcpToolRegistry/tools.json"
    registry = json.loads(registry_path.read_text(encoding="utf-8"))
    for tool in registry.get("tools", []):
        if tool.get("exposure") == "legacy_hidden":
            continue
        name = str(tool.get("name", "")).strip()
        if not name:
            continue
        add_cards_from_text(
            cards,
            source_id=name,
            title=name,
            category="mcp-tools",
            tags=[str(tool.get("category", "")), str(tool.get("riskLevel", ""))],
            source_kind="tool-registry",
            source_path="Tools/UnrealMcpToolRegistry/tools.json",
            url="",
            text=tool_text(tool),
            max_chunk_chars=max_chunk_chars,
            overlap_chars=overlap_chars,
            updated_at=updated_at,
        )


def validate_cards(project_dir: Path, cards: list[dict[str, Any]]) -> None:
    try:
        import jsonschema  # type: ignore
    except ImportError:
        print("Warning: jsonschema is not installed; skipping KnowledgeCard schema validation.", file=sys.stderr)
        return

    schema_path = project_dir / "Schemas/UnrealMcpKnowledgeCard.schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    for index, card in enumerate(cards):
        errors = sorted(validator.iter_errors(card), key=lambda error: list(error.path))
        if errors:
            location = ".".join(str(part) for part in errors[0].path) or "<root>"
            raise SystemExit(f"Card {index} failed schema validation at {location}: {errors[0].message}")


def build_cards(project_dir: Path, max_chunk_chars: int, overlap_chars: int) -> list[dict[str, Any]]:
    updated_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    cards: list[dict[str, Any]] = []
    for markdown_path in iter_versioned_markdown(project_dir):
        add_markdown_cards(cards, project_dir, markdown_path, max_chunk_chars, overlap_chars, updated_at)
    add_tool_registry_cards(cards, project_dir, max_chunk_chars, overlap_chars, updated_at)
    return cards


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build UEvolve KnowledgeCard JSONL from versioned sources.")
    parser.add_argument("--project-dir", default=".", help="Project directory. Defaults to the current working directory.")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT), help="Output cards.jsonl path, relative to project-dir unless absolute.")
    parser.add_argument("--max-chunk-chars", type=int, default=DEFAULT_MAX_CHUNK_CHARS, help="Maximum characters per text chunk.")
    parser.add_argument("--overlap-chars", type=int, default=DEFAULT_OVERLAP_CHARS, help="Chunk overlap in characters.")
    parser.add_argument("--validate-only", action="store_true", help="Parse, chunk, and validate without writing.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    project_dir = Path(args.project_dir).resolve()
    output_path = Path(args.output)
    if not output_path.is_absolute():
        output_path = project_dir / output_path

    cards = build_cards(project_dir, args.max_chunk_chars, args.overlap_chars)
    validate_cards(project_dir, cards)

    if not args.validate_only:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", encoding="utf-8", newline="\n") as handle:
            for card in cards:
                handle.write(json.dumps(card, ensure_ascii=False, separators=(",", ":")))
                handle.write("\n")

    suffix = " (validate-only; no file written)" if args.validate_only else ""
    print(f"Wrote {len(cards)} cards to {output_path}{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
