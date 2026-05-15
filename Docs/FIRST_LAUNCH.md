# UEvolve Full Experience First Launch

## English

### Prerequisites

- Windows 10 or Windows 11.
- Epic Launcher Unreal Engine 5.6.1. Use the Epic Launcher binary build, not an engine source build.
- Visual Studio 2022 with the "Game Development with C++" workload.
- .NET 6 or newer.
- For the no-compile pure-unzip experience, your project must be Blueprint-only with no `<Project>/Source/` directory. C++ projects will still rebuild their own module on first open after dropping the zip in; that is the project compiling, not the plugin.

### Step 1 - Extract

Extract `UnrealMcp-v0.12.0-pilot-full-win-ue561.zip` into your Unreal project root directory, next to `<YourProject>.uproject`.
After extraction you should see `Plugins/`, `Tools/`, `Docs/`, and `Schemas/` at the project root.

### Step 2 - Enable Plugins

Edit `<YourProject>.uproject` and add both plugins to the `Plugins` array:

```json
{ "Name": "PythonScriptPlugin", "Enabled": true },
{ "Name": "UnrealMcp", "Enabled": true }
```

### Step 3 - Open Unreal Editor

Double-click `<YourProject>.uproject`.
The plugin should load without a rebuild prompt because the bundled Win64 binary matches Epic Launcher Unreal Engine 5.6.1.

### Step 4 - Start The Codex Bridge

In File Explorer, double-click `Tools\UnrealMcpCodexBridge\start-bridge.cmd`.
A console window should show `Bridge listening on ws://127.0.0.1:8766/uevolve`.
Leave that window open while using Chat.

### Step 5 - Open Chat Or Workbench

In Unreal Editor, open `Window > Unreal MCP Chat` or `Window > Unreal MCP Workbench`.
Configure your AI provider in `Project Settings > Plugins > Unreal MCP`.
Test the setup by asking Chat to `list maps` or by calling `unreal.editor_status`.

### Troubleshooting

- If the plugin does not load or Unreal asks to rebuild, your engine BuildId probably does not match Epic Launcher Unreal Engine 5.6.1. Source-built engines need a locally built binary instead.
- If `start-bridge.cmd` flashes and closes, open PowerShell in `Tools\UnrealMcpCodexBridge` and run `.\start-bridge.cmd` so the error stays visible.
- If Chat cannot see tools, confirm `Tools\UnrealMcpToolRegistry\tools.json` exists under the project root. If it is missing, the zip was probably extracted into the wrong directory.

## 中文

### 前提条件

- Windows 10 或 Windows 11。
- Epic Launcher 安装的 Unreal Engine 5.6.1。请使用 Epic Launcher 二进制版本，不要使用源码编译版引擎。
- Visual Studio 2022，并安装 "Game Development with C++" 工作负载。
- .NET 6 或更新版本。
- 如果想获得无需编译、纯解压即可启动的体验，项目必须是 Blueprint-only，且没有 `<Project>/Source/` 目录。C++ 项目在放入 zip 后首次打开时仍会重新构建自己的项目模块；这是项目在编译，不是插件在编译。

### 步骤 1 - 解压

把 `UnrealMcp-v0.12.0-pilot-full-win-ue561.zip` 解压到你的 Unreal 项目根目录，也就是 `<YourProject>.uproject` 所在目录。
解压后，项目根目录下应该出现 `Plugins/`、`Tools/`、`Docs/` 和 `Schemas/`。

### 步骤 2 - 启用插件

编辑 `<YourProject>.uproject`，在 `Plugins` 数组中加入这两个插件：

```json
{ "Name": "PythonScriptPlugin", "Enabled": true },
{ "Name": "UnrealMcp", "Enabled": true }
```

### 步骤 3 - 打开 Unreal Editor

双击 `<YourProject>.uproject`。
插件应当直接加载，不会提示重新构建，因为包内 Win64 二进制与 Epic Launcher 的 Unreal Engine 5.6.1 匹配。

### 步骤 4 - 启动 Codex Bridge

