using System;
using System.Collections.Generic;
using System.IO;

namespace DoomLauncher;

class SyncManager
{
    private const string ManifestName = "doomlauncher_sync.manifest";

    // Tab can't appear in a Windows filename, so it's a safe delimiter for
    // manifest fields even when a save/config filename contains spaces.
    private const char Delim = '\t';

    public static void Restore(string backupDir, string saveDir, string configDir)
    {
        Directory.CreateDirectory(saveDir);
        Directory.CreateDirectory(configDir);

        string manifestPath = Path.Combine(backupDir, ManifestName);
        if (File.Exists(manifestPath))
        {
            RestoreFromManifest(manifestPath, backupDir, saveDir, configDir);
        }
        else
        {
            Logger.Info("No manifest found - falling back to filename scan for restore.");
            RestoreFromScan(backupDir, saveDir, configDir);
        }
    }

    private static void RestoreFromManifest(string manifestPath, string backupDir, string saveDir, string configDir)
    {
        int restored = 0, skipped = 0, missing = 0;

        foreach (string rawLine in File.ReadLines(manifestPath))
        {
            string line = rawLine.Trim();
            if (line.Length < 3)
                continue;

            if (!TryParseManifestLine(line, out char type, out string backupFile, out string originalPath, out bool hasMtime, out long recordedMtime))
            {
                Logger.Warn($"Skipping unparseable manifest line: {line}");
                continue;
            }

            string srcPath = Path.Combine(backupDir, backupFile);
            if (!File.Exists(srcPath))
            {
                missing++;
                continue;
            }

            if (hasMtime && File.GetLastWriteTimeUtc(srcPath).Ticks == recordedMtime)
            {
                skipped++;
                continue;
            }

            string destDir = type == 'c' ? configDir : saveDir;
            string destPath = Path.Combine(destDir, originalPath.Replace('/', '\\'));
            string? destParent = Path.GetDirectoryName(destPath);

            if (destParent != null && !Directory.Exists(destParent))
                Directory.CreateDirectory(destParent);

            try
            {
                File.Copy(srcPath, destPath, true);
                restored++;
            }
            catch (Exception ex)
            {
                Logger.Warn($"Failed to restore '{originalPath}': {ex.Message}");
            }
        }

        Logger.Info($"Restore complete: {restored} restored, {skipped} unchanged, {missing} missing from backup.");
    }

    /// <summary>
    /// Parses one manifest line. Supports the current tab-delimited format
    /// ("type\tbackupFile\toriginalPath\tmtime") and falls back to the
    /// legacy space-delimited format for manifests written by older builds
    /// (which breaks on filenames containing spaces - that's exactly why
    /// the format changed, but old manifests still need to be readable).
    /// </summary>
    private static bool TryParseManifestLine(string line, out char type, out string backupFile, out string originalPath, out bool hasMtime, out long recordedMtime)
    {
        type = '\0';
        backupFile = "";
        originalPath = "";
        hasMtime = false;
        recordedMtime = 0;

        if (line.IndexOf(Delim) >= 0)
        {
            string[] parts = line.Split(Delim);
            if (parts.Length < 3 || parts[0].Length != 1)
                return false;

            type = parts[0][0];
            backupFile = parts[1];
            originalPath = parts[2];

            if (parts.Length >= 4 && long.TryParse(parts[3], out long mtime))
            {
                hasMtime = true;
                recordedMtime = mtime;
            }

            return backupFile.Length > 0;
        }

        // Legacy space-delimited format: "type backupFile originalPath [mtime]"
        if (line[1] != ' ')
            return false;

        type = line[0];
        int firstSpace = line.IndexOf(' ');
        int secondSpace = line.IndexOf(' ', firstSpace + 1);
        if (secondSpace < 0)
            return false;

        int lastSpace = line.LastIndexOf(' ');
        hasMtime = lastSpace > secondSpace && long.TryParse(line.Substring(lastSpace + 1), out recordedMtime);

        backupFile = line.Substring(firstSpace + 1, secondSpace - firstSpace - 1);
        originalPath = hasMtime
            ? line.Substring(secondSpace + 1, lastSpace - secondSpace - 1)
            : line.Substring(secondSpace + 1);

        return backupFile.Length > 0;
    }

