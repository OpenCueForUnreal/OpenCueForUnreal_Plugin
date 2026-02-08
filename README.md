# OpenCueForUnreal Plugin

[![English](https://img.shields.io/badge/Language-English-blue)](./README.md)
[![简体中文](https://img.shields.io/badge/Language-Simplified%20Chinese-red)](./README_CN.md)

Unreal Engine 5 plugin for submitting Movie Render Queue (MRQ) jobs to OpenCue and executing them through OpenCue workers.

## Scope and Status

- Current production path is **one-shot execution**:
  - UE submits to OpenCue.
  - RQD starts `opencue-ue-agent` per task.
  - Agent starts one `UnrealEditor-Cmd.exe` process per task.
- Persistent worker pool mode remains in code as an evolution path and is currently **TODO / not the validated mainline**.

## Runtime Topology

This plugin is only the UE-side integration. OpenCue infrastructure and execution services are external.

- UE plugin side:
  - Builds `render_plan.json` and `submit_spec.json`.
  - Calls submitter CLI with `submit --spec <submit_spec.json>`.
- OpenCue services side (`opencue-ue-services`):
  - `opencue-ue-submitter` submits jobs to Cuebot.
  - `opencue-ue-agent` executes tasks from RQD.
- OpenCue core:
  - Cuebot + PostgreSQL + RQD + CueGUI.

## Architecture and Sequence Diagrams

### Sequence
![Project Sequence](./Resources/ProjectSequence.svg)

### Architecture
![Project Architecture](./Resources/ProjectArchitecture.svg)

## UE Plugin Screenshots

### Movie Render Queue - OpenCue Panel
![MovieRenderQueue OpenCue](./Resources/MovieRenderQueue_OpenCueForUnreal.png)

### Project Settings - OpenCue
![OpenCue Settings](./Resources/Plugins%20-%20OpenCue%20Settings.png)

### CueGUI - Job Queue
![CueGUI Job Queue](./Resources/Job%20queue.png)

### CueGUI - Job Details
![CueGUI Job Details](./Resources/Cuegui%20-%20Job%20details.png)

### CueGUI - Job Finish
![CueGUI Job Finish](./Resources/Job%20finish.png)

## One-shot Flow (Current Mainline)

1. In MRQ, user submits an OpenCue job.
2. Plugin writes:
   - `Saved/OpenCueRenderPlans/<job_id>.json`
   - `Saved/OpenCueSubmitSpecs/<job_id>_submit_spec.json`
3. Plugin calls submitter CLI:
   - runtime mode: `opencue-ue-submitter.exe submit --spec ...`
   - developer mode: `python -m src.ue_submit submit --spec ...`
4. Cuebot dispatches tasks to RQD.
5. RQD runs:
   - `opencue-ue-agent.bat run-one-shot-plan --plan-path <render_plan_path>`
6. Agent resolves task index from `CUE_IFRAME` (fallback `CUE_FRAME`) and launches `UnrealEditor-Cmd.exe`.

## Submitter Selection (Developer vs Runtime)

Project Settings path: `Plugins -> OpenCue Settings`.

Resolution logic:

1. If `Python Path` is non-empty:
   - plugin uses developer mode (`python -m src.ue_submit ...`).
   - `Submitter Path` is treated as module root hint (directory containing `src/ue_submit`).
2. If `Python Path` is empty:
   - plugin uses runtime mode.
   - `Submitter Path` points to a directory or executable (`.exe/.bat/.cmd/.py`).
3. If `Submitter Path` is empty:
   - plugin auto-discovers bundled submitter executable in plugin directories.

Notes:

- `--spec` is always passed as an absolute path.
- Working directory is selected to match the chosen mode so relative imports and local resources resolve consistently.

## Command-line GameMode Resolution

For `-game` rendering, GameMode is resolved in this order:

1. MRQ OpenCue panel: `GameMode Override (-game mode)` (per job)
2. MRQ native `Game Overrides` setting: `GameModeOverride`
3. Selected map `WorldSettings.GameMode Override`
4. Project setting fallback:
   - `OpenCue Settings -> CommandLine Rendering -> Fallback GameMode Class (-game mode)`

Resolved value is applied via map option:

- `<MapAssetPath>?game=<ClassPath>`

## OpenCue Progress Semantics

OpenCue Job/Layer progress is task-completion based:

- `progress ~= succeeded_tasks / total_tasks`
- Task-internal render percent does not directly drive default CueGUI job/layer progress bars.

Implication for UE:

- One long task tends to look like `0% -> 100%` jump.
- Smoother OpenCue progress comes from task splitting (for example frame chunking), not from in-process percent logs alone.

## UE and OpenCue Naming Boundary

UI rule: keep UE-facing naming in UE terms; expose OpenCue terms only where required.

| UE/OpenCue Field | Meaning |
| --- | --- |
| MRQ Job row name | UE-local queue naming |
| OpenCue Job Name | Submitted OpenCue `job.name` |
| OpenCue Job Name default | MRQ Job row name, fallback to Sequence name |
| Show Name | OpenCue show/project domain for submission |

Users can submit from MRQ without learning OpenCue internals; mapping is handled by plugin + submitter.

## Module Layout

| Module | Type | Responsibility |
| --- | --- | --- |
| `OpenCueForUnreal` | Runtime | Core runtime integration |
| `OpenCueForUnrealUtils` | Runtime | Shared utilities and encoder helpers |
| `OpenCueForUnrealCmdline` | Runtime | Command-line executor for OpenCue one-shot rendering |
| `OpenCueForUnrealEditor` | Editor | MRQ job type, settings, details customization, submit actions |

## Requirements

- Unreal Engine 5.4+
- Movie Render Pipeline plugin enabled
- OpenCue backend running (`cuebot`, `rqd`, DB)
- `opencue-ue-services` available for submit/execute binaries and launch scripts

## Related Repositories

- [opencue-ue-services](https://github.com/OpenCueForUnreal/opencue-ue-services)
- [OpenCueForMRQ_Demo](https://github.com/OpenCueForUnreal/OpenCueForMRQ_Demo)
