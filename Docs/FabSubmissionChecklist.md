# Fab Submission Checklist

This document is the pre-submission readiness checklist for publishing UEvolve on Fab (formerly Epic Marketplace). It maps each Fab review item to the file or process in this repository that supplies the evidence. Walk it top to bottom before opening a Fab submission.

## 1. Pre-submission overview

- Submission portal: https://fab.com, Epic's marketplace that replaced Unreal Engine Marketplace, Sketchfab, Quixel Bridge, and ArtStation Marketplace in late 2024.
- Submission category: Code Plugins, because UEvolve is a code-bearing Unreal Engine editor plugin.
- Required developer account: Epic Games account with completed Fab seller onboarding.
- Seller tax setup: W-9 or W-8BEN tax form must be on file before paid or free listing publication.
- Review SLA: first review is typically 5-10 business days.
- Re-submission SLA after reviewer feedback: typically 3-5 business days.
- Current release basis: v0.15.1 pilot release, with Mac and Windows UE 5.6 / 5.7 verification.
- Submission intent: not yet ready to submit; this file is a readiness checklist and evidence map.

## 2. Required plugin metadata

Fab reviewers inspect the plugin descriptor first. The source of truth is `Plugins/UnrealMcp/UnrealMcp.uplugin`.

| Field | Current value (Plugins/UnrealMcp/UnrealMcp.uplugin) | Fab requirement | Status |
|---|---|---|---|
| FriendlyName | "Unreal MCP" | Less than 50 characters, no profanity, distinct from existing Fab listings | OK |
| Description | "Runs an MCP server inside the Unreal Editor, exposes editor automation, batch editing, Python scripting, debugging, and AI-assisted tool use over localhost, and includes an in-editor copilot-style command and chat window." | About 120-300 characters; clear value proposition in first sentence | Review for marketing copy |
| Category | "AI" | Must match Fab category taxonomy such as AI, Animation, Audio, Blueprint, Code Plugins, Editor Modes, Optimization, Performance | OK |
| CreatedBy | "OpenAI Codex" | Should match Fab seller display name, usually company or studio | Gap: needs human or studio name |
| IsBetaVersion | true | If true, Fab listing will show Beta badge | OK, intentional |
| Modules[*].Type | "Editor" | Editor-only plugins go in the editor plugin review path | OK |
| Plugins[*] | depends on PythonScriptPlugin | Required dependencies must be disclosed in listing and descriptor | Gap: not currently mentioned in description |

Recommended polished descriptor description:

"Adds an MCP (Model Context Protocol) server inside Unreal Editor, plus Chat and Workbench panels so AI assistants can drive editor automation, batch editing, Python scripting, and audited tool authoring over localhost. Requires PythonScriptPlugin."

Before submission, update the `.uplugin` description only after the PM approves the exact public-facing copy.

## 3. Required assets (manual handoff)

- Icon: Fab requires a 256x256 PNG. Current repo evidence: no submission-ready icon has been verified. Gap.
- Recommended icon path: `Plugins/UnrealMcp/Resources/Icon128.png`, using a 256x256 source PNG even if the filename remains Unreal's conventional icon name.
- Featured screenshot: Fab expects a 1920x1080 PNG. Recommended hero shot: Workbench tab and Chat panel side by side in Unreal Editor. Gap.
- Additional screenshots: provide 3-5 more 1920x1080 PNGs.
- Additional screenshot 1: `Window > Unreal MCP Chat` with a multi-turn conversation.
- Additional screenshot 2: `Window > Unreal MCP Workbench` showing pipeline status and the install doctor card after v0.15.2.
- Additional screenshot 3: self-extension flow showing scaffold, validate, apply, build, and test.
- Additional screenshot 4: tool list response from a downstream MCP client, with 140+ tools visible.
- Additional screenshot 5: optional Codex Desktop bridge running side by side with the editor.
- Sizzle or demo video: optional but recommended for conversion.
- Recommended video length: 30-90 seconds.
- Recommended video format: 1920x1080 MP4 or YouTube link.
- Suggested video storyline: spawn 50 actors in a circle from Chat, generate a new MCP tool from a recipe, then apply, build, test, and roll back in one flow.
- Asset handoff owner: PM or release owner.
- Storage recommendation before submission: keep final source assets under a release staging folder outside runtime `Saved/` state, then copy only Fab-required deliverables into the submission package.

