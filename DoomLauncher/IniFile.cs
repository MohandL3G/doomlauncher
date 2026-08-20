using System;
using System.Collections.Generic;
using System.IO;

namespace DoomLauncher;

class IniFile
{
    private readonly Dictionary<string, Dictionary<string, List<string>>> _sections = new();

    public static IniFile Load(string path)
    {
        var ini = new IniFile();
        string currentSection = "";
        bool inArray = false;
        string arrayKey = "";

        foreach (string rawLine in File.ReadLines(path))
        {
            string line = rawLine.Trim();

            if (line.Length == 0 || line[0] == ';' || line[0] == '#')
                continue;

            if (line[0] == '[')
            {
                int end = line.IndexOf(']');
                if (end > 1)
                {
                    currentSection = line.Substring(1, end - 1);
                    if (!ini._sections.ContainsKey(currentSection))
                        ini._sections[currentSection] = new Dictionary<string, List<string>>();
                }
                continue;
            }

            if (inArray)
            {
                if (line == "}")
                {
                    inArray = false;
                    continue;
                }
                if (!string.IsNullOrWhiteSpace(line))
                {
                    ini._sections[currentSection][arrayKey].Add(line.Trim());
                }
                continue;
            }

            int eq = line.IndexOf('=');
            if (eq > 0)
            {
                string key = line.Substring(0, eq).Trim();
                string value = line.Substring(eq + 1).Trim();

                if (!ini._sections.ContainsKey(currentSection))
                    ini._sections[currentSection] = new Dictionary<string, List<string>>();

                if (!ini._sections[currentSection].ContainsKey(key))
                    ini._sections[currentSection][key] = new List<string>();

                if (value == "{")
                {
                    inArray = true;
                    arrayKey = key;
                }
                else if (!string.IsNullOrWhiteSpace(value))
                {
                    ini._sections[currentSection][key].Add(value);
                }
            }
        }

        return ini;
    }

    public string Get(string section, string key, string defaultValue)
    {
        var values = GetAll(section, key);
        return values.Count > 0 ? values[0] : defaultValue;
    }

    public List<string> GetAll(string section, string key)
    {
        if (_sections.TryGetValue(section, out var keys) && keys.TryGetValue(key, out var values))
            return values;
        return new List<string>();
    }
}
