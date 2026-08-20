using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;

namespace DoomLauncher;

class Program
{
    private const string DefaultConfig = @"; DoomLauncher config.ini
; All paths are relative to this config file's directory, unless absolute.

[Launch]
; Path to uzdoom.exe (relative to this config file)
ExePath=Mods\UZDoom\uzdoom.exe

; Working directory for uzdoom (where it finds its own .pk3 files)
WorkDir=Mods\UZDoom

; IWAD to use (absolute or relative to this config file)
IWAD=doom2.wad

; Mod/PWAD files to load, one per line
Mods={
    Mods\modfolder\file1.pk3
    Mods\modfolder\file2.pk3
}

; Optional: custom UZDoom config file (relative to UzDoomConfigDir or absolute)
; If empty, UZDoom uses its default config
ConfigFile=

; Extra command line arguments (appended last)
ExtraArgs=

[Sync]
; Enable/disable save syncing between UZDoom and rerelease (Steam Cloud)
Enabled=false

; Where the rerelease stores saves (Steam Cloud syncs this folder)
BackupDir=

; UZDoom save directory (contains subfolders like doom.id.doom2.kex/)
UzDoomSaveDir=

; UZDoom config directory (contains uzdoom.ini etc.)
UzDoomConfigDir=
";

    [STAThread]
    static int Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        string exeDir = AppContext.BaseDirectory;
        string configPath = Path.Combine(exeDir, "config.ini");

        if (!File.Exists(configPath))
        {
            File.WriteAllText(configPath, DefaultConfig);

            var okBtn = new TaskDialogButton("OK");
            var cancelBtn = TaskDialogButton.Cancel;

            var page = new TaskDialogPage
            {
                Caption = "DoomLauncher",
                Heading = "config.ini was not found.",
                Text = "A default config has been created.\n\nPlease edit it with your settings, click OK to open it in Notepad.",
                Icon = TaskDialogIcon.Information,
                Buttons = { okBtn, cancelBtn }
            };

            if (TaskDialog.ShowDialog(page) == okBtn)
                Process.Start(new ProcessStartInfo("notepad.exe", configPath) { UseShellExecute = true });

            return 0;
        }

        var config = IniFile.Load(configPath);

        string exePath = config.Get("Launch", "ExePath", "");
        string workDir = config.Get("Launch", "WorkDir", "");
        string iwad = config.Get("Launch", "IWAD", "");
        List<string> modList = config.GetAll("Launch", "Mods");
        string configFile = config.Get("Launch", "ConfigFile", "");
        string extraArgs = config.Get("Launch", "ExtraArgs", "");

        string syncEnabled = config.Get("Sync", "Enabled", "false");
        string backupDir = config.Get("Sync", "BackupDir", "");
        string saveDir = config.Get("Sync", "UzDoomSaveDir", "");
        string configDir = config.Get("Sync", "UzDoomConfigDir", "");

        if (string.IsNullOrWhiteSpace(exePath))
        {
            var page = new TaskDialogPage
            {
                Caption = "DoomLauncher",
                Heading = "ExePath is not set.",
                Text = "Please set the path to uzdoom.exe in config.ini.",
                Icon = TaskDialogIcon.Error,
                Buttons = { TaskDialogButton.OK }
            };
            TaskDialog.ShowDialog(page);
            return 1;
        }

        string exeFullPath = Path.GetFullPath(Path.Combine(exeDir, exePath));

        if (!File.Exists(exeFullPath))
        {
            var openBtn = new TaskDialogButton("Open config.ini");
            var closeBtn = TaskDialogButton.Close;

            var page = new TaskDialogPage
            {
                Caption = "DoomLauncher",
                Heading = "uzdoom.exe not found.",
                Text = $"Could not find uzdoom.exe at:\n{exeFullPath}\n\nDownload UZDoom and update the ExePath in config.ini.",
                Icon = TaskDialogIcon.Error,
                Buttons = { openBtn, closeBtn }
            };

            if (TaskDialog.ShowDialog(page) == openBtn)
                Process.Start(new ProcessStartInfo("notepad.exe", configPath) { UseShellExecute = true });

            return 1;
        }

