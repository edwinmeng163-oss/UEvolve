# Unreal MCP Pilot Install

## Full Experience Zip (Windows UE 5.6.1)

### English

Use this mode for `UnrealMcp-v0.12.0-pilot-full-win-ue561.zip`.
Extract the full-experience zip into your Unreal project root, next to `<YourProject>.uproject`; do not extract it under `Plugins/`.
It creates `Plugins/UnrealMcp/`, project-level `Tools/`, `Docs/`, optional `Schemas/`, root `INSTALL.md`, and root `README-FULL.md`.
The bundled Win64 plugin binary is for Epic Launcher Unreal Engine 5.6.1 only. Engine source builds or other patch versions need their own build.
For the 5-step first launch flow, read `Docs/FIRST_LAUNCH.md` after extraction.

### 中文

此模式适用于 `UnrealMcp-v0.12.0-pilot-full-win-ue561.zip`。
请把 full-experience zip 解压到 Unreal 项目根目录，也就是 `<YourProject>.uproject` 旁边；不要解压到 `Plugins/` 目录下。
它会创建 `Plugins/UnrealMcp/`、项目级 `Tools/`、`Docs/`、可选 `Schemas/`、根目录 `INSTALL.md` 和根目录 `README-FULL.md`。
包内 Win64 插件二进制只匹配 Epic Launcher 的 Unreal Engine 5.6.1。源码编译版引擎或其他补丁版本需要重新构建。
解压后，请阅读 `Docs/FIRST_LAUNCH.md` 中的 5 步首次启动流程。

### 日本語

この方式は `UnrealMcp-v0.12.0-pilot-full-win-ue561.zip` 用です。
full-experience zip は `<YourProject>.uproject` と同じ Unreal プロジェクトルートへ展開してください。`Plugins/` の下へは展開しません。
展開すると `Plugins/UnrealMcp/`、プロジェクトレベルの `Tools/`、`Docs/`、任意の `Schemas/`、ルートの `INSTALL.md`、ルートの `README-FULL.md` が作られます。
同梱の Win64 プラグインバイナリは Epic Launcher 版 Unreal Engine 5.6.1 専用です。ソースビルドのエンジンや別のパッチ版では、その環境でのビルドが必要です。
展開後、初回起動の 5 ステップは `Docs/FIRST_LAUNCH.md` を参照してください。

## English

### Prerequisites

- Unreal Editor 5.6.1 or 5.7.4 on macOS or Windows.
- macOS: Xcode 26.x, or another Xcode version compatible with your Unreal install.
- Windows: Visual Studio 2022 with the "Game Development with C++" workload, Windows 10/11 SDK, and .NET 6.0+ SDK.
- The built-in `PythonScriptPlugin` enabled in the project. Unreal Engine 5.x ships with this plugin.
- Close Unreal Editor before copying or replacing the plugin folder.

### Install Modes

This section is for the source-only `*-projectroot.zip` package. Source-only packages
are now project-root overlays: extract the zip into your Unreal project root,
next to `<YourProject>.uproject`; do not extract it under `Plugins/` or an
engine install. It creates:

```text
<UserProject>/Plugins/UnrealMcp/
<UserProject>/Tools/UnrealMcpPyTools/
<UserProject>/Tools/UnrealMcpToolScaffoldStarters/
<UserProject>/Tools/UnrealMcpToolScaffolds/
<UserProject>/Tools/UnrealMcpToolRegistry/
<UserProject>/Tools/UnrealMcpSkills/
<UserProject>/Tools/UnrealMcpKnowledge/
<UserProject>/Tools/UnrealMcpTests/
<UserProject>/Tools/UnrealMcpCodexBridge/
<UserProject>/Docs/FIRST_LAUNCH.md
```

This keeps Unreal MCP scoped to one project and ships the complete project-root
overlay that runtime tools expect: Python handlers, starter scaffold templates,
the writable registry and schema, skills, knowledge sources/evals, test fixtures,
Codex bridge source, and first-launch docs. Tools added through
`mcp_apply_scaffold` live in that project tree, which is easier to review, back
up, and remove. The source-only bridge tree intentionally excludes
`node_modules/` and `runtime/`; install Bun on the machine if you use the bridge.
The logical plugin, Tools, and Docs paths are the same on both OSes; the real
`<UserProject>` root differs by machine.

