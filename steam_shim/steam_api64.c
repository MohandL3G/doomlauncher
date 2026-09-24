// steam_api64.c — DoomLauncher proxy shim for the DOOM (2024 kex engine) rerelease.
//
// Inspired by the proxy-DLL technique used by SmokeAPI / CloudRedirect / BetterSteamTools,
// but much smaller: this DLL does NOT hook or alter any Steamworks behavior. Every
// Steamworks export is forwarded 1:1 to the original Valve DLL, which must be placed
// next to this file renamed as `steam_api64_o.dll` (SmokeAPI's naming convention).
// The forwarding itself is done by the linker via export-forwarding records
// emitted from the `#pragma comment(linker, "/export:...")` directives below —
// there is no forwarding code here and no hooking anywhere, so the
// Steam overlay, achievements, cloud saves, and ownership checks all behave exactly
// as with the original DLL.
//
// The only added behavior: when the game process (doom.exe) loads this DLL, DllMain
// starts DoomLauncher.exe from the same directory. DoomLauncher reads config.ini,
// shows its usual dialogs, syncs saves, launches UZDoom, and waits for it. The
// engine process keeps running underneath with fully functional (forwarded)
// Steamworks, which keeps Steam's play-state, rich presence, and overlay sane.
//
// If DoomLauncher.exe or config.ini is missing, the shim does nothing and the game
// launches vanilla through the forwards — deleting the launcher files (or renaming
// this DLL away) is a complete uninstall.
//
// Build: CMake (see CMakeLists.txt) with MSVC x64. Links nothing beyond kernel32.
// Diagnostics: failures while spawning the launcher are appended (best effort,
// UTF-16) to steam_shim.log next to the game exe.
//
// Forwarding mechanism: `#pragma comment(linker, "/export:...")` directives
// below — the documented /export:name=otherdll.name forwarder form (the same
// technique the classic proxy-DLL generators use). A .def file was tried first
// and rejected by MSVC's newer def parser, which treated every forwarder as a
// local symbol to implement (LNK2001 x19); pragma exports go through the same
// linker flag but are parsed as raw linker options, so they always work.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// --- Export forwarding --------------------------------------------------------
// Every Steamworks symbol the engine (or legacy tooling) may import is exported
// from this DLL as a forwarder record to steam_api64_o.dll. The linker creates
// pure PE export-forwarding entries (no code, no CRT, no load-time dependency
// beyond the name). Keep in sync with the installed Valve DLL — see
// generate-exports.ps1. A forward to a symbol MISSING from steam_api64_o.dll
// breaks module load at start-up, so only verified-present symbols are listed.

#pragma comment(linker, "/export:SteamAPI_Init=steam_api64_o.SteamAPI_Init")
#pragma comment(linker, "/export:SteamAPI_RestartAppIfNecessary=steam_api64_o.SteamAPI_RestartAppIfNecessary")
#pragma comment(linker, "/export:SteamAPI_RegisterCallback=steam_api64_o.SteamAPI_RegisterCallback")
#pragma comment(linker, "/export:SteamAPI_UnregisterCallback=steam_api64_o.SteamAPI_UnregisterCallback")
#pragma comment(linker, "/export:SteamAPI_RegisterCallResult=steam_api64_o.SteamAPI_RegisterCallResult")
#pragma comment(linker, "/export:SteamAPI_UnregisterCallResult=steam_api64_o.SteamAPI_UnregisterCallResult")
#pragma comment(linker, "/export:SteamAPI_RunCallbacks=steam_api64_o.SteamAPI_RunCallbacks")
#pragma comment(linker, "/export:SteamAPI_Shutdown=steam_api64_o.SteamAPI_Shutdown")
#pragma comment(linker, "/export:SteamAPI_GetHSteamUser=steam_api64_o.SteamAPI_GetHSteamUser")
#pragma comment(linker, "/export:SteamAPI_IsSteamRunning=steam_api64_o.SteamAPI_IsSteamRunning")
#pragma comment(linker, "/export:SteamAPI_ManualDispatch_Init=steam_api64_o.SteamAPI_ManualDispatch_Init")
#pragma comment(linker, "/export:SteamAPI_ManualDispatch_RunFrame=steam_api64_o.SteamAPI_ManualDispatch_RunFrame")
#pragma comment(linker, "/export:SteamInternal_ContextInit=steam_api64_o.SteamInternal_ContextInit")
#pragma comment(linker, "/export:SteamInternal_FindOrCreateUserInterface=steam_api64_o.SteamInternal_FindOrCreateUserInterface")

