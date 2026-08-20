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

## Building

Requires Docker. Run on the build machine:

```sh
docker compose build
```

The output binary will be in `output/`.
