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
// The only added behavior: when the game process (doom.exe) loads this DLL,
// DllMain duplicates a real handle to the engine's main thread, hands the work
// to a worker thread, and returns immediately. The worker SUSPENDS the main
// thread before the engine can reach main(), then starts DoomLauncher.exe from
// the same directory. The engine process therefore stays ALIVE for the whole
// UZDoom session — frozen: no window, no audio, no Steamworks calls, 0% CPU —
// which is what makes Steam count the session as DOOM playtime ("playing DOOM,
// but modded"). When DoomLauncher exits (it waits for UZDoom and performs the
// save backup), the worker terminates the frozen engine with the launcher's
// exit code, ending the Steam session at the right moment.
//
// If the main thread cannot be frozen safely (suspend failure, or it never
// leaves the loader walk within the attempt budget), the worker falls back to
// the old instant-kill behavior (TerminateProcess on self) rather than
// deadlocking or leaving the engine running alongside UZDoom.
//
// If DoomLauncher.exe or config.ini is missing, the shim does nothing and the game
// launches vanilla through the forwards — deleting the launcher files (or renaming
// this DLL away) is a complete uninstall.
//
// Build: CMake (see CMakeLists.txt) with MSVC x64. Links nothing beyond kernel32.
// Diagnostics: attach progress and launcher-spawn failures are appended (best
// effort, UTF-16) to steam_shim.log next to the game exe.
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

// Game server API (verified present; harmless if unused). Note: the modern
// Steamworks ABI exports SteamGameServer_InitSafe — there is no plain
// SteamGameServer_Init — so only the symbols that really exist are forwarded.
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

// Real handle to the engine's main thread, duplicated on that thread inside
// DllMain — GetCurrentThread() is only a pseudo-handle valid on the calling
// thread, so the duplication must happen here, not in the worker.
static HANDLE g_hMainThread = NULL;

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
    // ShimStrAppendW measures dst with ShimStrLenW, so the caller's buffer MUST
    // start empty — an uninitialized stack buffer scans as garbage length and
    // silently fails every join (this bug made the whole launcher path a no-op).
    outPath[0] = L'\0';
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

static DWORD WINAPI ShimWorkerThread(LPVOID arg);

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
        return; // Not a DoomLauncher deployment -> stay fully invisible.

    ShimLogW(dir, L"attach: shim loaded; DoomLauncher deployment detected.", 0);

    wchar_t exePath[MAX_PATH];
    if (!ShimJoinPathW(exePath, MAX_PATH, dir, LAUNCHER_EXE_NAME))
        return;
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES)
    {
        ShimLogW(dir, L"config.ini found but " LAUNCHER_EXE_NAME L" is missing; "
                      L"game continues without launcher.", 0);
        return;
    }

    // CreateMutexW does NOT reset the last error on success — only sets it to
    // ERROR_ALREADY_EXISTS when the mutex exists. Clear it first, or a stale
    // value from earlier loader activity is misread as "already exists" and
    // the launcher silently never starts.
    SetLastError(0);
    g_singleInstanceMutex = CreateMutexW(NULL, FALSE, SINGLE_INSTANCE_MUTEX);
    if (g_singleInstanceMutex != NULL &&
        GetLastError() == ERROR_ALREADY_EXISTS)
    {
        ShimLogW(dir, L"attach: launcher mutex already held; not starting a second "
                      L"DoomLauncher.", 0);
        return;
    }

    // Duplicate a REAL handle to the current (main) thread. GetCurrentThread()
    // returns a pseudo-handle that is only meaningful on the calling thread, so
    // this must happen here on the main thread — the worker cannot do it.
    HANDLE hMainThread = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &hMainThread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        ShimLogW(dir, L"Failed to duplicate main-thread handle; game continues vanilla.", GetLastError());
        return;
    }

    // Hand the freeze + launcher work to a worker thread so DllMain returns
    // immediately and the loader lock is released (never block inside DllMain).
    // The store below happens before CreateThread, so the worker observes it.
    g_hMainThread = hMainThread;
    HANDLE hWorker = CreateThread(NULL, 0, ShimWorkerThread, NULL, 0, NULL);
    if (hWorker == NULL)
    {
        // No worker = no freeze and no launcher; the engine would boot vanilla.
        ShimLogW(dir, L"Failed to create worker thread; game continues vanilla.", GetLastError());
        CloseHandle(hMainThread);
        g_hMainThread = NULL;
        return;
    }
    CloseHandle(hWorker); // we never join the worker; it lives for the session
}

