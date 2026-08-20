# AGENTS.md

## Build

This project cross-compiles from Linux (Docker on dietpi) to Windows (`win-x64`).

```sh
ssh dietpi "cd /mnt/dietpi_userdata/GitHub/doomlauncher && docker compose build --no-cache"
ssh dietpi "cd /mnt/dietpi_userdata/GitHub/doomlauncher && docker create --name extract doomlauncher:latest /bin/true && docker cp extract:/app/DoomLauncher.exe ./output/DoomLauncher.exe && docker rm extract"
```

Output: `output/DoomLauncher.exe`

## Deploy

Copy `DoomLauncher.exe` and `config.ini` to the rerelease directory:

```
C:\Program Files (x86)\Steam\steamapps\common\Ultimate Doom\rerelease\
```

## Project structure

- `DoomLauncher/Program.cs` — Entry point, reads config, launches UZDoom, handles sync
- `DoomLauncher/IniFile.cs` — Custom INI parser with `{ }` array support
- `DoomLauncher/SyncManager.cs` — Save/config backup and restore logic
- `DoomLauncher/DoomLauncher.csproj` — .NET 8, `WinExe`, `PublishSingleFile`, `SelfContained=false`
- `Dockerfile` — Multi-stage build: `sdk:8.0` build, `scratch` output
- `config.ini` — User-edited config (do not overwrite)

## Key details

- .NET 8 Desktop Runtime required on target machine
- `Application.EnableVisualStyles()` is required before showing `TaskDialog`
- Config file is read from `AppContext.BaseDirectory` (same dir as exe)
- Relative paths in config are resolved relative to the config file's directory
- Sync manifest is `doomlauncher_sync.manifest` in the backup dir; when missing, existing `.sav` files are auto-scanned
- Saves are flattened with `.sav` extension for Steam Cloud compatibility (e.g. `doom.id.doom2.kex.save00.zds.sav`)