在文件资源管理器中双击 `Tools\UnrealMcpCodexBridge\start-bridge.cmd`。
控制台窗口应显示 `Bridge listening on ws://127.0.0.1:8766/uevolve`。
使用 Chat 时请保持这个窗口打开。

### 步骤 5 - 打开 Chat 或 Workbench

在 Unreal Editor 中打开 `Window > Unreal MCP Chat` 或 `Window > Unreal MCP Workbench`。
在 `Project Settings > Plugins > Unreal MCP` 中配置 AI provider。
可以通过让 Chat 执行 `list maps`，或调用 `unreal.editor_status` 来测试配置。

### 故障排查

- 如果插件无法加载，或 Unreal 提示重新构建，通常是引擎 BuildId 与 Epic Launcher 的 Unreal Engine 5.6.1 不匹配。源码编译版引擎需要本机重新构建的二进制。
- 如果 `start-bridge.cmd` 窗口一闪而过，请在 `Tools\UnrealMcpCodexBridge` 中打开 PowerShell 并运行 `.\start-bridge.cmd`，这样错误信息会保留在窗口里。
- 如果 Chat 看不到工具，请确认项目根目录下存在 `Tools\UnrealMcpToolRegistry\tools.json`。如果缺失，通常是 zip 解压到了错误目录。

## 日本語

### 前提条件

- Windows 10 または Windows 11。
- Epic Launcher 版 Unreal Engine 5.6.1。エンジンをソースからビルドした版ではなく、Epic Launcher のバイナリ版を使ってください。
- Visual Studio 2022 と "Game Development with C++" ワークロード。
- .NET 6 以降。
- コンパイルなしの純粋な unzip 体験にするには、プロジェクトが Blueprint-only で、`<Project>/Source/` ディレクトリが存在しない必要があります。C++ プロジェクトでは zip を配置した後の初回起動時に自分のプロジェクトモジュールが再ビルドされます。これはプラグインではなくプロジェクト側のコンパイルです。

### Step 1 - 展開

`UnrealMcp-v0.12.0-pilot-full-win-ue561.zip` を Unreal プロジェクトのルート、つまり `<YourProject>.uproject` と同じ場所へ展開します。
展開後、プロジェクトルートに `Plugins/`、`Tools/`、`Docs/`、`Schemas/` があることを確認してください。

### Step 2 - プラグインを有効化

`<YourProject>.uproject` を編集し、`Plugins` 配列に次の 2 つを追加します。

```json
{ "Name": "PythonScriptPlugin", "Enabled": true },
{ "Name": "UnrealMcp", "Enabled": true }
```

### Step 3 - Unreal Editor を開く

`<YourProject>.uproject` をダブルクリックします。
同梱の Win64 バイナリは Epic Launcher 版 Unreal Engine 5.6.1 に合わせてあるため、プラグインは再ビルドの確認なしで読み込まれるはずです。

### Step 4 - Codex Bridge を起動

エクスプローラーで `Tools\UnrealMcpCodexBridge\start-bridge.cmd` をダブルクリックします。
コンソールに `Bridge listening on ws://127.0.0.1:8766/uevolve` と表示されます。
Chat を使う間はこのウィンドウを開いたままにしてください。

### Step 5 - Chat または Workbench を開く

Unreal Editor で `Window > Unreal MCP Chat` または `Window > Unreal MCP Workbench` を開きます。
`Project Settings > Plugins > Unreal MCP` で AI provider を設定します。
Chat に `list maps` と依頼するか、`unreal.editor_status` を呼び出して動作確認してください。

### トラブルシューティング

- プラグインが読み込まれない、または Unreal が再ビルドを求める場合、Engine BuildId が Epic Launcher 版 Unreal Engine 5.6.1 と一致していない可能性があります。ソースビルドのエンジンでは、その環境でビルドしたバイナリが必要です。
- `start-bridge.cmd` のウィンドウがすぐ閉じる場合は、`Tools\UnrealMcpCodexBridge` で PowerShell を開き、`.\start-bridge.cmd` を実行してエラーを確認してください。
- Chat からツールが見えない場合は、プロジェクトルートに `Tools\UnrealMcpToolRegistry\tools.json` があるか確認してください。存在しない場合、zip を展開した場所が間違っている可能性があります。