    private static void RestoreFromScan(string backupDir, string saveDir, string configDir)
    {
        var saveSubdirs = new List<string>();
        if (Directory.Exists(saveDir))
        {
            foreach (string dir in Directory.GetDirectories(saveDir))
                saveSubdirs.Add(Path.GetFileName(dir));
        }

        foreach (string filePath in Directory.GetFiles(backupDir, "*.sav"))
        {
            string backupName = Path.GetFileName(filePath);
            string withoutSav = backupName.Substring(0, backupName.Length - 4);

            string matchedSubdir = "";
            foreach (string subdir in saveSubdirs)
            {
                if (withoutSav.StartsWith(subdir + "."))
                {
                    matchedSubdir = subdir;
                    break;
                }
            }

            if (!string.IsNullOrEmpty(matchedSubdir))
            {
                string filename = withoutSav.Substring(matchedSubdir.Length + 1);
                string destPath = Path.Combine(saveDir, matchedSubdir, filename);
                string? destParent = Path.GetDirectoryName(destPath);
                if (destParent != null && !Directory.Exists(destParent))
                    Directory.CreateDirectory(destParent);
                File.Copy(filePath, destPath, true);
            }
            else if (withoutSav.Contains(".ini"))
            {
                string destPath = Path.Combine(configDir, withoutSav);
                File.Copy(filePath, destPath, true);
            }
        }
    }

    public static void Backup(string backupDir, string saveDir, string configDir)
    {
        Directory.CreateDirectory(backupDir);

        var lines = new List<string>();
        var currentBackupFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        if (Directory.Exists(saveDir))
        {
            string saveRoot = Path.GetFullPath(saveDir);
            foreach (string filePath in Directory.GetFiles(saveRoot, "*.*", SearchOption.AllDirectories))
            {
                string fullPath = Path.GetFullPath(filePath);
                string relativePath = fullPath.Substring(saveRoot.Length).TrimStart('\\', '/');

                string backupName = relativePath.Replace('\\', '.').Replace('/', '.') + ".sav";
                string destPath = Path.Combine(backupDir, backupName);

                File.Copy(filePath, destPath, true);
                long mtime = File.GetLastWriteTimeUtc(destPath).Ticks;
                lines.Add($"s{Delim}{backupName}{Delim}{relativePath}{Delim}{mtime}");
                currentBackupFiles.Add(backupName);
            }
        }

        if (Directory.Exists(configDir))
        {
            string configRoot = Path.GetFullPath(configDir);
            foreach (string filePath in Directory.GetFiles(configRoot))
            {
                string fileName = Path.GetFileName(filePath);
                string backupName = fileName + ".sav";
                string destPath = Path.Combine(backupDir, backupName);

                File.Copy(filePath, destPath, true);
                long mtime = File.GetLastWriteTimeUtc(destPath).Ticks;
                lines.Add($"c{Delim}{backupName}{Delim}{fileName}{Delim}{mtime}");
                currentBackupFiles.Add(backupName);
            }
        }

        string manifestPath = Path.Combine(backupDir, ManifestName);
        File.WriteAllLines(manifestPath, lines);

        int removed = RemoveOrphanedBackups(backupDir, currentBackupFiles);
        Logger.Info($"Backup complete: {lines.Count} files backed up, {removed} orphaned backup file(s) removed.");
    }

    /// <summary>
    /// Deletes .sav files in backupDir that no longer correspond to any file
    /// currently present in saveDir/configDir (e.g. saves that were deleted
    /// locally). Without this, deleted saves accumulate forever in the
    /// Steam Cloud-synced backup folder.
    /// </summary>
    private static int RemoveOrphanedBackups(string backupDir, HashSet<string> currentBackupFiles)
    {
        int removed = 0;

        foreach (string filePath in Directory.GetFiles(backupDir, "*.sav"))
        {
            string backupName = Path.GetFileName(filePath);
            if (currentBackupFiles.Contains(backupName))
                continue;

            try
            {
                File.Delete(filePath);
                removed++;
            }
            catch (Exception ex)
            {
                Logger.Warn($"Failed to remove orphaned backup '{backupName}': {ex.Message}");
            }
        }

        return removed;
    }
}
