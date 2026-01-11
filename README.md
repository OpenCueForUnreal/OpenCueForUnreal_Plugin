# OpenCueForUnreal Plugin

UE5 plugin for Movie Render Queue automation with OpenCue integration.

## Features

- **Persistent Worker Mode** - Keep UE running and poll for render tasks via HTTP
- **One-shot Mode** - Command-line rendering that exits after completion
- **Custom Encoder** - FFMPEG encoding with progress tracking
- **Scene Sync** - Wait for level data sync before rendering

## Modules

| Module | Type | Description |
|--------|------|-------------|
| OpenCueForUnreal | Runtime | Core module |
| OpenCueForUnrealUtils | Runtime | Custom encoder and utilities |
| OpenCueForUnrealCmdline | Runtime | Deferred executor for `-game` mode |
| OpenCueForUnrealEditor | Editor | PIE executor and worker subsystem |

## Installation

Clone into your project's `Plugins` folder:

```bash
cd YourProject/Plugins
git clone https://github.com/OpenCueForUnreal/OpenCueForUnreal_Plugin.git OpenCueForUnreal
```

Or add as submodule:

```bash
git submodule add https://github.com/OpenCueForUnreal/OpenCueForUnreal_Plugin.git Plugins/OpenCueForUnreal
```

## Requirements

- Unreal Engine 5.4+
- Movie Render Pipeline plugin (included with engine)

## Related Repositories

- [opencue-ue-services](https://github.com/OpenCueForUnreal/opencue-ue-services) - Python Worker Pool Service
- [OpenCueForMRQ_Demo](https://github.com/OpenCueForUnreal/OpenCueForMRQ_Demo) - Demo Project
