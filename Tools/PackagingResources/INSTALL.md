# Unreal MCP Pilot Install

## English

### Prerequisites

- Unreal Editor 5.6.1 or 5.7.4 on macOS.
- Xcode 26.x, or another Xcode version compatible with your Unreal install.
- The built-in `PythonScriptPlugin` enabled in the project. Unreal Engine 5.x ships with this plugin.
- Close Unreal Editor before copying or replacing the plugin folder.

### Install Modes

Project plugin placement is recommended:

```text
<UserProject>/Plugins/UnrealMcp/
```

This keeps Unreal MCP scoped to one project. Tools added through `mcp_apply_scaffold` live in that project tree, which is easier to review, back up, and remove.

Engine plugin placement is advanced:

```text
<UE Install>/Engine/Plugins/UnrealMcp/
```

This makes Unreal MCP available to every project using that engine install. Self-extension writes to the engine plugin path, so it needs write permission. Team-shared engine installs can also drift across projects when one project adds tools.

### First Launch

On first editor launch, Unreal Build Tool compiles the plugin against your local Unreal binary. This can take about 30 seconds to 15 minutes depending on your build cache and machine.

### Drop-in Limitations

- Workbench `Run Core Tests` and `Run RAG Evals` are disabled unless you copy the optional `Tools/UnrealMcpTests/` and `Tools/UnrealMcpKnowledge/Evals/` directories into your project. See `Docs/Release-2026-05.md` and `Plugins/UnrealMcp/README.md` in the full workbench repository for details.
- `unreal.tools.import_package` requires a writable `Tools/UnrealMcpToolRegistry/tools.json` at the project root. If it is missing, the tool returns a structured `REGISTRY_NOT_INITIALIZED` error with the setup recipe.
- Cross-developer tool transfer uses `unreal.tools.export_package` to create `Saved/UnrealMcp/Packages/*.zip`. Do not commit scaffold drafts as the transfer format.

### Not In This Pilot

Windows and Unreal Engine 5.8 support are coming as collaborators verify them. Linux is untested.

### Help

For deeper documentation, read `Docs/Release-2026-05.md` and `Plugins/UnrealMcp/README.md` in the repository. Report bugs at `https://github.com/edwinmeng163-oss/UEvolve/issues`.

### Verify The Package

Run this next to the zip and sidecar file:

```bash
shasum -a 256 -c UnrealMcp-v0.12.0-pilot-mac-ue56-ue57-source.zip.sha256
```

## 中文

### 前提条件

- macOS 上的 Unreal Editor 5.6.1 或 5.7.4。
- Xcode 26.x，或与你的 Unreal 安装兼容的其他 Xcode 版本。
- 项目中启用内置的 `PythonScriptPlugin`。Unreal Engine 5.x 自带这个插件。
- 复制或替换插件文件夹前，请先关闭 Unreal Editor。

### 安装模式

推荐使用项目级插件位置：

```text
<UserProject>/Plugins/UnrealMcp/
```

这会把 Unreal MCP 限定在单个项目内。通过 `mcp_apply_scaffold` 添加的工具会写入该项目目录，便于审查、备份和移除。

引擎级插件位置适合高级用法：

```text
<UE Install>/Engine/Plugins/UnrealMcp/
```

这会让使用该引擎安装的所有项目都能使用 Unreal MCP。自扩展会写入引擎插件路径，因此需要写入权限。团队共享的引擎安装也可能因为某个项目添加工具而导致跨项目漂移。

### 首次启动

首次启动编辑器时，Unreal Build Tool 会针对你的本地 Unreal 二进制编译插件。根据构建缓存和机器性能，这通常需要约 30 秒到 15 分钟。

### Drop-in 限制

