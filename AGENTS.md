# AGENTS.md

## Build

Cross-compile from Linux to Windows (`win-x64`) with the .NET 8 SDK directly — no Docker. Build box is a Debian 13 LXC (CT 101) on the PVE host, reached via `ssh pve` + `pct exec 101`. SDK 8 is at `/opt/dotnet` (symlinked into `/usr/local/bin`).

```sh
# Sync the working tree (incl. uncommitted changes) to the LXC
tar -cf doomlauncher_src.tar --exclude=.git --exclude=output -C <repo-root> .
scp doomlauncher_src.tar pve:/root/
ssh pve "pct push 101 /root/doomlauncher_src.tar /root/ && pct exec 101 -- bash -c 'mkdir -p /root/doomlauncher && tar -xf /root/doomlauncher_src.tar -C /root/doomlauncher'"

# Build
ssh pve "pct exec 101 -- bash -c 'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; cd /root/doomlauncher && dotnet publish DoomLauncher/DoomLauncher.csproj -c Release -o output'"

# Fetch the binary back
ssh pve "pct exec 101 -- cat /root/doomlauncher/output/DoomLauncher.exe > /root/DoomLauncher.exe"
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
- `DoomLauncher/DoomLauncher.csproj` — .NET 8, `WinExe`, `PublishSingleFile=true`, `SelfContained=false`
- `EnableWindowsTargeting` is `true` in the csproj — required for cross-compiling WinForms from Linux
- `config.ini` — User-edited config (do not overwrite)

## Key details

- .NET 8 Desktop Runtime required on target machine
- `pct exec 101` shells start with an empty `PATH` — export it explicitly before running dotnet
- `Application.EnableVisualStyles()` is required before showing `TaskDialog`
- Config file is read from `AppContext.BaseDirectory` (same dir as exe)
- Relative paths in config are resolved relative to the config file's directory
- Sync manifest is `doomlauncher_sync.manifest` in the backup dir; when missing, existing `.sav` files are auto-scanned
- Manifest lines carry a trailing mtime-tick field; `RestoreFromManifest` skips files whose backup mtime is unchanged since last sync (crash-safe — prevents stale backups overwriting newer local saves). Old manifests without the field always restore
- Saves are flattened with `.sav` extension for Steam Cloud compatibility (e.g. `doom.id.doom2.kex.save00.zds.sav`)
