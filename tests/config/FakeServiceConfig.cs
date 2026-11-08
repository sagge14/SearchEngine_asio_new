// Fixture for batch integration tests. Never calls Windows service APIs.
using System;
using System.IO;
using System.Text;

class FakeServiceConfig
{
    static string Env(string name) { return Environment.GetEnvironmentVariable(name) ?? ""; }
    static string Arg(string[] args, string key) {
        int i = Array.IndexOf(args, key);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : "";
    }
    static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        string exe = Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
        File.AppendAllText(Env("SE_TEST_LOG"), exe + "\t" + String.Join("\t", args) + "\r\n", Encoding.UTF8);
        if (exe == "fsutil") return Env("SE_TEST_MODE") == "not-admin" ? 1 : 0;
        if (exe == "netsh") { Console.WriteLine("LocalPort: 25027"); return 0; }
        string command = args.Length > 0 ? args[0] : "";
        string mode = Env("SE_TEST_MODE");
        if (command == "choose-installed-instance") {
            string selected = Env("SE_TEST_SELECTED");
            string output = "language=ru\r\n";
            if (mode != "cancel" && mode != "empty" && mode != "picker-error")
                output += "instance=" + selected + "\r\n";
            File.WriteAllText(Arg(args, "--output"), output, new UTF8Encoding(false));
            return mode == "cancel" ? 2 : mode == "empty" ? 3 : mode == "picker-error" ? 1 : 0;
        }
        if (command == "inspect-installed") {
            string instance = Arg(args, "--instance");
            if (instance == "missing" || mode == "access-denied") {
                Console.Error.WriteLine("ERROR: OpenService; Win32=" + (mode == "access-denied" ? "5" : "1060"));
                return 1;
            }
            if (String.Equals(instance, "default", StringComparison.OrdinalIgnoreCase)) instance = "default";
            string name = instance == "default" ? "SearchEngineService" : "SearchEngineService-" + instance;
            string data = Env("SE_TEST_DATA");
            Console.WriteLine("instance=" + instance + "\nservice_name=" + name + "\ndata_dir=" + data +
                "\nsettings_path=" + Path.Combine(data, "Settings.json") +
                "\nendpoint_path=" + Path.Combine(data, "client-endpoint.txt"));
            return 0;
        }
        if (command == "inspect") {
            if (Arg(args, "--settings") != Path.Combine(Env("SE_TEST_DATA"), "Settings.json")) {
                Console.Error.WriteLine("Wrong settings path"); return 1;
            }
            if (mode == "invalid-settings") return 1;
            Console.WriteLine("port=25027"); return 0;
        }
        if (command == "service-control") {
            if (mode == "stop-error" && Arg(args, "--action") == "stop") return 1;
            if (mode == "start-error" && Arg(args, "--action") == "start") return 1;
            return 0;
        }
        if (command == "health") return mode == "health-error" ? 1 : 0;
        if (command == "script-message") return 0;
        Console.Error.WriteLine("Unexpected fake-helper command"); return 1;
    }
}
