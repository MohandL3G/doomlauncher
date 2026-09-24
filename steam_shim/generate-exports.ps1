# generate-exports.ps1 — regenerate the /export pragma block in steam_api64.c.
#
# Run this when a DOOM engine update changes which Steamworks functions are
# imported (symptom: the game fails to start with the shim installed and
# steam_shim.log shows nothing — the loader itself refuses to load the shim).
#
# Usage (x64 Native Tools / VS dev prompt, from the game's rerelease dir where
# the Valve DLL has been renamed to steam_api64_o.dll):
#   powershell -File generate-exports.ps1 -ValveDll steam_api64_o.dll -EngineExe doom.exe
#
# The script cross-references the engine's import names with the Valve DLL's
# export names, so the regenerated pragma block only forwards symbols that
# actually exist in the installed Valve DLL (a forward to a missing export
# breaks module load). It rewrites steam_api64.c in place: existing
# `#pragma comment(linker, "/export:...")` lines are removed and the new block
# is inserted just above `#define LAUNCHER_EXE_NAME`. Commit the regenerated
# file and re-run CI.
param(
    [Parameter(Mandatory = $true)] [string] $ValveDll,
    [Parameter(Mandatory = $true)] [string] $EngineExe
)

$ErrorActionPreference = "Stop"

function Get-PEImports([string] $Path) {
    # Minimal PE parser: DOS header -> NT headers -> import directory,
    # then walk IMAGE_IMPORT_DESCRIPTORs and their thunks.
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)

        $fs.Seek(0x3C, "Begin") | Out-Null
        $peOffset = $br.ReadInt32()
        $fs.Seek($peOffset, "Begin") | Out-Null
        if ($br.ReadUInt32() -ne 0x00004550) { throw "Not a PE file: $Path" }
        $br.ReadUInt16() | Out-Null                     # machine
        $numSections = $br.ReadUInt16()
        $fs.Seek(4, "Current") | Out-Null               # timestamp + ptrs to symtab/symcount + size of optional + chars
        $br.ReadUInt32() | Out-Null
        $br.ReadUInt32() | Out-Null
        $br.ReadUInt32() | Out-Null
        $optHeaderSize = $br.ReadUInt16()
        $br.ReadUInt16() | Out-Null
        $fs.Seek($optHeaderSize, "Current") | Out-Null

        $sections = @()
        for ($i = 0; $i -lt $numSections; $i++) {
            $br.ReadBytes(12) | Out-Null                # name + virtual size + virtual address
            $virtualAddress = $br.ReadUInt32()
            $rawSize = $br.ReadUInt32()
            $rawPointer = $br.ReadUInt32()
            $br.ReadBytes(16) | Out-Null
            $sections += [pscustomobject]@{ VA = $virtualAddress; RawSize = $rawSize; RawPtr = $rawPointer }
        }

        # For PE32+ the import dir RVA is at optional+120; PE32 at optional+96.
        $magicOffset = $peOffset + 4 + 20
        $fs.Seek($magicOffset, "Begin") | Out-Null
        $magic = $br.ReadUInt16()
        $dirRvaOffset = $peOffset + 4 + 20 + $(if ($magic -eq 0x20B) { 120 } else { 96 })
        $fs.Seek($dirRvaOffset, "Begin") | Out-Null
        $importRva = $br.ReadUInt32()

        function RvaToOffset([uint32] $rva, $sections) {
            foreach ($s in $sections) {
                if ($rva -ge $s.VA -and $rva -lt ($s.VA + $s.RawSize)) {
                    return $s.RawPtr + ($rva - $s.VA)
                }
            }
            return $null
        }

        $names = New-Object System.Collections.Generic.List[string]
        $descOff = RvaToOffset $importRva $sections
        if ($null -eq $descOff) { return $names }

        while ($true) {
            $fs.Seek($descOff, "Begin") | Out-Null
            $originalFirstThunk = $br.ReadUInt32()
            $nullTime = $br.ReadUInt32()
            $forwarder = $br.ReadUInt32()
            $nameRva = $br.ReadUInt32()
            $firstThunk = $br.ReadUInt32()
            if ($originalFirstThunk -eq 0 -and $firstThunk -eq 0) { break }

            $thunkRva = $(if ($originalFirstThunk -ne 0) { $originalFirstThunk } else { $firstThunk })
            $thunkOff = RvaToOffset $thunkRva $sections
            if ($null -eq $thunkOff) { $descOff += 20; continue }

            while ($true) {
                $fs.Seek($thunkOff, "Begin") | Out-Null
                $ordinal = $br.ReadUInt64()
                if ($ordinal -eq 0) { break }
                if (($ordinal -shr 63) -eq 0) {          # import by name
                    $nameOff = RvaToOffset ([uint32]($ordinal -band 0xFFFFFFFF)) $sections
                    if ($null -ne $nameOff) {
                        $fs.Seek($nameOff + 2, "Begin") | Out-Null   # skip Hint
                        $chars = New-Object System.Collections.Generic.List[byte]
                        while (($b = $br.ReadByte()) -ne 0) { $chars.Add($b) }
                        $names.Add([System.Text.Encoding]::ASCII.GetString($chars.ToArray()))
                    }
                }
                $thunkOff += 8
            }
            $descOff += 20
        }
        return $names
    }
    finally { $fs.Dispose() }
}

