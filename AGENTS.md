# AGENTS.md

## Build

Cross-compile from Linux to Windows (`win-x64`) with the .NET 8 SDK directly — no Docker. Build box is a Debian 13 LXC (CT 122, hostname `building`) on the PVE host, reached via `ssh pve` + `pct exec 122`. SDK 8.0.425 at `/opt/dotnet` (symlinked into `/usr/local/bin`).

```sh
# Sync the working tree (incl. uncommitted changes) to the LXC
tar -cf doomlauncher_src.tar --exclude=.git --exclude=output -C <repo-root> .
scp doomlauncher_src.tar pve:/root/
ssh pve "pct push 122 /root/doomlauncher_src.tar /root/doomlauncher_src.tar && pct exec 122 -- bash -c 'mkdir -p /root/doomlauncher && tar -xf /root/doomlauncher_src.tar -C /root/doomlauncher'"

# Build
ssh pve "pct exec 122 -- bash -c 'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; cd /root/doomlauncher && dotnet publish DoomLauncher/DoomLauncher.csproj -c Release -o output'"

# Fetch the binary back
ssh pve "pct pull 122 /root/doomlauncher/output/DoomLauncher.exe /root/DoomLauncher.exe"
scp pve:/root/DoomLauncher.exe output/
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
- `DoomLauncher/Logger.cs` — Best-effort append-only log at `doomlauncher.log` next to the exe (rotates to `.old` at 1 MB)
- `DoomLauncher/DoomLauncher.csproj` — .NET 8, `WinExe`, `PublishSingleFile=true`, `SelfContained=false`
- `EnableWindowsTargeting` is `true` in the csproj — required for cross-compiling WinForms from Linux
- `config.ini` — User-edited config (do not overwrite)

## Key details

- .NET 8 Desktop Runtime required on target machine
- `pct exec 122` shells start with an empty `PATH` — export it explicitly before running dotnet
- `pct push` destination must be a full file path (trailing dir throws "Is a directory")
- `Application.EnableVisualStyles()` is required before showing `TaskDialog`
- Config file is read from `AppContext.BaseDirectory` (same dir as exe)
- Relative paths in config are resolved relative to the config file's directory
- Sync manifest is `doomlauncher_sync.manifest` in the backup dir; when missing, existing `.sav` files are auto-scanned
- Manifest fields are tab-delimited (`type\tbackupFile\toriginalPath\tmtime`) so filenames containing spaces parse correctly; the legacy space-delimited format (pre-fix) is still read for manifests written by older builds
- Manifest lines carry a trailing mtime-tick field; `RestoreFromManifest` skips files whose backup mtime is unchanged since last sync (crash-safe — prevents stale backups overwriting newer local saves). Old manifests without the field always restore
- `Backup` removes `.sav` files in the backup dir that no longer correspond to a local save/config (prevents unbounded growth in the Steam Cloud-synced folder)
- Saves are flattened with `.sav` extension for Steam Cloud compatibility (e.g. `doom.id.doom2.kex.save00.zds.sav`)
- Restore and Backup each have their own try/catch in `Program.cs`, separate from the launch try/catch, so a sync failure is never reported as a launch failure and never blocks the game from starting or its exit code from being returned
