# AGENTS.md

## Build

GitHub Actions (`.github/workflows/build.yml`) builds both components:

- **Shim** — `windows-latest`, CMake + MSVC x64: `cmake -S steam_shim -B build -A x64 && cmake --build build --config Release --target steam_api64`. Artifact `steam_api64-shim`, also attached to the rolling `shim-latest` pre-release.
- **Launcher** — `ubuntu-latest`, .NET 8 SDK: `dotnet publish DoomLauncher/DoomLauncher.csproj -c Release -o output`. Artifact `DoomLauncher`, same release.

The historical build LXC (CT 122) is destroyed; do not reference it. Releases are public, so fresh artifacts are fetchable locally without auth:
`https://github.com/MohandL3G/doomlauncher/releases/latest/download/steam_api64.dll` — NOTE: this resolves to the rolling `shim-latest` release only while it is the most recent release; after any versioned release, pin the tag explicitly.

## Deploy

Game dir: `C:\Program Files (x86)\Steam\steamapps\common\Ultimate Doom\rerelease\`

- `doom.exe` — ORIGINAL engine exe (verify vs `doom.exe.old`; never a launcher hardlink)
- `steam_api64.dll` — the shim (hash must match the CI artifact)
- `steam_api64_o.dll` — Valve's original Steamworks DLL, renamed
- `steam_api64.dll.orig` — untouched copy of Valve's DLL (rollback)
- `DoomLauncher.exe` + `config.ini` — launcher, as before

Shim debug log: `steam_shim.log` next to the game exe (written only on launcher-spawn failures/successes).

## Project structure

- `DoomLauncher/Program.cs` — Entry point, reads config, launches UZDoom, handles sync
- `DoomLauncher/IniFile.cs` — Custom INI parser with `{ }` array support
- `DoomLauncher/SyncManager.cs` — Save/config backup and restore logic
- `DoomLauncher/Logger.cs` — Best-effort append-only log at `doomlauncher.log` next to the exe (rotates to `.old` at 1 MB)
- `DoomLauncher/DoomLauncher.csproj` — .NET 8, `WinExe`, `PublishSingleFile=true`, `SelfContained=false`
- `steam_shim/steam_api64.c` — Proxy shim: spawns `DoomLauncher.exe` on DLL_PROCESS_ATTACH; exports are linker-level forwarders via `#pragma comment(linker, "/export:<name>=steam_api64_o.<name>")` directives (no forwarding code)
- `steam_shim/generate-exports.ps1` — Regenerates the `/export` pragma block in steam_api64.c from a Valve DLL + engine exe (run after engine updates)
- `config.ini` — User-edited config (do not overwrite)

## Key details

- The shim must NEVER hook or wrap Steamworks; forwarding is linker export-forwarding to `steam_api64_o.dll`. Nothing Valve-signed is modified.
- Forwarder entries must only reference exports that exist in the INSTALLED Valve DLL (`SteamAPI_InitEx`/`InitFlat`/`ManualDispatch_GetNextEvent` are NOT present in the current one — verified). A forward to a missing export breaks module load.
- MSVC's newer def-file parser (VS 18 2026 toolset) rejects def-file forwarders with LNK2001 ×N — that's why forwarding uses `/export` linker pragmas in the C source instead of a .def file (do not reintroduce one).
- Engine imports exactly 11 Steamworks symbols (verified from `doom.exe.old` import table); the def covers those plus verified-safe extras.
- Shim code is CRT-free in DllMain (kernel32 only, stack buffers, no thread-attach work) — loader-lock safe. A single-instance mutex (`Local\DoomLauncherShimMutex`) prevents double launcher spawns.
- If `config.ini` is missing the shim stays silent (game runs vanilla); if `config.ini` exists but `DoomLauncher.exe` is missing it logs and continues.
- .NET 8 Desktop Runtime required on target machine for the launcher
- `Application.EnableVisualStyles()` is required before showing `TaskDialog`
- Config file is read from `AppContext.BaseDirectory` (same dir as exe); relative config paths resolve against the config file's directory
- Sync manifest is `doomlauncher_sync.manifest` in the backup dir; when missing, existing `.sav` files are auto-scanned
- Manifest fields are tab-delimited (`type\tbackupFile\toriginalPath\tmtime`) so filenames containing spaces parse correctly; the legacy space-delimited format (pre-fix) is still read for manifests written by older builds
- Manifest lines carry a trailing mtime-tick field; `RestoreFromManifest` skips files whose backup mtime is unchanged since last sync (crash-safe — prevents stale backups overwriting newer local saves). Old manifests without the field always restore
- `Backup` removes `.sav` files in the backup dir that no longer correspond to a local save/config (prevents unbounded growth in the Steam Cloud-synced folder)
- Saves are flattened with `.sav` extension for Steam Cloud compatibility (e.g. `doom.id.doom2.kex.save00.zds.sav`)
- Restore and Backup each have their own try/catch in `Program.cs`, separate from the launch try/catch, so a sync failure is never reported as a launch failure and never blocks the game from starting or its exit code from being returned
- Steam may re-download `doom.exe`/`steam_api64.dll` on verify/update, wiping shim+rename; recovery = re-copy shim + rename `steam_api64_o.dll` (2 files, documented in README)