function Get-PEDllExports([string] $Path) {
    # Export directory walk: names array only.
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Seek(0x3C, "Begin") | Out-Null
        $peOffset = $br.ReadInt32()
        $fs.Seek($peOffset, "Begin") | Out-Null
        if ($br.ReadUInt32() -ne 0x00004550) { throw "Not a PE file: $Path" }
        $br.ReadUInt16() | Out-Null
        $numSections = $br.ReadUInt16()
        $br.ReadBytes(14) | Out-Null
        $optHeaderSize = $br.ReadUInt16()
        $br.ReadUInt16() | Out-Null
        $magicOffset = $peOffset + 4 + 20
        $fs.Seek($magicOffset, "Begin") | Out-Null
        $magic = $br.ReadUInt16()
        $dirRvaOffset = $peOffset + 4 + 20 + $(if ($magic -eq 0x20B) { 112 } else { 88 })
        $fs.Seek($dirRvaOffset, "Begin") | Out-Null
        $exportRva = $br.ReadUInt32()

        $sections = @()
        $sectionTable = $peOffset + 4 + 20 + $optHeaderSize
        for ($i = 0; $i -lt $numSections; $i++) {
            $fs.Seek($sectionTable + 40 * $i, "Begin") | Out-Null
            $br.ReadBytes(12) | Out-Null
            $virtualAddress = $br.ReadUInt32()
            $rawSize = $br.ReadUInt32()
            $rawPointer = $br.ReadUInt32()
            $sections += [pscustomobject]@{ VA = $virtualAddress; RawSize = $rawSize; RawPtr = $rawPointer }
        }

        function RvaToOffset([uint32] $rva, $sections) {
            foreach ($s in $sections) {
                if ($rva -ge $s.VA -and $rva -lt ($s.VA + $s.RawSize)) {
                    return $s.RawPtr + ($rva - $s.VA)
                }
            }
            return $null
        }

        $names = New-Object System.Collections.Generic.List[string]
        if ($exportRva -eq 0) { return $names }
        $expOff = RvaToOffset $exportRva $sections
        $fs.Seek($expOff + 24, "Begin") | Out-Null
        $numNames = $br.ReadUInt32()
        $namesRva = $br.ReadUInt32()
        $namesOff = RvaToOffset $namesRva $sections
        for ($i = 0; $i -lt $numNames; $i++) {
            $fs.Seek($namesOff + 4 * $i, "Begin") | Out-Null
            $nameRva = $br.ReadUInt32()
            $nameOff = RvaToOffset $nameRva $sections
            if ($null -eq $nameOff) { continue }
            $fs.Seek($nameOff, "Begin") | Out-Null
            $chars = New-Object System.Collections.Generic.List[byte]
            while (($b = $br.ReadByte()) -ne 0) { $chars.Add($b) }
            $names.Add([System.Text.Encoding]::ASCII.GetString($chars.ToArray()))
        }
        return $names
    }
    finally { $fs.Dispose() }
}

$engineNames = Get-PEImports $EngineExe
$valveExports = Get-PEDllExports $ValveDll

$steamEngine = $engineNames | Where-Object { $_ -match '^(SteamAPI_|SteamInternal_|SteamGameServer_|GetHSteam)' } | Sort-Object -Unique

$missing = @()
$forwards = @()
foreach ($name in $steamEngine) {
    if ($valveExports -contains $name) {
        $forwards += "#pragma comment(linker, `"/export:$name=steam_api64_o.$name`")"
    } else {
        $missing += $name
    }
}

if ($missing.Count -gt 0) {
    Write-Warning "Engine imports NOT present in Valve DLL (skipped, verify manually): $($missing -join ', ')"
}
if ($forwards.Count -eq 0) {
    throw "No forwardable symbols found — refusing to write an empty block."
}

# Splice into steam_api64.c: drop existing /export pragmas, insert the
# regenerated block right above the shim's first #define. LF line endings are
# preserved (WriteAllText with explicit joins, not WriteAllLines).
$sourcePath = Join-Path $PSScriptRoot "steam_api64.c"
$source = [System.IO.File]::ReadAllLines($sourcePath)
$kept = @($source | Where-Object { $_ -notmatch '^#pragma comment\(linker, "/export:' })

$defineIndex = -1
for ($i = 0; $i -lt $kept.Count; $i++) {
    if ($kept[$i] -like '#define LAUNCHER_EXE_NAME*') { $defineIndex = $i; break }
}
if ($defineIndex -lt 0) { throw "Could not find '#define LAUNCHER_EXE_NAME' in steam_api64.c" }

$newSource = @($kept[0..($defineIndex - 1)]) + $forwards + @("") + @($kept[$defineIndex..($kept.Count - 1)])
[System.IO.File]::WriteAllText($sourcePath, (($newSource -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Updated $sourcePath with $($forwards.Count) /export forwards."
if ($missing.Count -gt 0) {
    Write-Warning "Skipped $($missing.Count) engine import(s) missing from the Valve DLL (see warning above)."
}