- Workbench 的 `Run Core Tests` 和 `Run RAG Evals` 按钮会保持禁用，除非你把可选的 `Tools/UnrealMcpTests/` 和 `Tools/UnrealMcpKnowledge/Evals/` 目录复制到项目中。完整说明请查看完整 workbench 仓库里的 `Docs/Release-2026-05.md` 和 `Plugins/UnrealMcp/README.md`。
- `unreal.tools.import_package` 需要项目根目录下存在可写的 `Tools/UnrealMcpToolRegistry/tools.json`。如果缺失，工具会返回结构化的 `REGISTRY_NOT_INITIALIZED` 错误，并附带设置步骤。
- 跨开发者工具转移使用 `unreal.tools.export_package` 生成 `Saved/UnrealMcp/Packages/*.zip`。不要把 scaffold 草稿提交为转移格式。

### 本次 Pilot 不包含

Windows 和 Unreal Engine 5.8 支持会在协作者验证后提供。Linux 尚未测试。

### 获取帮助

更深入的文档请阅读仓库中的 `Docs/Release-2026-05.md` 和 `Plugins/UnrealMcp/README.md`。Bug 请在 `https://github.com/edwinmeng163-oss/UEvolve/issues` 报告。

### 验证包

在 zip 和 sidecar 文件旁运行：

```bash
shasum -a 256 -c UnrealMcp-v0.12.0-pilot-mac-ue56-ue57-source.zip.sha256
```

## 日本語

### 前提条件

- macOS 上の Unreal Editor 5.6.1 または 5.7.4。
- Xcode 26.x、または利用中の Unreal インストールと互換性のある Xcode。
- プロジェクトで組み込みの `PythonScriptPlugin` を有効にしてください。Unreal Engine 5.x にはこのプラグインが同梱されています。
- プラグインフォルダをコピーまたは置き換える前に Unreal Editor を閉じてください。

### インストール方式

推奨はプロジェクトプラグインとして配置する方法です。

```text
<UserProject>/Plugins/UnrealMcp/
```

この方式では Unreal MCP が 1 つのプロジェクトに限定されます。`mcp_apply_scaffold` で追加したツールはそのプロジェクトツリー内に置かれるため、レビュー、バックアップ、削除がしやすくなります。

エンジンプラグインとしての配置は上級者向けです。

```text
<UE Install>/Engine/Plugins/UnrealMcp/
```

この方式では、そのエンジンインストールを使うすべてのプロジェクトで Unreal MCP を利用できます。自拡張はエンジンプラグインのパスへ書き込むため、書き込み権限が必要です。チーム共有のエンジンインストールでは、あるプロジェクトで追加したツールが他のプロジェクトにも影響し、差分が広がることがあります。

### 初回起動

初回のエディタ起動時に、Unreal Build Tool がローカルの Unreal バイナリに合わせてプラグインをコンパイルします。ビルドキャッシュとマシン性能により、約 30 秒から 15 分かかります。

### Drop-in の制限

- Workbench の `Run Core Tests` と `Run RAG Evals` ボタンは、任意の `Tools/UnrealMcpTests/` と `Tools/UnrealMcpKnowledge/Evals/` ディレクトリをプロジェクトへコピーするまで無効です。詳しくはフル workbench リポジトリの `Docs/Release-2026-05.md` と `Plugins/UnrealMcp/README.md` を参照してください。
- `unreal.tools.import_package` には、プロジェクトルートの書き込み可能な `Tools/UnrealMcpToolRegistry/tools.json` が必要です。存在しない場合、ツールはセットアップ手順を含む構造化された `REGISTRY_NOT_INITIALIZED` エラーを返します。
- 開発者間のツール共有は、`unreal.tools.export_package` で `Saved/UnrealMcp/Packages/*.zip` を作成して行います。scaffold ドラフトを転送形式としてコミットしないでください。

### この Pilot に含まれないもの

Windows と Unreal Engine 5.8 のサポートは、協力者による検証後に提供予定です。Linux は未検証です。

### ヘルプ

詳しいドキュメントは、リポジトリ内の `Docs/Release-2026-05.md` と `Plugins/UnrealMcp/README.md` を参照してください。バグは `https://github.com/edwinmeng163-oss/UEvolve/issues` で報告してください。

### パッケージの検証

zip と sidecar ファイルの横で次を実行してください。

```bash
shasum -a 256 -c UnrealMcp-v0.12.0-pilot-mac-ue56-ue57-source.zip.sha256
```