// Bare HSteam handles (legacy ABI, verified present in the installed DLL)
#pragma comment(linker, "/export:GetHSteamUser=steam_api64_o.GetHSteamUser")
#pragma comment(linker, "/export:GetHSteamPipe=steam_api64_o.GetHSteamPipe")

// Game server API (verified present; harmless if unused)
#pragma comment(linker, "/export:SteamGameServer_Init=steam_api64_o.SteamGameServer_Init")
#pragma comment(linker, "/export:SteamGameServer_Shutdown=steam_api64_o.SteamGameServer_Shutdown")
#pragma comment(linker, "/export:SteamGameServer_RunCallbacks=steam_api64_o.SteamGameServer_RunCallbacks")

// -----------------------------------------------------------------------------

#define LAUNCHER_EXE_NAME L"DoomLauncher.exe"
#define LAUNCHER_CONFIG_NAME L"config.ini"
#define LOG_FILE_NAME L"steam_shim.log"
#define SINGLE_INSTANCE_MUTEX L"Local\\DoomLauncherShimMutex"

// One launcher per game process at a time. If two doom.exe processes start within
// the same instant, only the first spawns DoomLauncher (prevents double UZDoom
// launches and concurrent save-sync runs).
static HANDLE g_singleInstanceMutex = NULL;

// CRT-free helpers: DllMain runs under the loader lock, so keep every operation
// down to kernel32 calls on fixed-size stack buffers.

static DWORD ShimStrLenW(const wchar_t *s)
{
    DWORD n = 0;
    while (s[n] != L'\0')
        n++;
    return n;
}

// dst must hold srcLen+1 wchar_t.
static void ShimStrCopyW(wchar_t *dst, const wchar_t *src)
{
    while ((*dst++ = *src++) != L'\0')
        ;
}

// Appends src to dst. dstSize is the full capacity of dst in wchar_t.
static BOOL ShimStrAppendW(wchar_t *dst, DWORD dstSize, const wchar_t *src)
{
    DWORD dstLen = ShimStrLenW(dst);
    DWORD srcLen = ShimStrLenW(src);
    if (dstLen + srcLen + 1 > dstSize)
        return FALSE;
    ShimStrCopyW(dst + dstLen, src);
    return TRUE;
}

// Directory of the module containing `marker` (i.e. this DLL's own directory,
// which is also the game exe's directory in a standard deployment).
static BOOL ShimSelfDirW(const void *marker, wchar_t *outDir, DWORD outDirCch)
{
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)marker, &self))
        return FALSE;

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return FALSE;

    wchar_t *lastSlash = NULL;
    for (DWORD i = 0; i < n; i++)
    {
        if (path[i] == L'\\')
            lastSlash = &path[i];
    }
    if (lastSlash == NULL)
        return FALSE;

    DWORD dirLen = (DWORD)(lastSlash - path);
    if (dirLen == 0 || dirLen + 1 > outDirCch)
        return FALSE;

    for (DWORD i = 0; i < dirLen; i++)
        outDir[i] = path[i];
    outDir[dirLen] = L'\0';
    return TRUE;
}