Engine plugin placement is advanced on both macOS and Windows and is not a direct
unzip target for this package:

```text
<UE Install>/Engine/Plugins/UnrealMcp/
```

If you manually copy the plugin there, also keep or copy the full project-root
`Tools/` overlay into each Unreal project root, because Python dispatch,
scaffold apply/import, skills, knowledge, tests, and bridge-source tools resolve
assets from `<ProjectDir>/Tools/...`.
Engine placement makes Unreal MCP available to every project using that engine
install. Self-extension writes to the engine plugin path, so it needs write
permission. Team-shared engine installs can also drift across projects when one
project adds tools. The logical engine plugin path is the same on both OSes; the
real `<UE Install>` root is under the platform-specific Unreal install directory.

If you build from the command line before first launch, UBT uses different entry scripts per OS.

macOS:

```bash
"<UE Install>/Engine/Build/BatchFiles/Mac/Build.sh" \
  YourProjectEditor Mac Development \
  -Project="/path/to/UserProject/UserProject.uproject" \
  -WaitMutex
```

Windows PowerShell:

```powershell
& "<UE Install>\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development `
  "-Project=C:\Path\To\UserProject\UserProject.uproject" `
  -WaitMutex
```

### First Launch

On first editor launch, Unreal Build Tool compiles the plugin against your local Unreal binary. This can take about 30 seconds to 15 minutes depending on your build cache and machine.

### Source-Only Drop-in Notes

- Workbench `Run Core Tests`, `Run RAG Evals`, `unreal.knowledge_search`, `unreal.knowledge_eval_run`, and `unreal.mcp_run_test_suite` use the included `Tools/UnrealMcpTests/` and `Tools/UnrealMcpKnowledge/` trees.
- `unreal.mcp_apply_scaffold` and `unreal.tools.import_package` use the included writable `Tools/UnrealMcpToolRegistry/tools.json` and `schema.json` at the project root. Keep this registry with the rest of the `Tools/` overlay.
- `Tools/UnrealMcpCodexBridge/` is source-only in this package. It does not include `node_modules/` or a bundled runtime; install Bun on macOS/Windows yourself before using bridge scripts.
- Cross-developer tool transfer uses `unreal.tools.export_package` to create `Saved/UnrealMcp/Packages/*.zip`. Do not commit scaffold drafts as the transfer format.

### Not In This Pilot

Windows packaging via `Tools/package_plugin.ps1` is verified on UE 5.7.4 / Win64 (Stage 2 end-to-end: build PASS, headless smoke PASS, `tools/list` count = 111). UE 5.8 support comes after upstream Epic 5.8 transport stabilises. Linux is untested.

### Help

For deeper documentation, read `Docs/Release-2026-05.md` and `Plugins/UnrealMcp/README.md` in the repository. Report bugs at `https://github.com/edwinmeng163-oss/UEvolve/issues`.

### Verify The Package

Run the matching check next to the zip and sidecar file.

Mac package:

```bash
shasum -a 256 -c UnrealMcp-v0.12.0-pilot-mac-ue56-ue57-projectroot.zip.sha256
```

Windows preview package:

```powershell
Get-FileHash -Algorithm SHA256 UnrealMcp-v<version>-win-ue56-ue57-projectroot.zip
Get-Content UnrealMcp-v<version>-win-ue56-ue57-projectroot.zip.sha256
```

Compare the `Get-FileHash` value with the hash in the `.sha256` sidecar.

## 中文

### 前提条件

- macOS 或 Windows 上的 Unreal Editor 5.6.1 或 5.7.4。
- macOS：Xcode 26.x，或与你的 Unreal 安装兼容的其他 Xcode 版本。
- Windows：Visual Studio 2022，并安装 "Game Development with C++" 工作负载、Windows 10/11 SDK 和 .NET 6.0+ SDK。
- 项目中启用内置的 `PythonScriptPlugin`。Unreal Engine 5.x 自带这个插件。
- 复制或替换插件文件夹前，请先关闭 Unreal Editor。

