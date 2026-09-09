using System;
using System.Collections.Generic;
using System.IO;

namespace DoomLauncher;

class SyncManager
{
    private const string ManifestName = "doomlauncher_sync.manifest";

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
            RestoreFromScan(backupDir, saveDir, configDir);
        }
    }

    private static void RestoreFromManifest(string manifestPath, string backupDir, string saveDir, string configDir)
    {
        foreach (string rawLine in File.ReadLines(manifestPath))
        {
            string line = rawLine.Trim();
            if (line.Length < 3 || line[1] != ' ')
                continue;

            char type = line[0];
            int firstSpace = line.IndexOf(' ');
            int secondSpace = line.IndexOf(' ', firstSpace + 1);
            if (secondSpace < 0) continue;

            int lastSpace = line.LastIndexOf(' ');
            long recordedMtime = 0;
            bool hasMtime = lastSpace > secondSpace && long.TryParse(line.Substring(lastSpace + 1), out recordedMtime);

            string backupFile = line.Substring(firstSpace + 1, secondSpace - firstSpace - 1);
            string originalPath = hasMtime
                ? line.Substring(secondSpace + 1, lastSpace - secondSpace - 1)
                : line.Substring(secondSpace + 1);

            string srcPath = Path.Combine(backupDir, backupFile);
            if (!File.Exists(srcPath))
                continue;

            if (hasMtime && File.GetLastWriteTimeUtc(srcPath).Ticks == recordedMtime)
                continue;

            string destDir = type == 'c' ? configDir : saveDir;
            string destPath = Path.Combine(destDir, originalPath.Replace('/', '\\'));
            string? destParent = Path.GetDirectoryName(destPath);

            if (destParent != null && !Directory.Exists(destParent))
                Directory.CreateDirectory(destParent);

            File.Copy(srcPath, destPath, true);
        }
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
                lines.Add($"s {backupName} {relativePath} {mtime}");
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
                lines.Add($"c {backupName} {fileName} {mtime}");
            }
        }

        string manifestPath = Path.Combine(backupDir, ManifestName);
        File.WriteAllLines(manifestPath, lines);
    }
}
