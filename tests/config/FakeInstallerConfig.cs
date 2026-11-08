// Installer regression fixture. Never calls service, registry, or firewall APIs.
using System;
using System.IO;

class FakeInstallerConfig
{
    static string Env(string name) { return Environment.GetEnvironmentVariable(name) ?? ""; }
    static string Arg(string[] args, string key) {
        int i = Array.IndexOf(args, key);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : "";
    }
    static int Main(string[] args)
    {
        string exe = Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
        string command = args.Length > 0 ? args[0] : "";
        string mode = Env("SE_INSTALL_TEST_MODE");
        string log = Env("SE_INSTALL_TEST_LOG");
        File.AppendAllText(log, exe + "\t" + String.Join("\t", args) + "\r\n");
        if (exe == "fsutil") return mode == "not-admin" ? 1 : 0;
        if (exe == "sc") {
            if (command == "query") {
                if (args.Length > 1 && File.Exists(Env("SE_INSTALL_TEST_STARTED"))) {
                    Console.WriteLine("STATE : 4 RUNNING"); return 0;
                }
                return 1;
            }
            if (command == "start") File.WriteAllText(Env("SE_INSTALL_TEST_STARTED"), "started");
            return 0;
        }
        if (exe == "reg" || exe == "netsh") return 0;
        if (exe == "SearchEngine") return command == "--initial-update" ? 0 : 97;
        if (exe == "SearchClientTokenIssuer") {
            if (command == "--language" && Array.IndexOf(args, "--show-computer-id") >= 0)
                Console.WriteLine("00000000-0000-0000-0000-000000000001");
            string output = Arg(args, "--output");
            if (output != "") File.WriteAllText(output, "test token");
            return 0;
        }
        if (exe == "AuthDbTool") return 0;
        if (command == "script-message") {
            Console.WriteLine(Arg(args, "--id")); return 0;
        }
        if (command == "system-info") {
            string countFile = Env("SE_INSTALL_TEST_INFO_COUNT");
            int count = File.Exists(countFile) ? Int32.Parse(File.ReadAllText(countFile)) : 0;
            File.WriteAllText(countFile, (++count).ToString());
            if (mode == "crt-load-failure" || (mode == "instance-helper-failure" && count > 1)) return 1;
            Console.WriteLine("recommended_threads=2");
            if (mode != "missing-year") Console.WriteLine("current_year=2026");
            return 0;
        }
        if (command == "inspect") {
            if (mode == "template-inspect-failure") return 1;
            Console.WriteLine("port=25027\nyear=2026\nthreads=2\nfile_timeout_sec=10\n" +
                "prm_short_content_autodetect=0\ndocument_catalog_storage=sqlite\n" +
                "authentication_mode=" + ((mode == "success-none" || mode == "success-fast-none") ? "none" : "required"));
            return 0;
        }
        if (command == "configure-local-machine") {
            File.WriteAllText(Arg(args, "--output"), "{}"); return 0;
        }
        if (command == "validate" || command == "check-port" || command == "health") return 0;
        Console.Error.WriteLine("Unexpected installer fixture command: " + exe + " " + command);
        return 97;
    }
}
