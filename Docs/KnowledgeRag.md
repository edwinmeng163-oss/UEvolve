# UEvolve Knowledge And RAG Plan

UEvolve's self-extension loop needs more than a larger prompt. The assistant has
to know which tools exist, when to compose them, when a new tool is justified,
and which failure patterns have already been solved. This document defines the
local-first knowledge layer that should support that behavior.

## Goals

- Help Chat choose existing tools before inventing new ones.
- Recommend safe tool combinations and verification steps for open-ended tasks.
- Detect when a request is better solved by a high-level workflow or by a new MCP
  tool scaffold.
- Preserve useful project knowledge without depending on one long chat context.
- Keep private project activity local unless a user explicitly promotes it into
  versioned docs or skills.

## Initial Knowledge Sources

The authoritative source taxonomy, including the eventKind enum and capture priorities, lives in `Docs/KnowledgeRagSources.md`.
Versioned sources:

- `Tools/UnrealMcpToolRegistry/tools.json`: tool category, handler, risk, dry-run,
  preflight/postcheck, test coverage, owner, and docs path.
- C++ tool descriptors in `UnrealMcpToolRegistrar.cpp`: canonical AI-visible
  tool schemas and descriptions.
- `Tools/UnrealMcpTests/**`: examples of valid, invalid, happy-path, and
  sandboxed tool usage.
- `Docs/**`, `README.md`, and `Plugins/UnrealMcp/README.md`: installation,
  architecture, security, supervisor, pipeline, and troubleshooting docs.
- `Docs/DeploymentTroubleshooting.md` and `Docs/UnrealTaskRecipes.md`:
  practical install recovery, common editor tasks, and safe self-extension
  workflows.
- `Tools/UnrealMcpSkills/**`: reviewed reusable skills promoted by the user.

Local runtime sources:

- `Saved/UnrealMcp/ProjectMemory.json`: active task, decisions, and pipeline
  state.
- `Saved/UnrealMcp/ActivityLog/*.jsonl`: optional user-enabled activity records.
- `Saved/UnrealMcp/SkillDrafts/**`: unpromoted distilled skill drafts.
- `Saved/UnrealMcp/TestScaffolds/**`: generated tests and scaffold artifacts.
- `Saved/UnrealMcp/SupervisorLogs/**`: restart/build/test handoff evidence.

External reference sources to summarize into local docs instead of fetching every
turn:

- MCP specification: tools, resources, prompts, and schema expectations.
- Unreal Engine 5.7 docs: Blueprint, Widget Blueprint, Python API, C++ API,
  editor automation, build tooling, and plugin deployment.
- OpenAI function calling and structured output docs: schema compatibility,
  tool calling behavior, and error patterns.
- Tool-use research notes such as tool retrieval, tool recommendation, and
  workflow composition papers.

Official Unreal Engine docs bootstrap:

- Versioned seed manifest:
  `Tools/UnrealMcpKnowledge/Sources/unreal_engine_official_docs_5_7.json`.
- Fetcher:
  `Tools/unreal_mcp_fetch_docs.py`.
- Local cache:
  `Saved/UnrealMcp/KnowledgeSources/UnrealEngineOfficialDocs/5.7`.
- Normal documentation pages should prefer Epic's structured
  `community/api/documentation/document.json` endpoint. Static pages such as the
  Unreal Python API can be fetched as HTML.
- Downloaded official documentation payloads should stay local under `Saved/`
  and should not be redistributed in Git unless the upstream license explicitly
  permits it.

## Indexing Strategy

Start with deterministic local retrieval before embeddings:

1. Parse versioned docs, registry entries, test fixtures, skills, and selected
   local memory into normalized `KnowledgeCard` records. The versioned schema is
   `Schemas/UnrealMcpKnowledgeCard.schema.json`.
2. Store a generated index under `Saved/UnrealMcp/KnowledgeIndex/index.json`.
   The `cards.jsonl` companion file is written as UTF-8 JSONL so external
   scripts and package validators can inspect it without Unreal-specific text
   decoding.
3. Split markdown-like sources by section headings before chunking, so search
   can cite the relevant local section instead of a random character window.
4. Score search with token matching, Chinese/English synonym expansion,
   title/section/category boosts, source weights, confidence weights, and exact
   tool-name boosts.
5. De-duplicate near-identical cards and collapse repeated adjacent source
   sections in search results.
6. Return compact cards with source path, excerpt, category, risk metadata, and
   suggested next tool calls.

This avoids API cost, preserves privacy, and works offline. Embeddings can be
added later as an optional backend, but the baseline should not require them.

