# Imperial Tavern / Unreal MCP Prototype

This repository is an Unreal Engine 5.7 prototype for **Imperial Tavern** (`帝国酒馆`), an RTS auto-battler concept built around tavern-phase card management, shared card pools, squad growth, civilization traits, and automated combat.

It also includes an in-project editor plugin, **Unreal MCP**, which exposes Unreal Editor automation through a local Model Context Protocol server and an in-editor AI/chat workflow.

## 中文概览

本项目目前是一个 **帝国酒馆 / Imperial Tavern** 的 UE5 MVP 平台，同时也是一个带有 Unreal MCP 开发工具扩展的编辑器自动化实验项目。

当前重点不是已经完成完整玩法，而是先搭好：

- Imperial Tavern 的地图、核心蓝图、UI、占位单位和系统骨架。
- 面向自动战斗/酒馆回合制玩法的数据与架构方向。
- 可由 AI 调用的 Unreal Editor 工具层，包括 Blueprint 图编辑、UMG 编辑和玩法系统脚手架生成。
- 适合继续开发和 GitHub 托管的 Unreal 项目结构、Git LFS 和忽略规则。

## 已有功能概览

### Imperial Tavern MVP

- 已创建 `/Game/ImperialTavern` 独立功能目录。
- 已基于 Strategy 模板复制出 MVP 地图 `LVL_ImperialTavern_MVP`。
- 已创建核心游戏框架蓝图骨架：GameMode、GameState、PlayerState、PlayerController、AIController。
- 已创建玩法管理器蓝图骨架：BoardManager、ShopManager、CombatArena。
- 已创建单位和卡牌占位蓝图：UnitBase、CardActor、PlaceholderUnit。
- 已创建文明能力组件基类：CivAbilityBase。
- 已创建基础 UI Widget 蓝图：HUD、Shop、Board、EconomyBar、Discover。
- 已设置 Unreal 二进制资源通过 Git LFS 管理。

### Unreal MCP 开发工具

- 已内置 Unreal MCP 插件，并提供本地 MCP 服务端点。
- 已支持项目检查、地图列表、资产列表、PIE 控制、日志读取、Map Check 等编辑器自动化能力。
- 已扩展 Blueprint 图编辑工具，用于添加变量、函数、事件节点、调用节点、分支、循环、连线、默认值和编译保存。
- 已扩展 UMG 可视化编辑工具，用于添加/删除 Widget、设置属性、设置布局、绑定事件、绑定蓝图变量和生成 UI 模板。
- 已扩展 Imperial Tavern 高层脚手架工具，用于生成回合、商店、经济、自动战斗 AI 和结算 UI 的原型骨架。

### 当前边界

- 当前还不是完整可玩的帝国酒馆游戏。
- 现有蓝图多为骨架和占位资产，核心经济、商店、三连、战斗、胜负结算还需要继续填充逻辑。
- AI 工具可以辅助创建和整理资产、生成脚手架、执行编辑器自动化，但复杂玩法仍建议逐步验证，不要一次性盲写完整系统。
- Epic 模板资源和 Unreal Engine 内容仍受 Epic EULA 约束，项目原创代码/插件代码若要开放复用，建议后续单独添加明确 License。

## Project Goals

The long-term goal is to turn the current Unreal template project into a standalone Imperial Tavern vertical slice:

- Build a data-driven RTS auto-battler framework.
- Support tavern/preparation and combat phase loops.
- Represent cards as persistent squad instances, not just UI objects.
- Add shared card pool logic, economy, tavern upgrades, triples, growth, and techs.
- Spawn combat units from player board state and resolve automated battles.
- Use Unreal MCP tools to accelerate safe editor automation, Blueprint scaffolding, UMG setup, and project inspection.

## Current Status

This is an early MVP platform, not a complete playable game yet.

The project currently contains:

- Unreal Engine 5.7 C++ project foundation.
- TopDown, Strategy, and TwinStick template content kept as reference/prototype material.
- `/Game/ImperialTavern` feature folder with initial MVP assets.
- A duplicated MVP strategy-style map for Imperial Tavern prototyping.
- Blueprint shell classes for core game framework, units, UI, civ abilities, and managers.
- Unreal MCP editor plugin with expanded AI-safe tool coverage.
- Git LFS setup for Unreal binary assets.

## Existing Imperial Tavern Assets

Main feature path:

```text
/Game/ImperialTavern
```

Current MVP assets include:

- `Maps/LVL_ImperialTavern_MVP`
- `Blueprints/Core/BP_IT_GameMode`
- `Blueprints/Core/BP_IT_GameState`
- `Blueprints/Core/BP_IT_PlayerState`
- `Blueprints/Core/BP_IT_PlayerController`
- `Blueprints/Core/BP_IT_AIController`
- `Blueprints/Core/BP_IT_BoardManager`
- `Blueprints/Core/BP_IT_ShopManager`
- `Blueprints/Core/BP_IT_CombatArena`
- `Blueprints/Units/BP_IT_UnitBase`
- `Blueprints/Units/BP_IT_CardActor`
- `Blueprints/Units/BP_IT_PlaceholderUnit`
- `Blueprints/Civ/BP_IT_CivAbilityBase`
- `Blueprints/UI/WBP_IT_HUD`
- `Blueprints/UI/WBP_IT_Shop`
- `Blueprints/UI/WBP_IT_Board`
- `Blueprints/UI/WBP_IT_EconomyBar`
- `Blueprints/UI/WBP_IT_Discover`

