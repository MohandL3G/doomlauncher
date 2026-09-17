using System;
using System.IO;

namespace DoomLauncher;

/// <summary>
/// Minimal append-only logger. Writes to doomlauncher.log next to the exe.
/// Never throws - logging failures must not affect the launcher's behavior.
/// </summary>
static class Logger
{
    private static string? _logPath;
    private const long MaxLogBytes = 1024 * 1024; // 1 MB, then rotate

    public static void Init(string exeDir)
    {
        _logPath = Path.Combine(exeDir, "doomlauncher.log");

        try
        {
            if (File.Exists(_logPath) && new FileInfo(_logPath).Length > MaxLogBytes)
            {
                string rotated = Path.Combine(exeDir, "doomlauncher.log.old");
                File.Copy(_logPath, rotated, true);
                File.Delete(_logPath);
            }
        }
        catch
        {
            // Rotation failing is not worth surfacing to the user.
        }
    }

    public static void Info(string message) => Write("INFO", message);

    public static void Warn(string message) => Write("WARN", message);

    public static void Error(string message) => Write("ERROR", message);

    private static void Write(string level, string message)
    {
        if (_logPath == null)
            return;

        try
        {
            string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} [{level}] {message}";
            File.AppendAllText(_logPath, line + Environment.NewLine);
        }
        catch
        {
            // Best-effort only - a locked or missing log file should never
            // stop the launcher from starting the game.
        }
    }
}