## Proposed Tools

`unreal.knowledge_index_refresh` implemented

- Rebuilds the local knowledge index from versioned docs plus optional local
  runtime sources.
- Default should be read-mostly with writes constrained to
  `Saved/UnrealMcp/KnowledgeIndex`.
- Should report source counts, skipped private sources, and index size.

`unreal.knowledge_search` implemented

- Searches the local index.
- Inputs: `query`, optional `categories`, `limit`, `maxExcerptChars`, and
  `includeText`.
- Outputs concise cards with source paths and suggested follow-up tools.

`unreal.tool_recommend` implemented

- Takes a natural-language task and recommends existing MCP tools, order,
  dry-run/preflight needs, backup needs, and verification tools.
- Should prefer composition before self-extension.

`unreal.tool_gap_analyze` implemented

- Decides whether a task should use existing tools, compose existing tools, or
  scaffold a new MCP tool.
- If a new tool is needed, returns descriptor hints, schema risks, test ideas,
  and the self-extension pipeline steps.

`unreal.workflow_recommend` implemented

- Converts a task plus retrieved cards into a bounded `unreal.workflow_run`
  draft.
- Should default to `dryRun:true` and include verification gates.

`unreal.knowledge_eval_run` implemented

- Runs versioned local eval cases from `Tools/UnrealMcpKnowledge/Evals`.
- Covers search, tool recommendation, gap analysis, and workflow recommendation
  regressions.
- Returns pass rate, failed cases, and optional structured per-case evidence.

## Chat Integration

Recommended turn flow for complex requests:

1. Chat builds a compact local RAG/tool-planning capsule before AI turns. It
   calls `unreal.tool_recommend`, refreshes the local index when missing, and
   injects only the top tools/cards/workflow gates into the model context.
2. `unreal.tool_recommend` checks whether existing tools or recipes cover it.
3. `unreal.knowledge_search` retrieves docs, tests, and failure patterns. If the
   index is missing, run `unreal.knowledge_index_refresh` and retry the search.
4. `unreal.preview_change_plan` turns the request into a structured plan using
   the recommendation/search evidence.
5. `unreal.tool_gap_analyze` decides whether existing tools, a workflow, or a
   new descriptor-first MCP tool is the right path.
6. If the plan is composable, generate a bounded draft with
   `unreal.workflow_recommend`, then run it through `unreal.workflow_run` only
   after exact arguments are filled and risk gates are clear.
7. If there is a true gap, use the self-extension pipeline:
   schema validation, dry-run apply, build, test suite, verification, and
   rollback/fix plan on failure.
8. Write `chat.active_task` when the task is long, paused, failed, or near tool
   round limits.

## Privacy And Safety

- Versioned docs and registry data are safe to index by default.
- Runtime memories, activity logs, chat history, drafts, and supervisor logs are
  local-only and should be opt-in for indexing.
- Search results should include source paths so users can inspect why a
  recommendation was made.
- High-risk recommendations must preserve ToolRegistry policy, dry-run support,
  lock requirements, and verification steps.

## Performance Notes

The baseline lexical index should be cheap:

- No per-minute embedding calls.
- No external network required.
- Rebuild on demand, after registry changes, or after skill promotion.
- Runtime sources can be capped by file count, event count, and total bytes.

Optional embeddings can be added later for better semantic search. If added,
they should support local model backends and explicit opt-in cloud embeddings.

## Milestones

1. Add `KnowledgeCard` JSON schema and local index writer. Done for local
   `cards.jsonl` plus `Schemas/UnrealMcpKnowledgeCard.schema.json`.
2. Implement `unreal.knowledge_index_refresh` and `unreal.knowledge_search`.
   Done.
3. Convert fetched docs manifests and `documents.jsonl` rows into
   `KnowledgeCard` records, skipping low-content pages by default.
   Done for `documents.jsonl`, versioned docs, and visible ToolRegistry cards.
4. Add tests for registry/doc search, missing index, and private-source opt-in.
   Partially done with versioned-doc index refresh, search, recommendation, and
   missing-required tests.
5. Implement `unreal.tool_recommend` using registry policy and search cards.
   Done.
6. Implement `unreal.tool_gap_analyze` and connect it to scaffold generation.
   Done for read-only gap decisions and scaffold hints.
7. Implement `unreal.workflow_recommend` and allow dry-run execution through
   `unreal.workflow_run`. Done for safe draft generation with skipped
   placeholder task-specific steps.
8. Add Workbench buttons for Refresh Knowledge, Search Knowledge, Recommend
   Tools, and Run RAG Evals while keeping the UI thin. Done.