### 安装模式

本节适用于 source-only 的 `*-projectroot.zip` 包。source-only 包现在也是项目根目录 overlay：
请解压到 Unreal 项目根目录，也就是 `<YourProject>.uproject` 旁边；不要解压到 `Plugins/` 或引擎安装目录下。它会创建：

```text
<UserProject>/Plugins/UnrealMcp/
<UserProject>/Tools/UnrealMcpPyTools/
<UserProject>/Tools/UnrealMcpToolScaffoldStarters/
<UserProject>/Tools/UnrealMcpToolScaffolds/
<UserProject>/Tools/UnrealMcpToolRegistry/
<UserProject>/Tools/UnrealMcpSkills/
<UserProject>/Tools/UnrealMcpKnowledge/
<UserProject>/Tools/UnrealMcpTests/
<UserProject>/Tools/UnrealMcpCodexBridge/
<UserProject>/Docs/FIRST_LAUNCH.md
```

这会把 Unreal MCP 限定在单个项目内，并随包提供运行时工具期望的完整项目根目录
overlay：Python handlers、scaffold starter 模板、可写 registry 和 schema、skills、
knowledge sources/evals、test fixtures、Codex bridge 源码以及首次启动文档。通过
`mcp_apply_scaffold` 添加的工具也会写入该项目目录，便于审查、备份和移除。
source-only bridge 目录会刻意排除 `node_modules/` 和 `runtime/`；如果要使用 bridge，
请在对应机器上自行安装 Bun。两个 OS 使用相同的逻辑插件、Tools 和 Docs 路径；实际的
`<UserProject>` 根目录取决于具体机器。

macOS 和 Windows 的引擎级插件位置都属于高级用法，而且这个包不能直接解压到该位置：

```text
<UE Install>/Engine/Plugins/UnrealMcp/
```

如果你手动把插件复制到这里，仍需要在每个 Unreal 项目根目录保留或复制完整的项目根目录
`Tools/` overlay，因为 Python dispatch、scaffold apply/import、skills、knowledge、
tests 和 bridge-source 工具都会从 `<ProjectDir>/Tools/...` 解析资源。
引擎级安装会让使用该引擎安装的所有项目都能使用 Unreal MCP。自扩展会写入引擎插件路径，因此需要写入权限。团队共享的引擎安装也可能因为某个项目添加工具而导致跨项目漂移。两个 OS 使用相同的逻辑引擎插件路径；实际的 `<UE Install>` 根目录位于对应平台的 Unreal 安装目录下。

如果你在首次启动前从命令行构建，UBT 在不同 OS 上使用不同入口脚本。

macOS：

```bash
"<UE Install>/Engine/Build/BatchFiles/Mac/Build.sh" \
  YourProjectEditor Mac Development \
  -Project="/path/to/UserProject/UserProject.uproject" \
  -WaitMutex
```

Windows PowerShell：

```powershell
& "<UE Install>\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development `
  "-Project=C:\Path\To\UserProject\UserProject.uproject" `
  -WaitMutex
```

### 首次启动

首次启动编辑器时，Unreal Build Tool 会针对你的本地 Unreal 二进制编译插件。根据构建缓存和机器性能，这通常需要约 30 秒到 15 分钟。

### Source-Only Drop-in 说明

- Workbench 的 `Run Core Tests`、`Run RAG Evals`、`unreal.knowledge_search`、`unreal.knowledge_eval_run` 和 `unreal.mcp_run_test_suite` 会使用包内的 `Tools/UnrealMcpTests/` 与 `Tools/UnrealMcpKnowledge/` 目录。
- `unreal.mcp_apply_scaffold` 和 `unreal.tools.import_package` 会使用项目根目录下随包提供的可写 `Tools/UnrealMcpToolRegistry/tools.json` 与 `schema.json`。请让这个 registry 跟随其余 `Tools/` overlay 一起保留。
- 此包中的 `Tools/UnrealMcpCodexBridge/` 只包含源码，不包含 `node_modules/` 或捆绑 runtime；使用 bridge 脚本前请在 macOS/Windows 上自行安装 Bun。
- 跨开发者工具转移使用 `unreal.tools.export_package` 生成 `Saved/UnrealMcp/Packages/*.zip`。不要把 scaffold 草稿提交为转移格式。