These are mostly scaffolding assets. The next major step is to fill them with authoritative gameplay logic.

## Unreal MCP Plugin

The project includes:

```text
Plugins/UnrealMcp
```

Unreal MCP runs inside the editor and exposes a local MCP endpoint:

```text
http://127.0.0.1:8765/mcp
```

The plugin also provides an in-editor chat window:

```text
Window > Unreal MCP Chat
```

See the plugin documentation for the full tool list:

```text
Plugins/UnrealMcp/README.md
```

### Major Tool Groups

Unreal MCP currently supports:

- Editor/project inspection.
- Map and asset listing.
- PIE start/stop.
- Log tailing and map checks.
- Actor selection, spawning, transforms, layout, and batch property edits.
- Python execution inside Unreal Editor.
- Blueprint asset creation and compilation.
- Blueprint graph editing tools:
  - `unreal.bp_add_variable`
  - `unreal.bp_add_function`
  - `unreal.bp_add_event_node`
  - `unreal.bp_add_call_function_node`
  - `unreal.bp_add_branch_node`
  - `unreal.bp_add_for_each_node`
  - `unreal.bp_connect_pins`
  - `unreal.bp_set_pin_default`
  - `unreal.bp_arrange_graph`
  - `unreal.bp_compile_save`
- UMG Widget Blueprint editing tools:
  - `unreal.widget_add`
  - `unreal.widget_remove`
  - `unreal.widget_set_property`
  - `unreal.widget_set_slot_layout`
  - `unreal.widget_bind_event`
  - `unreal.widget_bind_blueprint_variable`
  - `unreal.widget_build_template`
- Imperial Tavern gameplay scaffold tools:
  - `unreal.scaffold_round_system`
  - `unreal.scaffold_shop_system`
  - `unreal.scaffold_economy_system`
  - `unreal.scaffold_autobattler_ai`
  - `unreal.scaffold_result_ui`

## Target Gameplay Architecture

The intended Imperial Tavern architecture is:

### Data Layer

- Card definitions in data tables or data assets.
- Runtime card instances separated from static card definitions.
- Tech definitions and stat modifier data.
- Player board state separated into shop, stash, and field zones.

### Game Framework

- `GameMode`: server-only phase flow and match resolution.
- `GameState`: replicated global round/phase state and shared card pool.
- `PlayerState`: replicated health, food, gold, tavern level, streaks, and board state.
- `PlayerController`: UI requests and server-authoritative RPC entry points.

### Preparation Phase

- Refresh shop.
- Buy/sell cards.
- Move cards between shop, stash, and field.
- Upgrade tavern.
- Detect triples.
- Trigger discover rewards.

### Combat Phase

- Pair players.
- Spawn combat-only units from field cards.
- Apply tech, card-instance, and civilization modifiers.
- Run automated target selection and attacks.
- Resolve winner, survivor damage, and player health.

### Civilization Layer

- Civilization traits should be implemented through components or event-driven hooks.
- Example hooks:
  - round start
  - shop refresh
  - card bought
  - combat start
  - unit killed
  - combat resolved

## Roadmap

Near-term priorities:

- Define card, tech, board, and runtime card instance structs.
- Replace placeholder `Name` arrays with proper data structs.
- Implement server-authoritative economy actions.
- Implement shop refresh and shared pool sampling.
- Build a minimal drag/drop UMG board flow.
- Implement triple detection and discover rewards.
- Spawn basic combat units from field cards.
- Add simple auto-battler AI target selection and attacks.
- Add round result UI and health damage resolution.

Later priorities:

- Multiplayer replication hardening.
- Fast array replication for board/shop state.
- Civilization-specific passive abilities.
- Kill-growth and squad-growth persistence.
- Combat pooling or Mass Entity experiments for larger unit counts.
- Production UI pass.
- Save/load and lobby flow.

## Opening The Project

Requirements:

- Unreal Engine 5.7.
- Git LFS.
- macOS setup is currently the most tested path for this repository.

Clone and pull LFS assets:

```bash
git clone https://github.com/edwinmeng163-oss/UEMCP.git
cd UEMCP
git lfs install
git lfs pull
```

Open:

```text
MyProject.uproject
```

Recommended map:

```text
/Game/ImperialTavern/Maps/LVL_ImperialTavern_MVP
```

## Git / Repository Notes

This repository uses Git LFS for Unreal binary assets:

- `.uasset`
- `.umap`
- `.ubulk`
- `.uexp`

Generated Unreal folders are intentionally ignored:

- `Binaries/`
- `Intermediate/`
- `Saved/`
- `DerivedDataCache/`
- plugin build caches

Do not commit local API keys, editor user settings, or generated test assets.

## AI / API Key Safety

Unreal MCP can connect to the OpenAI Responses API from inside the editor. Configure API keys locally in:

```text
Project Settings > Plugins > Unreal MCP > AI
```

Do not commit API keys to Git. User-specific editor settings are ignored by `.gitignore`.

## License Notice

No standalone open-source license file has been added yet.

Important:

- Unreal Engine, Epic template assets, Starter Content, Mannequin assets, and other Epic-provided content remain governed by the Epic Unreal Engine EULA.
- Project-specific source code and original assets should receive a clear license before others rely on this repository for reuse.
- If this repository is intended to stay public, a common next step is to add an MIT license for original project/plugin code plus a notice excluding Epic/third-party content from that license.

## Repository

GitHub:

```text
https://github.com/edwinmeng163-oss/UEMCP
```
