# OpenCueForUnreal Plugin

[![English](https://img.shields.io/badge/Language-English-blue)](./README.md)
[![简体中文](https://img.shields.io/badge/Language-Simplified%20Chinese-red)](./README_CN.md)

用于将 Movie Render Queue (MRQ) 任务提交到 OpenCue 并通过 OpenCue Worker 执行的 Unreal Engine 5 插件。

## 范围与状态

- 当前生产主链路是 **one-shot 执行模式**：
  - UE 负责向 OpenCue 提交任务。
  - RQD 按 task 拉起 `opencue-ue-agent`。
  - agent 每个 task 拉起一个 `UnrealEditor-Cmd.exe` 进程。
- 常驻 worker pool 模式仍保留在代码中，当前标记为 **TODO / 非已验证主链路**。

## 运行拓扑

本插件仅负责 UE 侧集成，OpenCue 基础设施与执行服务在插件外部。

- UE 插件侧：
  - 生成 `render_plan.json` 与 `submit_spec.json`。
  - 调用 submitter CLI：`submit --spec <submit_spec.json>`。
- OpenCue 服务侧（`opencue-ue-services`）：
  - `opencue-ue-submitter` 负责提交到 Cuebot。
  - `opencue-ue-agent` 负责被 RQD 拉起后执行任务。
- OpenCue 核心：
  - Cuebot + PostgreSQL + RQD + CueGUI。

## 架构图与时序图

### 提交执行时序
![Project Sequence](./Resources/ProjectSequence.svg)

### 系统架构
![Project Architecture](./Resources/ProjectArchitecture.svg)

## UE 插件界面截图

### Movie Render Queue - OpenCue 面板
![MovieRenderQueue OpenCue](./Resources/MovieRenderQueue_OpenCueForUnreal.png)

### Project Settings - OpenCue
![OpenCue Settings](./Resources/Plugins%20-%20OpenCue%20Settings.png)

### CueGUI - Job Queue
![CueGUI Job Queue](./Resources/Job%20queue.png)

### CueGUI - Job Details
![CueGUI Job Details](./Resources/Cuegui%20-%20Job%20details.png)

### CueGUI - Job Finish
![CueGUI Job Finish](./Resources/Job%20finish.png)

## One-shot 流程（当前主链路）

1. 用户在 MRQ 中提交 OpenCue 任务。
2. 插件写出：
   - `Saved/OpenCueRenderPlans/<job_id>.json`
   - `Saved/OpenCueSubmitSpecs/<job_id>_submit_spec.json`
3. 插件调用 submitter CLI：
   - 运行时模式：`opencue-ue-submitter.exe submit --spec ...`
   - 开发者模式：`python -m src.ue_submit submit --spec ...`
4. Cuebot 将任务分发到 RQD。
5. RQD 执行：
   - `opencue-ue-agent.bat run-one-shot-plan --plan-path <render_plan_path>`
6. agent 从 `CUE_IFRAME`（回退 `CUE_FRAME`）解析 task 索引并拉起 `UnrealEditor-Cmd.exe`。

## Submitter 选择逻辑（开发者模式 vs 运行时模式）

项目设置路径：`Plugins -> OpenCue Settings`。

解析顺序：

1. 若 `Python Path` 非空：
   - 插件进入开发者模式（`python -m src.ue_submit ...`）。
   - `Submitter Path` 被视为模块根目录提示（应包含 `src/ue_submit`）。
2. 若 `Python Path` 为空：
   - 插件进入运行时模式。
   - `Submitter Path` 指向目录或可执行入口（`.exe/.bat/.cmd/.py`）。
3. 若 `Submitter Path` 也为空：
   - 插件会自动查找插件目录下打包的 submitter 可执行文件。

说明：

- `--spec` 一律使用绝对路径传入。
- Working directory 会根据当前模式选择，保证相对导入和资源解析稳定。

## Command-line GameMode 解析顺序

`-game` 渲染时，GameMode 按以下优先级解析：

1. MRQ OpenCue 面板：`GameMode Override (-game mode)`（每 Job）
2. MRQ 原生 `Game Overrides` 设置中的 `GameModeOverride`
3. 当前选中 Map 的 `WorldSettings.GameMode Override`
4. 项目级兜底设置：
   - `OpenCue Settings -> CommandLine Rendering -> Fallback GameMode Class (-game mode)`

最终以 map 参数形式应用：

- `<MapAssetPath>?game=<ClassPath>`

## OpenCue Progress 语义

OpenCue Job/Layer 的 progress 默认基于 task 完成度：

- `progress ~= succeeded_tasks / total_tasks`
- task 内部渲染百分比不会直接驱动 CueGUI 默认的 job/layer progress 条。

对 UE 的影响：

- 单个长 task 常见表现是 `0% -> 100%` 跳变。
- 若要让 OpenCue progress 更平滑，需要通过 task 拆分（例如 frame chunk）而不是仅依赖进程内日志百分比。

## UE 与 OpenCue 术语边界

原则：UE 面向用户的 UI 使用 UE 术语；OpenCue 术语只在必须处暴露。

| UE/OpenCue 字段 | 含义 |
| --- | --- |
| MRQ Job 行名称 | UE 本地队列命名 |
| OpenCue Job Name | 提交到 OpenCue 的 `job.name` |
| OpenCue Job Name 默认值 | 优先 MRQ Job 行名称，回退 Sequence 名称 |
| Show Name | OpenCue 里的 show / 渲染归属域 |

用户可在 MRQ 中直接提交，无需理解 OpenCue 内部实体，术语映射由插件和 submitter 完成。

## 模块结构

| 模块 | 类型 | 职责 |
| --- | --- | --- |
| `OpenCueForUnreal` | Runtime | 核心运行时集成 |
| `OpenCueForUnrealUtils` | Runtime | 公共工具与编码器辅助 |
| `OpenCueForUnrealCmdline` | Runtime | OpenCue one-shot 命令行执行器 |
| `OpenCueForUnrealEditor` | Editor | MRQ Job 类型、设置、详情定制、提交动作 |

## 依赖

- Unreal Engine 5.4+
- 启用 Movie Render Pipeline 插件
- OpenCue 后端运行中（`cuebot`、`rqd`、DB）
- 可用的 `opencue-ue-services`（提供提交/执行二进制与启动脚本）

## 相关仓库

- [opencue-ue-services](https://github.com/OpenCueForUnreal/opencue-ue-services)
- [OpenCueForMRQ_Demo](https://github.com/OpenCueForUnreal/OpenCueForMRQ_Demo)