### 本次 Pilot 不包含

Windows 打包通过 `Tools/package_plugin.ps1` 已在 UE 5.7.4 / Win64 上验证（Stage 2 端到端：build PASS、headless smoke PASS、`tools/list` count = 111）。UE 5.8 支持会在上游 Epic 5.8 transport 稳定后提供。Linux 尚未测试。

### 获取帮助

更深入的文档请阅读仓库中的 `Docs/Release-2026-05.md` 和 `Plugins/UnrealMcp/README.md`。Bug 请在 `https://github.com/edwinmeng163-oss/UEvolve/issues` 报告。

### 验证包

在 zip 和 sidecar 文件旁运行对应的检查。

Mac 包：

```bash
shasum -a 256 -c UnrealMcp-v0.12.0-pilot-mac-ue56-ue57-projectroot.zip.sha256
```

Windows 预览包：

```powershell
Get-FileHash -Algorithm SHA256 UnrealMcp-v<version>-win-ue56-ue57-projectroot.zip
Get-Content UnrealMcp-v<version>-win-ue56-ue57-projectroot.zip.sha256
```

将 `Get-FileHash` 的值与 `.sha256` sidecar 中的 hash 对比。

## 日本語

### 前提条件

- macOS または Windows 上の Unreal Editor 5.6.1 または 5.7.4。
- macOS: Xcode 26.x、または利用中の Unreal インストールと互換性のある Xcode。
- Windows: Visual Studio 2022 に "Game Development with C++" ワークロード、Windows 10/11 SDK、.NET 6.0+ SDK をインストールしてください。
- プロジェクトで組み込みの `PythonScriptPlugin` を有効にしてください。Unreal Engine 5.x にはこのプラグインが同梱されています。
- プラグインフォルダをコピーまたは置き換える前に Unreal Editor を閉じてください。

### インストール方式

このセクションは source-only の `*-projectroot.zip` パッケージ向けです。
source-only パッケージもプロジェクトルート overlay になりました。zip は
`<YourProject>.uproject` と同じ Unreal プロジェクトルートへ展開してください。`Plugins/` やエンジンインストール配下へは展開しないでください。展開すると次が作られます。

```text
<UserProject>/Plugins/UnrealMcp/
<UserProject>/Tools/UnrealMcpPyTools/
<UserProject>/Tools/UnrealMcpToolScaffoldStarters/
<UserProject>/Tools/UnrealMcpToolScaffolds/
<UserProject>/Tools/UnrealMcpToolRegistry/
<UserProject>/Tools/UnrealMcpSkills/
<UserProject>/Tools/UnrealMcpKnowledge/
<UserProject>/Tools/UnrealMcpTests/
<UserProject>/Tools/UnrealMcpCodexBridge/
<UserProject>/Docs/FIRST_LAUNCH.md
```

この方式では Unreal MCP が 1 つのプロジェクトに限定され、runtime tools が期待する完全なプロジェクトルート
overlay も同梱されます。内容は Python handlers、scaffold starter テンプレート、書き込み可能な registry と
schema、skills、knowledge sources/evals、test fixtures、Codex bridge ソース、初回起動ドキュメントです。
`mcp_apply_scaffold` で追加したツールもそのプロジェクトツリー内に置かれるため、レビュー、バックアップ、削除がしやすくなります。
source-only bridge ツリーでは意図的に `node_modules/` と `runtime/` を除外しています。bridge を使う場合は、対象マシンで Bun を別途インストールしてください。
論理的なプラグイン、Tools、Docs のパスは両 OS で同じで、実際の `<UserProject>` ルートはマシンごとに異なります。

