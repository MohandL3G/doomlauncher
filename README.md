# DoomLauncher

A lightweight launcher that starts [UZDoom](https://github.com/UZDoom/UZDoom) with configurable launch options, reading settings from a `config.ini` file. Supports Steam Cloud save syncing with the Doom re-release via piggybacking on its existing save folder.

## Requirements

- [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/en-us/download/dotnet/8.0) (x64)

## Usage

1. Place `DoomLauncher.exe` and `config.ini` in your game directory
2. Edit `config.ini` with your paths
3. Run `DoomLauncher.exe` (or add it as a Steam launch option)

On first run, if `config.ini` is missing, a default one is created and opened in Notepad.

## Config

```ini
[Launch]
ExePath=Mods\UZDoom\uzdoom.exe
WorkDir=Mods\UZDoom
IWAD=doom2.wad
Mods={
    Mods\Bloom\Bloom.pk3
}
ConfigFile=
ExtraArgs=

[Sync]
Enabled=false
BackupDir=
UzDoomSaveDir=
UzDoomConfigDir=
```

### [Launch]

| Key | Description |
|---|---|
| `ExePath` | Path to `uzdoom.exe` (relative to config or absolute) |
| `WorkDir` | Working directory for UZDoom (where its `.pk3` files are) |
| `IWAD` | Path to the IWAD (`doom.wad`, `doom2.wad`, etc.) |
| `Mods` | Mod/PWAD files to load, one per line inside `{ }` |
| `ConfigFile` | Optional custom UZDoom config file (relative to `UzDoomConfigDir` or absolute). If set but missing, you'll be prompted to copy from default or use UZDoom's default |
| `ExtraArgs` | Additional command line arguments |

### [Sync]

Syncs UZDoom saves and configs to the Doom re-release's save folder, which is already synced to Steam Cloud (App ID 228980).

| Key | Description |
|---|---|
| `Enabled` | Set to `true` to enable save syncing |
| `BackupDir` | The rerelease's saves folder (e.g. `Saved Games\Nightdive Studios\DOOM\saves`) |
| `UzDoomSaveDir` | UZDoom's save directory (e.g. `Saved Games\UZDoom`) |
| `UzDoomConfigDir` | UZDoom's config directory (e.g. `Documents\My Games\UZDoom`) |

How it works:
- **Before launch:** Restores saves/configs from the backup dir into UZDoom's directories
- **After exit:** Backs up saves/configs from UZDoom's directories into the backup dir
- On first run (or on a new PC), existing `.sav` files are auto-detected and a manifest is generated
- Only files managed by DoomLauncher are touched; the re-release's own saves are never modified

All paths are resolved relative to the directory containing `DoomLauncher.exe`.

## Steam integration (steam_api64.dll shim)

Instead of hijacking `doom.exe` with a hardlink, the repo ships a tiny proxy `steam_api64.dll` (see `steam_shim/`). When the real engine loads its Steamworks DLL, the shim forwards every Steamworks export to the original Valve DLL (renamed to `steam_api64_o.dll`) and starts `DoomLauncher.exe` from the same folder.

Steam's Play button therefore launches UZDoom through DoomLauncher, while the engine process keeps a fully functional (unmodified, forwarded) Steamworks: overlay, achievements, Steam Cloud and play-state all behave normally. If the shim or launcher files are removed, the game simply runs vanilla.

The shim hooks nothing — the forwarding is emitted by the linker as PE export-forwarding records from `#pragma comment(linker, "/export:...")` directives in `steam_api64.c`, so there is zero Steamworks-altering code and no def file involved. If a DOOM engine update changes its import table, regenerate the pragma block with `steam_shim/generate-exports.ps1` (see the script header) and rebuild.

### Install (game rerelease folder)

1. Copy the original Valve `steam_api64.dll` to `steam_api64_o.dll` (and keep a pristine copy as `steam_api64.dll.orig` for rollback).
2. Copy the shim build's `steam_api64.dll` into the folder.
3. Keep `DoomLauncher.exe` + `config.ini` next to it (as before).
4. `doom.exe` must be the ORIGINAL engine exe (no hardlink needed anymore).

### Uninstall / rollback

Delete the shim `steam_api64.dll`, rename `steam_api64_o.dll` (or `steam_api64.dll.orig`) back to `steam_api64.dll`. Nothing else changes.

## Building

GitHub Actions (`.github/workflows/build.yml`) builds both components on every push touching them and attaches them to the rolling `shim-latest` pre-release:

- `steam_api64.dll` — MSVC x64 shim (windows-latest, CMake)
- `DoomLauncher.exe` — .NET 8 win-x64 single-file publish (ubuntu-latest)

Locally, the launcher still builds with the .NET 8 SDK:

```sh
dotnet publish DoomLauncher/DoomLauncher.csproj -c Release -o output
```

The shim needs MSVC (forwarders use MSVC `/export` linker pragmas): `cmake -S steam_shim -B build -A x64 && cmake --build build --config Release`.