        string workDirFull = string.IsNullOrWhiteSpace(workDir)
            ? Path.GetDirectoryName(exeFullPath)!
            : Path.GetFullPath(Path.Combine(exeDir, workDir));

        bool doSync = syncEnabled.Equals("true", StringComparison.OrdinalIgnoreCase)
                       && !string.IsNullOrWhiteSpace(backupDir)
                       && !string.IsNullOrWhiteSpace(saveDir)
                       && !string.IsNullOrWhiteSpace(configDir);

        if (doSync)
        {
            string backupDirFull = Path.GetFullPath(backupDir);
            string saveDirFull = Path.GetFullPath(saveDir);
            string configDirFull = Path.GetFullPath(configDir);
            SyncManager.Restore(backupDirFull, saveDirFull, configDirFull);
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = exeFullPath,
            WorkingDirectory = workDirFull,
            UseShellExecute = false,
        };

        string commandLine = "";

        if (!string.IsNullOrWhiteSpace(iwad))
        {
            string iwadPath = Path.GetFullPath(Path.Combine(exeDir, iwad));
            commandLine += $"-iwad \"{iwadPath}\" ";
        }

        if (!string.IsNullOrWhiteSpace(configFile))
        {
            string configFilePath;
            if (Path.IsPathRooted(configFile))
            {
                configFilePath = configFile;
            }
            else if (!string.IsNullOrWhiteSpace(configDir))
            {
                configFilePath = Path.GetFullPath(Path.Combine(configDir, configFile));
            }
            else
            {
                configFilePath = Path.GetFullPath(Path.Combine(exeDir, configFile));
            }

            if (File.Exists(configFilePath))
            {
                commandLine += $"-config \"{configFilePath}\" ";
            }
            else
            {
                string defaultConfigPath = Path.Combine(configDir, "uzdoom.ini");
                bool hasDefault = File.Exists(defaultConfigPath);

                var copyBtn = new TaskDialogButton("Copy from default");
                var useDefaultBtn = new TaskDialogButton("Use default");
                var cancelBtn = TaskDialogButton.Cancel;

                var page = new TaskDialogPage
                {
                    Caption = "DoomLauncher",
                    Heading = "Config file not found.",
                    Text = $"ConfigFile is set to \"{configFile}\" but the file was not found at:\n{configFilePath}",
                    Icon = TaskDialogIcon.Warning,
                };

                if (hasDefault)
                {
                    page.Buttons.Add(copyBtn);
                    page.Buttons.Add(useDefaultBtn);
                }
                else
                {
                    page.Buttons.Add(useDefaultBtn);
                }

                var result = TaskDialog.ShowDialog(page);

                if (result == copyBtn && hasDefault)
                {
                    File.Copy(defaultConfigPath, configFilePath);
                    commandLine += $"-config \"{configFilePath}\" ";
                }
                else if (result == copyBtn && !hasDefault)
                {
                    commandLine += $"-config \"{configFilePath}\" ";
                }
            }
        }

        if (modList.Count > 0)
        {
            commandLine += "-file ";
            foreach (string mod in modList)
            {
                string modPath = Path.GetFullPath(Path.Combine(exeDir, mod.Trim()));
                commandLine += $"\"{modPath}\" ";
            }
        }

        if (!string.IsNullOrWhiteSpace(extraArgs))
        {
            commandLine += extraArgs.Trim() + " ";
        }

        startInfo.Arguments = commandLine.TrimEnd();

        try
        {
            Process? proc = Process.Start(startInfo);
            if (proc != null)
            {
                proc.WaitForExit();

                if (doSync)
                {
                    string backupDirFull = Path.GetFullPath(backupDir);
                    string saveDirFull = Path.GetFullPath(saveDir);
                    string configDirFull = Path.GetFullPath(configDir);
                    SyncManager.Backup(backupDirFull, saveDirFull, configDirFull);
                }

                return proc.ExitCode;
            }
            return 1;
        }
        catch (Exception ex)
        {
            var page = new TaskDialogPage
            {
                Caption = "DoomLauncher",
                Heading = "Error launching UZDoom.",
                Text = ex.Message,
                Icon = TaskDialogIcon.Error,
                Buttons = { TaskDialogButton.OK }
            };
            TaskDialog.ShowDialog(page);
            return 1;
        }
    }
}