## 4. Pricing and licensing

- Recommended pricing tier: free until v1.0 GA.
- Rationale: UEvolve is a beta plugin, active development is ongoing, and the current value is workflow proof plus early adopter feedback rather than a polished commercial product.
- Pricing decision owner: PM.
- Current license evidence: root `LICENSE` exists in this checkout.
- Pre-submission license action: confirm the root `LICENSE` is the intended public license for Fab distribution.
- If license terms are not final: choose and document MIT, Apache 2.0, or a commercial dual-license model before submission.
- Per Fab terms, code plugins distributed for free can still be commercially licensed if the seller clearly documents the license.
- Listing text should state the license plainly and avoid implying rights beyond the checked-in license.
- If dual licensing is chosen, add a short `Docs/` note or listing paragraph explaining the public license and commercial support boundary.

## 5. Tags

Recommended Fab tags, with a maximum of 10:

- AI Assistant
- Editor Extension
- Python
- MCP
- Code Plugin
- Workflow
- Automation
- Self-Extension
- Chat
- Cross-Platform

## 6. Engine compatibility range

- Descriptor evidence: `Plugins/UnrealMcp/UnrealMcp.uplugin`.
- Fab compatibility evidence: explicit engine-version dropdown choices in the Fab submission portal.
- Current verified range: UE 5.6.1 and UE 5.7.4.
- Current verified platforms for that range: macOS and Windows.
- Minimum release claim: UE 5.6.
- Maximum release claim for v0.15.1: UE 5.7.
- UE 5.8 readiness: not verified.
- Fab listing requirement: explicitly say UE 5.8 is not verified unless a separate forward-compatibility probe passes.
- Pre-submission source compatibility command:

```sh
python3 Tools/check_ue56_compat.py
```

- Required result: 0 errors and 0 warnings.
- Build verification requirement: fresh build on every claimed engine version on every claimed platform.
- Do not widen the Fab engine range based only on source review.
- If a platform cannot be verified, declare it as unsupported or unverified in the listing.

## 7. Test report

Fab does not require third-party test reports for code plugins, but including a concise test summary in the listing description should improve buyer confidence.

- Test fixture reference: `Tools/UnrealMcpTests/`.
- Current fixture scope: about 70 JSON fixtures across Core, Blueprint, SelfExtension, Widget, and related categories.
- C++ automation test reference: `Plugins/UnrealMcp/Source/UnrealMcp/Private/Tests/UnrealMcpInstallDoctorTests.cpp`.
- C++ path resolver test reference: `Plugins/UnrealMcp/Source/UnrealMcp/Private/Tests/UnrealMcpSharedPathResolverTests.cpp`.
- Package integrity reference: `Tools/verify_package_integrity.py --strict`.
- Current release evidence: package integrity was reported PASS as of v0.15.1.
- Submission recommendation: prepare a short listing paragraph that says UEvolve is covered by registry validation, package integrity checks, JSON MCP smoke fixtures, and Unreal automation tests for install doctor and shared path resolution.
- Keep the test summary factual; do not imply Epic has certified the plugin before Fab review completes.

## 8. PII / secret scrub

Run this checklist on a fresh worktree or extracted submission zip before upload:

```sh
# In a fresh worktree (or extracted zip):
grep -rE "(sk-[A-Za-z0-9]{32,}|AIza[0-9A-Za-z_-]{35}|ghp_[A-Za-z0-9]{36})" . --exclude-dir=.git
grep -rEi "(api[_-]?key|secret[_-]?key|access[_-]?token)\s*[:=]\s*['\"][^'\"]+['\"]" . --exclude-dir=.git
grep -rE "[a-zA-Z0-9._-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}" Plugins/ Tools/ Docs/  # personal email
```

Expected result: 0 hits except explicitly whitelisted non-shipped references.

Known whitelist guidance:

- Git commit author trailers are not shipped if `.git` is excluded.
- Public documentation URLs are acceptable.
- Personal emails in shipped source, docs, configs, or test fixtures require PM approval.
- API keys, access tokens, OAuth secrets, and provider keys must never ship.
- Runtime files under `Saved/UnrealMcp` must not be part of the Fab package.

If anything unexpected surfaces, redact it before packaging and rerun the grep checks from scratch.

## 9. Support contact