// outPath = dir + "\\" + fileName (outPathCch = capacity in wchar_t).
static BOOL ShimJoinPathW(wchar_t *outPath, DWORD outPathCch,
                          const wchar_t *dir, const wchar_t *fileName)
{
    if (!ShimStrAppendW(outPath, outPathCch, dir))
        return FALSE;
    if (!ShimStrAppendW(outPath, outPathCch, L"\\"))
        return FALSE;
    return ShimStrAppendW(outPath, outPathCch, fileName);
}

// Best-effort UTF-16 log. Never blocks launch on failure.
static void ShimLogW(const wchar_t *dir, const wchar_t *msg1, DWORD error)
{
    wchar_t logPath[MAX_PATH];
    if (!ShimJoinPathW(logPath, MAX_PATH, dir, LOG_FILE_NAME))
        return;

    HANDLE h = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    wchar_t line[MAX_PATH + 64];
    ShimStrCopyW(line, L"[steam_shim] ");
    if (!ShimStrAppendW(line, MAX_PATH + 64, msg1))
        line[0] = L'\0';

    wchar_t code[16];
    if (error != 0)
    {
        // Manual u32->dec to stay CRT-free.
        wchar_t *p = code + 15;
        *p = L'\0';
        do
        {
            *--p = (wchar_t)(L'0' + (error % 10));
            error /= 10;
        } while (error != 0 && p > code);
        ShimStrAppendW(line, MAX_PATH + 64, L" (error ");
        ShimStrAppendW(line, MAX_PATH + 64, p);
        ShimStrAppendW(line, MAX_PATH + 64, L")");
    }
    ShimStrAppendW(line, MAX_PATH + 64, L"\r\n");

    DWORD written = 0;
    WriteFile(h, line, (DWORD)(ShimStrLenW(line) * sizeof(wchar_t)), &written, NULL);
    CloseHandle(h);
}

static void ShimStartLauncher(void)
{
    wchar_t dir[MAX_PATH];
    if (!ShimSelfDirW((const void *)&ShimStartLauncher, dir, MAX_PATH))
        return;

    // Not a DoomLauncher deployment -> stay fully invisible.
    wchar_t configPath[MAX_PATH];
    if (!ShimJoinPathW(configPath, MAX_PATH, dir, LAUNCHER_CONFIG_NAME))
        return;
    if (GetFileAttributesW(configPath) == INVALID_FILE_ATTRIBUTES)
        return;

    wchar_t exePath[MAX_PATH];
    if (!ShimJoinPathW(exePath, MAX_PATH, dir, LAUNCHER_EXE_NAME))
        return;
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES)
    {
        ShimLogW(dir, L"config.ini found but " LAUNCHER_EXE_NAME L" is missing; "
                      L"game continues without launcher.", 0);
        return;
    }

    g_singleInstanceMutex = CreateMutexW(NULL, FALSE, SINGLE_INSTANCE_MUTEX);
    if (g_singleInstanceMutex != NULL &&
        GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // A launcher is already managing this game session.
        return;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SecureZeroMemory(&si, sizeof(si));
    SecureZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    // CreateProcessW may modify its command line buffer; pass a writable copy.
    // No arguments: DoomLauncher reads everything from config.ini.
    wchar_t cmdLine[MAX_PATH];
    ShimStrCopyW(cmdLine, L"\"");
    ShimStrAppendW(cmdLine, MAX_PATH, exePath);
    ShimStrAppendW(cmdLine, MAX_PATH, L"\"");

    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi))
    {
        ShimLogW(dir, L"Failed to start " LAUNCHER_EXE_NAME, GetLastError());
        return;
    }

    // The launcher outlives us only as a separate process; we hold no handles
    // beyond these two, which we can drop immediately.
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ShimLogW(dir, L"Started " LAUNCHER_EXE_NAME, 0);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);
        ShimStartLauncher();
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        if (g_singleInstanceMutex != NULL)
            CloseHandle(g_singleInstanceMutex);
    }

    return TRUE;
}