// Worker thread: freezes the engine's main thread BEFORE it can run the engine
// (no window, no audio, no Steamworks calls, ~0% CPU), then runs DoomLauncher
// and keeps the engine process alive — suspended — for the whole UZDoom
// session so Steam accrues playtime. When the launcher exits (it waits for
// UZDoom and performs the save backup), the engine is terminated with the
// launcher's exit code, which ends the Steam session.
//
// Loader-lock rendezvous (why a plain SuspendThread is not enough): the main
// thread spends the first moments of its life inside the loader's DLL-init
// walk HOLDING the loader lock. If it is frozen there, every loader-lock
// operation from this worker (CreateProcessW among them) deadlocks forever —
// the lock owner can never run again. So each suspend is followed by a RIP
// check: if the main thread was caught inside ntdll (or anywhere outside the
// main exe image), it is resumed and the suspend retried. Only when RIP is
// inside the main exe image is the freeze kept — the walk is provably done
// (the exe entry point only runs after the loader releases the lock), so the
// engine is frozen at its entry code: before any window, audio, or Steamworks
// initialization.
static DWORD WINAPI ShimWorkerThread(LPVOID arg)
{
    (void)arg;

    wchar_t dir[MAX_PATH];
    if (!ShimSelfDirW((const void *)&ShimWorkerThread, dir, MAX_PATH))
    {
        // Purely defensive: this failure happens before the suspend, so the
        // engine would still boot vanilla without the launcher. Tear down
        // anyway so a frozen-hang is impossible from any path below.
        TerminateProcess(GetCurrentProcess(), 1);
        return 1;
    }

    // Freeze the main thread FIRST — before spawning anything. This is the only
    // instant at which the engine could start creating a window or audio, so
    // the freeze must land before the engine runs. See the rendezvous comment
    // above for why the first suspend usually lands mid loader-walk and must
    // not be kept: freezing the loader-lock owner deadlocks CreateProcessW.
    DWORD freezeErr = 0;
    BOOL frozen = FALSE;
    for (DWORD attempt = 0; attempt < 2000 && g_hMainThread != NULL; attempt++)
    {
        if (SuspendThread(g_hMainThread) == (DWORD)-1)
        {
            freezeErr = GetLastError();
            break; // cannot suspend at all -> fallback below
        }

        // Suspended: verify WHERE we caught the main thread before keeping it.
        CONTEXT ctx;
        SecureZeroMemory(&ctx, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        BOOL safe = FALSE;
        if (GetThreadContext(g_hMainThread, &ctx) && ctx.Rip != 0)
        {
            HMODULE hExe = GetModuleHandleW(NULL);
            MEMORY_BASIC_INFORMATION mbi;
            if (hExe != NULL &&
                VirtualQuery((LPCVOID)ctx.Rip, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.AllocationBase == hExe)
            {
                safe = TRUE; // inside the exe = loader walk done, lock free
            }
        }

        if (safe)
        {
            // Residual corner (accepted): if the exe registered TLS callbacks,
            // a freeze landing exactly inside one would still hold the loader
            // lock despite RIP being in the exe. The window is microseconds
            // wide and modern MSVC exes rarely use TLS callbacks, so this is
            // not worth more machinery.
            frozen = TRUE;
            break;
        }

        // Caught inside the loader walk (lock held): resume and retry shortly.
        ResumeThread(g_hMainThread);
        Sleep(1);
    }

    if (!frozen)
    {
        // Cannot freeze safely (suspend failure, or the main thread never left
        // the loader walk within the attempt budget): fall back to the previous
        // instant-kill behavior rather than deadlock or hang.
        ShimLogW(dir,
                 freezeErr != 0
                     ? L"Failed to suspend main thread; terminating engine (fallback)."
                     : L"Main thread never left the loader walk; terminating engine (fallback).",
                 freezeErr);
        TerminateProcess(GetCurrentProcess(), 0);
        return 1;
    }
    ShimLogW(dir, L"Main thread suspended; engine frozen at entry (loader lock free).", 0);

    wchar_t exePath[MAX_PATH];
    if (!ShimJoinPathW(exePath, MAX_PATH, dir, LAUNCHER_EXE_NAME))
    {
        // Main thread is already suspended here; returning would leave the
        // engine frozen forever.
        ShimLogW(dir, L"Internal error: failed to build launcher path; terminating engine.", 0);
        TerminateProcess(GetCurrentProcess(), 1);
        return 1;
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
        // Nothing preserves the session without the launcher; tear the frozen
        // engine down instead of leaving it suspended forever.
        TerminateProcess(GetCurrentProcess(), 1);
        return 1;
    }

    CloseHandle(pi.hThread);
    ShimLogW(dir, L"Started " LAUNCHER_EXE_NAME L"; engine stays suspended for the session.", 0);

    // Block until the launcher exits (DoomLauncher waits for UZDoom and syncs
    // saves before exiting). Keeping this process handle open also keeps the
    // worker alive for the whole session.
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD launcherExit = 0;
    GetExitCodeProcess(pi.hProcess, &launcherExit);
    CloseHandle(pi.hProcess);

    ShimLogW(dir, L"DoomLauncher exited; terminating suspended engine.", 0);
    // End the frozen engine with the launcher's exit code — Steam ends the
    // "DOOM" play session here, so playtime covers the real session length.
    TerminateProcess(GetCurrentProcess(), launcherExit);
    return 0;
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
        if (g_hMainThread != NULL)
            CloseHandle(g_hMainThread);
    }

    return TRUE;
}
