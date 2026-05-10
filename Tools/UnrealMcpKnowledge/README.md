# Unreal MCP Knowledge Sources

This folder stores versioned source manifests for UEvolve's local knowledge/RAG
bootstrap. It should contain source lists, schemas, and small metadata files, not
downloaded third-party documentation payloads.

The generated local index writes `KnowledgeCard` JSONL records under
`Saved/UnrealMcp/KnowledgeIndex`. The versioned card schema lives at:

```text
Schemas/UnrealMcpKnowledgeCard.schema.json
```

`cards.jsonl` is written as UTF-8 JSONL so external scripts, CI checks, and
package validators can inspect the same index that the plugin reads.

Current retrieval is intentionally local-first and lexical: section-aware chunks,
Chinese/English synonym expansion, source weighting, confidence weighting, and
duplicate source-section suppression. Embeddings can be added later as an
optional backend, but the baseline should keep working offline.

## Evals

Versioned RAG regression cases live under:

```text
Tools/UnrealMcpKnowledge/Evals
```

Run them from Chat or Workbench with:

```text
/tool unreal.knowledge_eval_run {"evalPath":"Tools/UnrealMcpKnowledge/Evals","includeDetails":false}
```

The eval runner covers `knowledge_search`, `tool_recommend`,
`tool_gap_analyze`, and `workflow_recommend` so retrieval quality can be checked
after changing sources, scoring, synonyms, or ToolRegistry metadata.

High-value local RAG pages include deployment troubleshooting and Unreal task
recipes for first-person characters, Widget HUDs, Blueprint graph edits, and
self-extension tool creation.

## Official Unreal Engine Docs

The first curated seed list is:

```text
Tools/UnrealMcpKnowledge/Sources/unreal_engine_official_docs_5_7.json
```

Fetch the seed pages into a local ignored cache:

```bash
python3 Tools/unreal_mcp_fetch_docs.py --max-pages 20
```

Output defaults to:

```text
Saved/UnrealMcp/KnowledgeSources/UnrealEngineOfficialDocs/5.7
```

The downloader prefers Epic's structured documentation JSON endpoint for normal
documentation pages and falls back to static HTML for pages such as the Unreal
Python API. Low extracted text counts are flagged in the generated manifest so
the indexer can skip or deprioritize weak pages.

Do not commit fetched official documentation content unless the upstream license
explicitly allows redistribution. Commit only source manifests and downloader
code.
