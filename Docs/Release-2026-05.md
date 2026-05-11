# UEvolve Release Notes & Setup Guide — 2026-05

> Trilingual: [中文](#中文) · [English](#english) · [日本語](#日本語)
> Target audience: new contributors cloning the repo, including Windows users.

---

## 中文

### 更新摘要

本次主线合并把两条 feature 分支整合进 `main`：

1. **多 Provider AI 体系（`feat/ai-multi-provider`）**
2. **RAG 闭环与 Activity 基础（`dev-b/rag-closed-loop`）**
3. **跨平台 Codex Desktop Bridge（P7.D）**

#### 1. 多 Provider AI 体系

`UUnrealMcpSettings::Providers[]` 取代旧的单 OpenAI 字段。已有 OpenAI 用户启动一次会**自动迁移**（旧字段保留并标 deprecated；新表内多一条 `openai-default` 条目）。新增 5 种 Provider Kind：

| Kind | 用途 | 鉴权 |
|---|---|---|
| `OpenAiResponses` | OpenAI Responses API（默认） | `Authorization: Bearer` |
| `OpenAiChatCompat` | OpenAI 兼容 chat/completions —— **覆盖 Kimi / Moonshot、GLM / 智谱、DeepSeek、Qwen、Ollama 等** | 同上 |
| `AnthropicMessages` | Anthropic Claude Messages API（含 tool_use / tool_result + thinking 字段） | `x-api-key` + `anthropic-version` |
| `Codex` | 本地 Codex CLI 子进程（macOS/Linux）—— 锁 `gpt-5.5 xhigh workspace-write` | 无（local） |
| `CodexAppServer` | 通过 Codex App Server bridge 走 Codex Desktop（**跨平台**） | 无（local） |

聊天面板顶部新增 **Provider/Model 选择器**；Codex 系两个 kind 显示 `gpt-5.5 (locked)` 不可改。

#### 2. RAG 闭环

- 新增 Activity Log 基础：`UnrealMcpActivityLog.{h,cpp}` + `Schemas/UnrealMcpActivityLogEntry.schema.json` + 滚动窗口持久化
- 工具调用与聊天面板事件 always-on 落 log
- `Tools/UnrealMcpKnowledge/build_index.py` 索引活动日志与 skill 文档，支持 kind-aware 检索 / 按 kind 过滤 / 分组
- `preview_change_plan` 与 `verify_task_outcome` 现在会注入并反写 `evidence[]` 字段（manifest schema 已扩展）

#### 3. Codex Desktop Bridge

`Tools/UnrealMcpCodexBridge/` —— Bun TypeScript daemon，把 Codex App Server 协议（WebSocket-over-UDS / WebSocket-over-TCP）桥到一个简单的 `ws://127.0.0.1:8766/uevolve` 接口给 UE plugin 用。

**关键能力**：
- Codex 通过此桥能**直接调用 Unreal MCP 工具**（`unreal.spawn_actor` / `unreal.execute_python` / 全部 114 个工具），不再只是给文字方案
- 安全姿态：Codex 自身的 file edit / shell exec 全拒；只放行带 `codex_approval_kind: "mcp_tool_call"` 且 `serverName === "unrealmcp"` 的 MCP 工具调用 —— 由 UE MCP 自己的 audit/dry-run/safety 兜底
- 跨平台：macOS/Linux 用 Unix Domain Socket，Windows 自动切 `ws://127.0.0.1:<auto-port>`
- 模型硬锁：`gpt-5.5` + `xhigh reasoning`（CLAUDE.md 项目策略）

#### 4. UI 体验打磨

- 顶部进度 `SThrobber` + 计时
- 错误条目红色高亮 + ⚠ 前缀
- 工具调用卡片默认折叠，点击展开 args + result
- 每条消息📋复制按钮
- 智能自动滚动 —— 仅当用户在底部时才下拉，读历史不被打断

#### 5. Schema 与工具注册表

- `Schemas/UnrealMcpExtensionManifest.schema.json` 扩展 evidence / outcome 字段
- `Schemas/UnrealMcpActivityLogEntry.schema.json` 新增
- `Tools/UnrealMcpToolRegistry/tools.json` 新工具（chat_label、knowledge 检索增强等）

#### 6. 已知限制

- macOS 上"attach 现有 Codex Desktop 进程的 IPC socket"还没做（v2 增强）；当前 bridge 都是 spawn 一个独立 codex app-server 子进程
- `Codex` CLI provider（直 subprocess）目前只支持 macOS/Linux；Windows 用户请用 `CodexAppServer` provider
- 桥的 approval policy v1 不支持 Codex 在桥内执行 OS 级命令——这是有意为之

### Engine Compatibility / 引擎兼容性

UEvolve plugin 当前支持 Unreal Engine 5.6 和 5.7。同一套 C++ 源码以 UE 5.6 作为最低支持版本；`UEvolve.uproject` 默认 `EngineAssociation` 设为 `5.6`，5.7 用户可以通过右键项目文件选择 **Switch Unreal Engine Version** 升级本地工程绑定。本次代码审计没有发现需要 5.7 专用 API shim 的 plugin 调用；请在 UE 5.6 环境中执行最终编译验证。

---

### 安装指南（新用户从 Git 拉取到本地）

#### 共通先决条件

| 工具 | 用途 | 版本 |
|---|---|---|
| Unreal Engine | 编辑器宿主 | **5.7** |
| Git | 拉代码 | 任意现代版本 |
| Bun | 跑 Codex Bridge daemon（仅当用 CodexAppServer provider 时需要） | ≥ 1.1 |
| Codex CLI | Codex 系 provider 必备 | ≥ 0.130 |
| Codex Desktop | 可选，让 Codex 任务在 GUI 中可见 | 最新 |

#### macOS

```bash
# 1. 装依赖
brew install --cask unreal-engine                              # 或从 Epic Launcher 装 UE 5.7
curl -fsSL https://bun.sh/install | bash                       # Bun
npm install -g @openai/codex-cli                               # Codex CLI（或装 Codex Desktop App）

# 2. 拉代码
git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve

# 3. 生成 Xcode 工程并编译
/Users/Shared/Epic\ Games/UE_5.7/Engine/Build/BatchFiles/Mac/GenerateProjectFiles.sh -project="$(pwd)/UEvolve.uproject" -game
/Users/Shared/Epic\ Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh UEvolveEditor Mac Development \
  -Project="$(pwd)/UEvolve.uproject" -waitmutex

# 4. 启动编辑器
"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$(pwd)/UEvolve.uproject"
```

#### Linux

```bash
# 1. UE 5.7 from Epic（按官方说明），Bun 与 Codex CLI 同 macOS
curl -fsSL https://bun.sh/install | bash
npm install -g @openai/codex-cli

# 2. 拉代码 + 编译
git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve
/path/to/UE_5.7/Engine/Build/BatchFiles/Linux/Build.sh UEvolveEditor Linux Development \
  -Project="$(pwd)/UEvolve.uproject" -waitmutex
```

#### Windows

```powershell
# 1. 装依赖
# - 通过 Epic Games Launcher 装 UE 5.7
# - https://bun.sh 下载 Windows 安装器
# - npm install -g @openai/codex-cli   (或装 Codex Desktop 桌面应用)

# 2. 拉代码
git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve

# 3. 编译
"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" UEvolveEditor Win64 Development `
  -Project="$PWD\UEvolve.uproject" -waitmutex

# 4. 启动编辑器
"C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" `
  "$PWD\UEvolve.uproject"
```

#### 配置 AI Provider

1. 打开编辑器，菜单 `Edit > Project Settings > Plugins > Unreal MCP > AI`
2. 勾选 `bEnableAiAssistant`
3. 在 `Providers` 数组里添加条目（至少一条），按你选的服务填：

```toml
# 例：Kimi（OpenAI Chat 兼容）
Id              = kimi-default
DisplayName     = Kimi
Kind            = OpenAiChatCompat
BaseUrl         = https://api.moonshot.cn/v1/chat/completions
ApiKey          = <你的 moonshot api key>
Model           = moonshot-v1-8k

# 例：GLM
Id, DisplayName = glm, GLM 4
Kind            = OpenAiChatCompat
BaseUrl         = https://open.bigmodel.cn/api/paas/v4/chat/completions
Model           = glm-4

# 例：DeepSeek
BaseUrl         = https://api.deepseek.com/v1/chat/completions
Model           = deepseek-chat

# 例：Anthropic Claude
Kind            = AnthropicMessages
BaseUrl         = https://api.anthropic.com/v1/messages
Model           = claude-sonnet-4-5
```

4. 设 `ActiveProviderId` 等于你想用的那条的 `Id`，保存（Ctrl+S）
5. 打开 `Window > Unreal MCP Chat`，顶部 Provider 选择器应能切换、模型字段显示当前选择

#### Codex Desktop Bridge（可选）

让 Codex 真正驱动 Unreal 工具：

**1. 起 bridge daemon**（同一台机器上）

```bash
# macOS / Linux
Tools/UnrealMcpCodexBridge/start-bridge.sh
```

```cmd
:: Windows cmd.exe
Tools\UnrealMcpCodexBridge\start-bridge.cmd
```

```powershell
# Windows PowerShell
powershell -ExecutionPolicy Bypass -File Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

启动后看到：

```text
Codex binary: /opt/homebrew/bin/codex
UEvolve Codex Bridge listening at ws://127.0.0.1:8766/uevolve
Codex app-server transport=unix endpoint=/var/.../codex.sock
# Windows 上会显示 transport=ws endpoint=ws://127.0.0.1:<port>
```

**2. UE 里加 CodexAppServer provider**

```toml
Id              = codex-desktop
DisplayName     = Codex Desktop
Kind            = CodexAppServer
BaseUrl         = ws://127.0.0.1:8766/uevolve
```

**3. ChatPanel 顶部 Provider 切到 `Codex Desktop`，发 `/ask` 即可。**

Bridge 已自动把编辑器的 MCP server（`http://127.0.0.1:8765/mcp`）注册给 Codex，所以 Codex 能直接调 Unreal 工具。

**环境变量（cross-platform）**

| 变量 | 默认值 | 作用 |
|---|---|---|
| `UEVOLVE_CODEX_BRIDGE_PORT` | 8766 | UE 朝桥的 WebSocket 端口 |
| `UEVOLVE_CODEX_TRANSPORT` | `unix`(POSIX) / `ws`(Windows) | 桥跟 codex 之间用 Unix socket 还是 WebSocket |
| `UEVOLVE_CODEX_APP_SERVER_PORT` | auto | 强制 codex app-server 绑定的端口（仅 ws 模式生效） |
| `UEVOLVE_CODEX_BIN` | auto (`where`/`which`) | Codex 可执行文件绝对路径；Windows 非 PATH 安装时设置 |
| `UEVOLVE_MCP_URL` | `http://127.0.0.1:8765/mcp` | UE MCP server 地址（编辑器没默认端口请改） |
| `UEVOLVE_CODEX_APPROVAL_POLICY` | `reject` | OS 级能力策略；`auto-approve` 仅本地开发用 |

#### 验证一切就绪

ChatPanel 里依次发：

1. `/ask 用 unreal.editor_status 报当前关卡` → 应看到工具调用卡片 + currentMap
2. `/ask 在原点生成一个 PointLight 起名 TestLight` → 关卡里真出现一盏灯
3. （Codex Desktop 路径）`/ask 用 execute_python 在原点画一个 3×3 立方体迷宫` → Codex 调用 MCP，关卡里出现迷宫

---

## English

### What's New

This release merges two feature branches into `main`:

1. **Multi-provider AI subsystem** (`feat/ai-multi-provider`)
2. **RAG closed loop + activity log foundation** (`dev-b/rag-closed-loop`)
3. **Cross-platform Codex Desktop Bridge** (P7.D)

#### 1. Multi-provider AI

`UUnrealMcpSettings::Providers[]` replaces the legacy single-OpenAI flat fields. Existing OpenAI users are **auto-migrated** on first launch (legacy fields kept and deprecated; the new array gets an `openai-default` entry). Five provider kinds:

| Kind | Purpose | Auth |
|---|---|---|
| `OpenAiResponses` | OpenAI Responses API (default) | `Authorization: Bearer` |
| `OpenAiChatCompat` | OpenAI-compatible chat/completions — **covers Kimi (Moonshot), GLM (Zhipu), DeepSeek, Qwen, Ollama** | same |
| `AnthropicMessages` | Anthropic Claude Messages API (tool_use / tool_result + optional thinking) | `x-api-key` + `anthropic-version` |
| `Codex` | Local Codex CLI subprocess (macOS/Linux only) — model locked to `gpt-5.5 xhigh workspace-write` | none |
| `CodexAppServer` | Via the Codex App Server bridge — **cross-platform** | none |

A Provider/Model selector at the top of the chat panel switches between configured entries; Codex-kind providers display `gpt-5.5 (locked)` since they hard-code the model per project policy.

#### 2. RAG Closed Loop

- New activity log foundation: `UnrealMcpActivityLog.{h,cpp}`, schema, rolling-window persistence.
- Always-on emitters from tool execution and chat panel.
- `Tools/UnrealMcpKnowledge/build_index.py` indexes activity log and skill docs with kind-aware search, grouping, and source-kind filters.
- `preview_change_plan` and `verify_task_outcome` inject and persist `evidence[]` against the extension manifest schema.

#### 3. Codex Desktop Bridge

`Tools/UnrealMcpCodexBridge/` is a Bun TypeScript daemon that bridges the Codex App Server protocol (WebSocket-over-UDS on POSIX, WebSocket-over-TCP on Windows) to a simple `ws://127.0.0.1:8766/uevolve` endpoint consumed by the UE plugin.

**Capabilities**:
- Codex can **invoke Unreal MCP tools directly** (`unreal.spawn_actor`, `unreal.execute_python`, all 114 tools) instead of only emitting `/tool ...` text suggestions.
- Safety posture: Codex's built-in file edit and shell exec are universally rejected; only MCP tool-call approvals whose `serverName === "unrealmcp"` AND `_meta.codex_approval_kind === "mcp_tool_call"` are auto-accepted. UE MCP's own audit / dry-run / safety layer is trusted to gate destructive work.
- Cross-platform: macOS/Linux uses a Unix Domain Socket transport; Windows defaults to `ws://127.0.0.1:<auto-port>`.
- Model hard-locked: `gpt-5.5` with `xhigh` reasoning per CLAUDE.md.

#### 4. UI Polish

- `SThrobber` + elapsed-time label while a request is in flight
- Error entries get a subtle red background and a ⚠ glyph
- Tool call cards default to collapsed `SExpandableArea`, expand to show args + result
- Per-entry 📋 copy button
- Smart auto-scroll — only follows the bottom when the user is already there

#### 5. Schemas & Tool Registry

- `Schemas/UnrealMcpExtensionManifest.schema.json` extended with evidence/outcome fields
- `Schemas/UnrealMcpActivityLogEntry.schema.json` added
- `Tools/UnrealMcpToolRegistry/tools.json` gains the new tools (chat_label, knowledge search enhancements)

#### 6. Known Limitations

- Attaching the bridge to an already-running Codex Desktop's IPC socket (`ipc-501.sock` on macOS) is a future enhancement; the current bridge always spawns its own codex app-server subprocess.
- The `Codex` CLI provider (subprocess) is macOS/Linux only. Windows users should use the `CodexAppServer` provider via the bridge.
- The bridge's V1 approval policy intentionally denies Codex's built-in OS-level capabilities. This is by design.

### Engine Compatibility

The UEvolve plugin supports Unreal Engine 5.6 and 5.7 from the same source tree. UE 5.6 is the lower bound, so `UEvolve.uproject` defaults `EngineAssociation` to `5.6`; UE 5.7 users can switch the local project association through **Switch Unreal Engine Version**. This code-only audit found no plugin API calls that require UE 5.7-specific conditional shims; final compile validation still needs to run in a UE 5.6 installation.

---

### Setup Guide (new contributor cloning the repo)

#### Common Prerequisites

| Tool | Why | Version |
|---|---|---|
| Unreal Engine | Editor host | **5.7** |
| Git | Source control | any modern |
| Bun | Codex bridge daemon (only needed for `CodexAppServer` provider) | ≥ 1.1 |
| Codex CLI | Required for any Codex-kind provider | ≥ 0.130 |
| Codex Desktop | Optional GUI companion | latest |

#### macOS

```bash
brew install --cask unreal-engine                              # or via Epic Launcher
curl -fsSL https://bun.sh/install | bash
npm install -g @openai/codex-cli

git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve

/Users/Shared/Epic\ Games/UE_5.7/Engine/Build/BatchFiles/Mac/GenerateProjectFiles.sh \
  -project="$(pwd)/UEvolve.uproject" -game
/Users/Shared/Epic\ Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh \
  UEvolveEditor Mac Development -Project="$(pwd)/UEvolve.uproject" -waitmutex

"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$(pwd)/UEvolve.uproject"
```

#### Linux

```bash
curl -fsSL https://bun.sh/install | bash
npm install -g @openai/codex-cli

git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve
/path/to/UE_5.7/Engine/Build/BatchFiles/Linux/Build.sh \
  UEvolveEditor Linux Development -Project="$(pwd)/UEvolve.uproject" -waitmutex
```

#### Windows

```powershell
# Install UE 5.7 via Epic Games Launcher.
# Install Bun from https://bun.sh (Windows installer).
# npm install -g @openai/codex-cli   (or install Codex Desktop instead)

git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve

& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  UEvolveEditor Win64 Development -Project="$PWD\UEvolve.uproject" -waitmutex

& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" `
  "$PWD\UEvolve.uproject"
```

#### Configure an AI Provider

1. `Edit > Project Settings > Plugins > Unreal MCP > AI`
2. Toggle `bEnableAiAssistant`
3. Add at least one row to `Providers`. Examples:

```toml
# Kimi (Moonshot)
Id          = kimi-default
DisplayName = Kimi
Kind        = OpenAiChatCompat
BaseUrl     = https://api.moonshot.cn/v1/chat/completions
ApiKey      = <your moonshot key>
Model       = moonshot-v1-8k

# GLM (Zhipu)
Kind        = OpenAiChatCompat
BaseUrl     = https://open.bigmodel.cn/api/paas/v4/chat/completions
Model       = glm-4

# DeepSeek
BaseUrl     = https://api.deepseek.com/v1/chat/completions
Model       = deepseek-chat

# Anthropic Claude
Kind        = AnthropicMessages
BaseUrl     = https://api.anthropic.com/v1/messages
Model       = claude-sonnet-4-5
```

4. Set `ActiveProviderId` to the row you want. Save.
5. Open `Window > Unreal MCP Chat`. The top-bar Provider selector should list your entries; the model field reflects the active provider.

#### Codex Desktop Bridge (optional, recommended for "let Codex do work")

**1. Start the bridge**

```bash
# macOS / Linux
Tools/UnrealMcpCodexBridge/start-bridge.sh
```

```cmd
:: Windows cmd.exe
Tools\UnrealMcpCodexBridge\start-bridge.cmd
```

```powershell
# Windows PowerShell
powershell -ExecutionPolicy Bypass -File Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

Expected startup:

```text
Codex binary: /opt/homebrew/bin/codex
UEvolve Codex Bridge listening at ws://127.0.0.1:8766/uevolve
Codex app-server transport=unix endpoint=/var/.../codex.sock     # POSIX
# or:
Codex app-server transport=ws endpoint=ws://127.0.0.1:<port>     # Windows or override
```

**2. Add a `CodexAppServer` provider in UE**

```toml
Id          = codex-desktop
DisplayName = Codex Desktop
Kind        = CodexAppServer
BaseUrl     = ws://127.0.0.1:8766/uevolve
```

**3. Switch to it in the chat panel and ask `/ask`.** The bridge auto-registers the editor's MCP server with Codex, so Codex can call all Unreal tools.

**Environment variables (cross-platform)**

| Var | Default | Purpose |
|---|---|---|
| `UEVOLVE_CODEX_BRIDGE_PORT` | 8766 | UE-facing WebSocket port |
| `UEVOLVE_CODEX_TRANSPORT` | `unix` (POSIX) / `ws` (Windows) | Transport to Codex App Server |
| `UEVOLVE_CODEX_APP_SERVER_PORT` | auto | Pin Codex's listen port (ws mode) |
| `UEVOLVE_CODEX_BIN` | auto (`where`/`which`) | Absolute path to `codex.exe`, `codex.cmd`, or `codex` if auto-detection misses it |
| `UEVOLVE_MCP_URL` | `http://127.0.0.1:8765/mcp` | UE MCP server endpoint |
| `UEVOLVE_CODEX_APPROVAL_POLICY` | `reject` | Codex OS-level cap policy. `auto-approve` for local dev only. |

#### Verify

In the chat panel:

1. `/ask Use unreal.editor_status to report the current level` → tool card + currentMap
2. `/ask Spawn a PointLight named TestLight at origin` → a light appears in the level
3. (Codex Desktop path) `/ask Use execute_python to build a 3x3 cube maze at origin` → Codex calls MCP, maze appears

---

## 日本語

### アップデート概要

このリリースで 2 つの feature ブランチが `main` にマージされました：

1. **マルチプロバイダー AI 基盤**（`feat/ai-multi-provider`）
2. **RAG クローズドループとアクティビティ基盤**（`dev-b/rag-closed-loop`）
3. **クロスプラットフォーム Codex Desktop Bridge**（P7.D）

#### 1. マルチプロバイダー AI

`UUnrealMcpSettings::Providers[]` が旧来の OpenAI 単一フィールドを置き換えます。既存の OpenAI ユーザーは初回起動時に**自動マイグレーション**されます（旧フィールドは保持されたまま deprecated 化、新しい配列に `openai-default` エントリが追加されます）。5 種類の Provider Kind を追加：

| Kind | 用途 | 認証 |
|---|---|---|
| `OpenAiResponses` | OpenAI Responses API（デフォルト） | `Authorization: Bearer` |
| `OpenAiChatCompat` | OpenAI 互換 chat/completions — **Kimi（Moonshot）、GLM（Zhipu）、DeepSeek、Qwen、Ollama** をカバー | 同上 |
| `AnthropicMessages` | Anthropic Claude Messages API（tool_use / tool_result + thinking） | `x-api-key` + `anthropic-version` |
| `Codex` | ローカル Codex CLI サブプロセス（macOS/Linux のみ） — `gpt-5.5 xhigh workspace-write` 固定 | なし |
| `CodexAppServer` | Codex App Server bridge 経由 — **クロスプラットフォーム** | なし |

チャットパネル上部に **Provider/Model セレクター** が追加されました。Codex 系の 2 種類は `gpt-5.5 (locked)` と表示され、モデル変更は不可です。

#### 2. RAG クローズドループ

- Activity Log 基盤：`UnrealMcpActivityLog.{h,cpp}` + `Schemas/UnrealMcpActivityLogEntry.schema.json` + ローリングウィンドウ永続化
- ツール実行とチャットパネルからの always-on イベント送出
- `Tools/UnrealMcpKnowledge/build_index.py` がアクティビティログとスキルドキュメントをインデックス化、kind-aware 検索 / フィルタ / グルーピングをサポート
- `preview_change_plan` と `verify_task_outcome` が manifest schema の `evidence[]` フィールドへ証跡を注入・反映

#### 3. Codex Desktop Bridge

`Tools/UnrealMcpCodexBridge/` は Bun の TypeScript daemon で、Codex App Server プロトコル（POSIX では WebSocket-over-UDS、Windows では WebSocket-over-TCP）を UE plugin が消費するシンプルな `ws://127.0.0.1:8766/uevolve` エンドポイントに橋渡しします。

**主要機能**：
- Codex が **Unreal MCP ツール（`unreal.spawn_actor`、`unreal.execute_python` 等、計 114 個）を直接呼び出せる**ようになります。これまでのように `/tool ...` 文字列を返すだけ、ではなく実行されます
- セーフティポスチャ：Codex 内蔵のファイル編集・シェル実行はすべて拒否。MCP ツール呼び出し承認のうち、`serverName === "unrealmcp"` かつ `_meta.codex_approval_kind === "mcp_tool_call"` のものだけが自動承認されます。破壊的変更は UE MCP 側の audit / dry-run / safety レイヤーが担保します
- クロスプラットフォーム：macOS/Linux は Unix Domain Socket、Windows は `ws://127.0.0.1:<auto-port>`
- モデル固定：`gpt-5.5` + `xhigh reasoning`（CLAUDE.md プロジェクトポリシー）

#### 4. UI 改善

- 進捗 `SThrobber` + 経過時間ラベル
- エラーエントリの赤背景 + ⚠ 接頭辞
- ツールカードはデフォルト折りたたみ、クリックで展開して args + result を表示
- 各メッセージに 📋 コピーボタン
- スマートオートスクロール — ユーザーが下部にいる時のみ追従、履歴閲覧中は中断しない

#### 5. スキーマとツールレジストリ

- `Schemas/UnrealMcpExtensionManifest.schema.json` に evidence / outcome フィールド追加
- `Schemas/UnrealMcpActivityLogEntry.schema.json` 新規
- `Tools/UnrealMcpToolRegistry/tools.json` に新ツール（chat_label、knowledge 検索拡張等）

#### 6. 既知の制限

- macOS で「すでに起動中の Codex Desktop の IPC socket にアタッチする」機能は今後の拡張です。現状の bridge は常に独自の codex app-server サブプロセスを spawn します
- `Codex` CLI provider（直接サブプロセス）は macOS/Linux のみ対応。Windows ユーザーは `CodexAppServer` provider を使用してください
- Bridge V1 の承認ポリシーは Codex の OS レベル操作を意図的に拒否します（設計通り）

### Engine Compatibility / エンジン互換性

UEvolve plugin は Unreal Engine 5.6 と 5.7 の両方を同じソースツリーでサポートします。最低対応バージョンは UE 5.6 のため、`UEvolve.uproject` の既定 `EngineAssociation` は `5.6` です。UE 5.7 のユーザーは **Switch Unreal Engine Version** でローカルプロジェクトの関連付けを更新できます。今回のコードのみの監査では UE 5.7 専用の conditional shim が必要な plugin API 呼び出しは見つかっていません。最終確認は UE 5.6 環境でのコンパイルで行ってください。

---

### セットアップ手順（Git から新規クローン）

#### 共通の前提条件

| ツール | 用途 | バージョン |
|---|---|---|
| Unreal Engine | エディタ | **5.7** |
| Git | ソース管理 | 任意 |
| Bun | Codex bridge daemon（`CodexAppServer` provider 使用時のみ必要） | ≥ 1.1 |
| Codex CLI | Codex 系 provider に必須 | ≥ 0.130 |
| Codex Desktop | 任意の GUI コンパニオン | 最新 |

#### macOS

```bash
brew install --cask unreal-engine                              # または Epic Launcher 経由
curl -fsSL https://bun.sh/install | bash
npm install -g @openai/codex-cli

git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve

/Users/Shared/Epic\ Games/UE_5.7/Engine/Build/BatchFiles/Mac/GenerateProjectFiles.sh \
  -project="$(pwd)/UEvolve.uproject" -game
/Users/Shared/Epic\ Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh \
  UEvolveEditor Mac Development -Project="$(pwd)/UEvolve.uproject" -waitmutex

"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$(pwd)/UEvolve.uproject"
```

#### Linux

```bash
curl -fsSL https://bun.sh/install | bash
npm install -g @openai/codex-cli

git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve
/path/to/UE_5.7/Engine/Build/BatchFiles/Linux/Build.sh \
  UEvolveEditor Linux Development -Project="$(pwd)/UEvolve.uproject" -waitmutex
```

#### Windows

```powershell
# Epic Games Launcher で UE 5.7 をインストール
# https://bun.sh から Windows 用 Bun をインストール
# npm install -g @openai/codex-cli  （または Codex Desktop アプリを使用）

git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve

& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  UEvolveEditor Win64 Development -Project="$PWD\UEvolve.uproject" -waitmutex

& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" `
  "$PWD\UEvolve.uproject"
```

#### AI Provider の設定

1. `Edit > Project Settings > Plugins > Unreal MCP > AI`
2. `bEnableAiAssistant` を ON
3. `Providers` 配列に最低 1 行追加。例：

```toml
# Kimi（Moonshot）
Id          = kimi-default
DisplayName = Kimi
Kind        = OpenAiChatCompat
BaseUrl     = https://api.moonshot.cn/v1/chat/completions
ApiKey      = <Moonshot の API キー>
Model       = moonshot-v1-8k

# GLM（Zhipu）
Kind        = OpenAiChatCompat
BaseUrl     = https://open.bigmodel.cn/api/paas/v4/chat/completions
Model       = glm-4

# DeepSeek
BaseUrl     = https://api.deepseek.com/v1/chat/completions
Model       = deepseek-chat

# Anthropic Claude
Kind        = AnthropicMessages
BaseUrl     = https://api.anthropic.com/v1/messages
Model       = claude-sonnet-4-5
```

4. `ActiveProviderId` を選択した行の `Id` に設定して保存
5. `Window > Unreal MCP Chat` を開き、上部の Provider セレクターが利用可能なら成功

#### Codex Desktop Bridge（任意、Codex に作業させたい場合に推奨）

**1. Bridge を起動**

```bash
# macOS / Linux
Tools/UnrealMcpCodexBridge/start-bridge.sh
```

```cmd
:: Windows cmd.exe
Tools\UnrealMcpCodexBridge\start-bridge.cmd
```

```powershell
# Windows PowerShell
powershell -ExecutionPolicy Bypass -File Tools\UnrealMcpCodexBridge\start-bridge.ps1
```

期待される起動ログ：

```text
Codex binary: /opt/homebrew/bin/codex
UEvolve Codex Bridge listening at ws://127.0.0.1:8766/uevolve
Codex app-server transport=unix endpoint=/var/.../codex.sock     # POSIX
# あるいは：
Codex app-server transport=ws endpoint=ws://127.0.0.1:<port>     # Windows またはオーバーライド時
```

**2. UE に `CodexAppServer` provider を追加**

```toml
Id          = codex-desktop
DisplayName = Codex Desktop
Kind        = CodexAppServer
BaseUrl     = ws://127.0.0.1:8766/uevolve
```

**3. チャットパネルで切り替えて `/ask` を送信。** Bridge がエディタの MCP server（`http://127.0.0.1:8765/mcp`）を Codex に自動登録するので、Codex は Unreal ツールを直接呼び出せます。

**環境変数（クロスプラットフォーム）**

| 変数 | デフォルト | 用途 |
|---|---|---|
| `UEVOLVE_CODEX_BRIDGE_PORT` | 8766 | UE 側 WebSocket ポート |
| `UEVOLVE_CODEX_TRANSPORT` | `unix`(POSIX) / `ws`(Windows) | Bridge ⇔ Codex 間のトランスポート |
| `UEVOLVE_CODEX_APP_SERVER_PORT` | auto | Codex の listen ポートを固定（ws モード） |
| `UEVOLVE_CODEX_BIN` | auto (`where`/`which`) | 自動検出できない場合の `codex.exe`、`codex.cmd`、または `codex` の絶対パス |
| `UEVOLVE_MCP_URL` | `http://127.0.0.1:8765/mcp` | UE MCP server エンドポイント |
| `UEVOLVE_CODEX_APPROVAL_POLICY` | `reject` | Codex の OS 操作ポリシー。`auto-approve` はローカル開発専用 |

#### 動作確認

チャットパネルで順に：

1. `/ask unreal.editor_status で現在のレベルを報告して` → ツールカード + currentMap が表示
2. `/ask 原点に TestLight という名前の PointLight を生成して` → レベルに実際にライトが出現
3. （Codex Desktop ルート）`/ask execute_python で原点に 3x3 の立方体迷宫を作って` → Codex が MCP を呼び、迷宫が出現

---

## トラブルシューティング / Troubleshooting / 故障排查

| 症状 / Symptom | 原因 / Cause | 対処 / Fix |
|---|---|---|
| `127.0.0.1:8765 already in use` | 別のエディタが起動中 / Another editor is running | 全エディタを閉じてから再起動 / Close all editors |
| ChatPanel `No AI providers configured` | `Providers` 配列が空 / Empty providers list | Project Settings で 1 行追加 / Add a row |
| Bridge `Failed to connect` | Bridge daemon 未起動 / Daemon not running | `start-bridge` launcher で起動 / Start with a `start-bridge` launcher |
| `codex.cmd not found` / .cmd shim issue / .cmd シム問題 | Bun/Node spawn missed Windows shim | Set `UEVOLVE_CODEX_BIN` to full path |
| Codex returns `Provider not authorized` | API key 未設定 / Empty key | Provider 設定で `ApiKey` 設定 / Fill ApiKey |
| Anthropic 400 about `thinking` | モデルが thinking 非対応 / Model lacks thinking | `ReasoningEffort` を空に / Clear ReasoningEffort |
| Codex 「I cannot run commands」 | bridge approval policy が reject / Bridge rejecting | 期待通り（Codex MCP 経由で動作）/ Expected — Codex works via MCP |
| Windows で `unix://` エラー / unix:// error on Windows | デフォルトが unix だった / Default was unix | `UEVOLVE_CODEX_TRANSPORT=ws` を設定 / Set the env var |

---

## License & Contributing

See repo root `README.md` and `AGENTS.md`.