- Public support email: TBD.
- Gap: PM must register a `support@uevolve.*` address or designate a personal email on file.
- Issue tracker: https://github.com/edwinmeng163-oss/UEvolve/issues.
- Listing action: link the issue tracker in the Fab support section.
- Update cadence: declare an explicit beta cadence, such as monthly patches and quarterly major versions.
- Recommended beta triage target: respond to GitHub issues within 72 hours.
- Recommended post-GA triage target: respond to GitHub issues within 48 hours.
- Support boundary: make clear that UEvolve automates Unreal Editor locally and does not provide general Unreal project consulting.

## 10. Self-extension disclosure (UEvolve-specific)

UEvolve's self-extension pipeline is unusual for a Fab plugin and must be disclosed clearly.

- The plugin includes tools that can write to its own C++ source through the `apply_scaffold` family.
- Apply paths are bounded to the plugin-source domain by the path-resolution policy.
- Policy references: `Docs/Architecture.md` and `AGENTS.md`.
- Apply requires an explicit extension lock through `lock_extension_session`.
- Normal apply flow is followed by `mcp_build_editor` and test execution.
- Recovery path is manifest-backed rollback through `rollback_to_manifest`.
- This should be framed as an audited tool-authoring feature, not as a hidden self-modifying behavior.
- Listing language should explain that source-writing tools are local, explicit, bounded, and intended for advanced editor automation authors.
- Buyer expectation: users should close Unreal Editor or disable Live Coding before build steps when source has changed.

## 11. Network requirements disclosure

- Primary listener: `127.0.0.1:8765` for MCP streamable HTTP.
- Optional bridge listener: `127.0.0.1:8766` for the Codex Desktop bridge WebSocket.
- Both listeners bind to loopback only.
- No inbound connections from non-localhost are expected or required.
- The plugin itself should not make outbound network calls for its core MCP server behavior.
- Optional bridge behavior: the bridge speaks stdio or Unix socket to Codex Desktop / Codex App Server.
- Codex Desktop or other external AI clients make their own outbound calls under their own consent, account, and network rules.
- Listing language should distinguish UEvolve's local listener from the network behavior of whichever AI assistant connects to it.
- Firewall note: users may see local-loopback prompts depending on platform policy, endpoint security tooling, or corporate device management.

## 12. Cross-platform support disclosure

- macOS support: arm64 and x86_64, UE 5.6 / 5.7.
- macOS status: verified for the pilot release.
- Windows support: x64, UE 5.6 / 5.7.
- Windows status: verified after the Tier 1-5 compatibility fixes.
- Windows evidence reference: `Docs/WindowsCompatibilityLessons.md`.
- Linux support: not verified.
- Fab listing requirement: explicitly declare Linux as unverified unless a Linux build and smoke pass have been completed.
- Do not imply Linux support by saying cross-platform without naming the verified platforms.
- Recommended listing phrasing: "Verified on macOS and Windows for Unreal Engine 5.6 and 5.7; Linux is not yet verified."

## 13. Pre-launch verification ritual

Before zipping for Fab submission, run all steps from a clean staging area:

```sh
# 1. Fresh clone into a scratch dir
git clone https://github.com/edwinmeng163-oss/UEvolve.git uevolve-fab-submission
cd uevolve-fab-submission
git checkout v0.15.1   # or whichever tag is being submitted

# 2. Source-level validators
python3 Tools/validate_tool_registry.py
python3 Tools/check_ue56_compat.py
python3 Tools/verify_package_integrity.py --root . --mode source --repo-root . --strict

# 3. Build verify on each engine version on each platform
"/Users/Shared/Epic Games/UE_5.6/Engine/Build/BatchFiles/Mac/Build.sh" MyProjectEditor Mac Development -project=Examples/UEvolveExample/UEvolveExample.uproject -WaitMutex
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" MyProjectEditor Mac Development -project=Examples/UEvolveExample57/UEvolveExample57.uproject -WaitMutex
# Then equivalent on Windows.

# 4. Zip-level verifier on the produced submission zip
bash Tools/package_plugin.sh --version 0.15.1 --output /tmp/uevolve-fab-stage
python3 Tools/verify_package_integrity.py --zip /tmp/uevolve-fab-stage/*.zip --mode source --strict
```

All four stages must succeed before opening a Fab submission.

Failure handling:

- If registry validation fails, fix metadata or mirror parity before packaging.
- If UE 5.6 compatibility fails, do not claim UE 5.6 support.
- If package integrity fails, treat the package as non-shippable.
- If any build fails on a claimed engine or platform, narrow the listed compatibility range or fix the build.
- If smoke tests expose stale plugin binaries, clean plugin-level `Binaries/` and `Intermediate/`, rebuild, and rerun the smoke.

## 14. Submission package layout

Fab expects code plugins as a zip archive.

- Expected zip root: a single `<PluginName>/` directory.
- Expected plugin contents: `.uplugin`, `Source/`, `Resources/`, `Config/` if present, and `Content/` if present.
- Usually excluded: `Saved/`, `Intermediate/`, `.git`, `.gitignore`, `.vscode/`, `.idea/`, `node_modules/`, and IDE workspace files.
- Usually excluded for source submissions: `Binaries/`.
- Exception: include `Binaries/` only if Fab explicitly requires a binary redistribution package for the submission type.
- Repo verifier evidence: `Tools/verify_package_integrity.py`.
- Verifier invariant: `excluded_paths_absent` enforces most package-exclusion rules.
- UEvolve-specific issue: the current package layout is project-root, not plugin-root.
- Current project-root package includes shared `Tools/`, `Docs/`, `Schemas/`, and `Plugins/UnrealMcp/`.
- Fab follow-up requirement: build a plugin-only repackage path.
- Do not upload the current project-root zip to Fab without confirming Fab accepts the extra top-level support directories.

## 15. Post-submission maintenance plan

- Patch releases: cherry-pick fixes to a `release/v0.15.x` branch.
- Patch versioning: retag as `v0.15.x+1` or the agreed release tag style.
- Fab update flow: submit the patch package as an update to the existing listing.
- Major releases: decide whether to replace the existing Fab listing or open a new listing only if the product identity materially changes.
- Compatibility maintenance: keep `Tools/check_ue56_compat.py` green while UE 5.6 remains supported.
- Minimum-engine changes: bump the listed minimum engine only when the compatibility contract is intentionally changed.
- Beta support SLA target: less than 72 hours to first response on GitHub issues.
- Post-GA support SLA target: less than 48 hours to first response on GitHub issues.
- Security fixes: publish as patch releases and call them out clearly in Fab release notes.
- Breaking changes: document in release notes, README, and the Fab changelog before upload.

## 16. Action items before opening a Fab submission

1. Confirm the root `LICENSE` is final for Fab distribution; if not, replace it with the chosen MIT, Apache 2.0, or commercial license terms.
2. Update `UnrealMcp.uplugin` `CreatedBy` to the human, studio, or seller display name.
3. Refine `UnrealMcp.uplugin` `Description` to about 200 characters and disclose the PythonScriptPlugin dependency.
4. Produce a 256x256 plugin icon PNG and stage it at `Plugins/UnrealMcp/Resources/Icon128.png`.
5. Produce a featured 1920x1080 screenshot showing Workbench and Chat together.
6. Produce 3-5 additional 1920x1080 screenshots following the list in section 3.
7. Decide pricing tier, likely free until v1.0 GA, and record the decision in this checklist or the release notes.
8. Register a support email or assign an approved personal mailbox.
9. Build a plugin-only repackage script, likely as a variant of `Tools/package_plugin.sh` that zips only `Plugins/UnrealMcp/`.
10. Linux verification: at minimum compile the UE 5.7 editor target on Linux and run the install doctor smoke, then declare the result in the listing.
11. UE 5.8 forward-compatibility probe: run when UE 5.8 is GA before widening the engine range.
12. Produce a 30-90 second sizzle video; optional, but recommended.

## 17. Cross-references

- `Tools/PackagingResources/INSTALL.md` - end-user install handoff text currently present in this checkout.
- `Docs/FIRST_LAUNCH.md` - end-user first-launch experience.
- `Docs/PackagingIntegrity.md` - verifier invariant catalog added for v0.15.1.
- `Docs/WindowsCompatibilityLessons.md` - Windows-specific traps from the v0.15.0 sweep.
- `AGENTS.md` - full project briefing for developer-facing continuity.
- `README.md` - repository overview for developer-facing orientation.

## 18. Revision history

| Date | Version | Author | Notes |
|---|---|---|---|
| 2026-05-18 | initial | UEvolve PM | Created during v0.15.1 C5 Phase 2 wrap |