macOS と Windows のどちらでも、エンジンプラグインとしての配置は上級者向けで、このパッケージの直接の展開先ではありません。

```text
<UE Install>/Engine/Plugins/UnrealMcp/
```

手動でプラグインをここへコピーする場合も、各 Unreal プロジェクトルートに完全なプロジェクトルート
`Tools/` overlay を残すかコピーしてください。Python dispatch、scaffold apply/import、skills、knowledge、
tests、bridge-source tools は `<ProjectDir>/Tools/...` からリソースを解決します。
エンジン配置では、そのエンジンインストールを使うすべてのプロジェクトで Unreal MCP を利用できます。自拡張はエンジンプラグインのパスへ書き込むため、書き込み権限が必要です。チーム共有のエンジンインストールでは、あるプロジェクトで追加したツールが他のプロジェクトにも影響し、差分が広がることがあります。論理的なエンジンプラグインパスは両 OS で同じで、実際の `<UE Install>` ルートは各プラットフォーム固有の Unreal インストールディレクトリ配下です。

初回起動前にコマンドラインでビルドする場合、UBT は OS ごとに異なる入口スクリプトを使います。

macOS:

```bash
"<UE Install>/Engine/Build/BatchFiles/Mac/Build.sh" \
  YourProjectEditor Mac Development \
  -Project="/path/to/UserProject/UserProject.uproject" \
  -WaitMutex
```

Windows PowerShell:

```powershell
& "<UE Install>\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development `
  "-Project=C:\Path\To\UserProject\UserProject.uproject" `
  -WaitMutex
```

### 初回起動

初回のエディタ起動時に、Unreal Build Tool がローカルの Unreal バイナリに合わせてプラグインをコンパイルします。ビルドキャッシュとマシン性能により、約 30 秒から 15 分かかります。

### Source-Only Drop-in の注意

- Workbench の `Run Core Tests`、`Run RAG Evals`、`unreal.knowledge_search`、`unreal.knowledge_eval_run`、`unreal.mcp_run_test_suite` は、同梱の `Tools/UnrealMcpTests/` と `Tools/UnrealMcpKnowledge/` ツリーを使用します。
- `unreal.mcp_apply_scaffold` と `unreal.tools.import_package` は、プロジェクトルートに同梱された書き込み可能な `Tools/UnrealMcpToolRegistry/tools.json` と `schema.json` を使用します。この registry は他の `Tools/` overlay と一緒に保持してください。
- このパッケージの `Tools/UnrealMcpCodexBridge/` は source-only です。`node_modules/` や同梱 runtime は含まれません。bridge scripts を使う前に macOS/Windows で Bun を別途インストールしてください。
- 開発者間のツール共有は、`unreal.tools.export_package` で `Saved/UnrealMcp/Packages/*.zip` を作成して行います。scaffold ドラフトを転送形式としてコミットしないでください。

### この Pilot に含まれないもの

Windows パッケージングは `Tools/package_plugin.ps1` により UE 5.7.4 / Win64 で検証済みです（Stage 2 end-to-end: build PASS、headless smoke PASS、`tools/list` count = 111）。UE 5.8 サポートは upstream Epic 5.8 transport の安定後に対応します。Linux は未検証です。

### ヘルプ

詳しいドキュメントは、リポジトリ内の `Docs/Release-2026-05.md` と `Plugins/UnrealMcp/README.md` を参照してください。バグは `https://github.com/edwinmeng163-oss/UEvolve/issues` で報告してください。

### パッケージの検証

zip と sidecar ファイルの横で、該当する確認を実行してください。

Mac パッケージ:

```bash
shasum -a 256 -c UnrealMcp-v0.12.0-pilot-mac-ue56-ue57-projectroot.zip.sha256
```

Windows プレビューパッケージ:

```powershell
Get-FileHash -Algorithm SHA256 UnrealMcp-v<version>-win-ue56-ue57-projectroot.zip
Get-Content UnrealMcp-v<version>-win-ue56-ue57-projectroot.zip.sha256
```

`Get-FileHash` の値を `.sha256` sidecar 内の hash と比較してください。
