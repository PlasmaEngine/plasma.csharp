using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Runtime.InteropServices;
using System.ComponentModel;

namespace Plasma.ScriptCompiler;

internal sealed record BuildRequest(string Project, string Manifest, string Configuration, string Output);
internal sealed record BuildResponse(bool Success, string Diagnostics, bool CanFallback = false);

internal static class Program
{
    private const int ProtocolVersion = 2;
    private static readonly Lazy<string> ToolIdentity = new(() =>
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (string file in Directory.EnumerateFiles(AppContext.BaseDirectory).Order(StringComparer.Ordinal))
        {
            if (Path.GetExtension(file) is not (".dll" or ".json")) continue;
            hash.AppendData(Encoding.UTF8.GetBytes(Path.GetFileName(file) + "\0"));
            hash.AppendData(SHA256.HashData(File.ReadAllBytes(file)));
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    });

    public static async Task<int> Main(string[] args)
    {
        try
        {
            if (args.Length == 2 && args[0] == "--serve")
                return await Serve(args[1]);
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            for (int i = 0; i + 1 < args.Length; i += 2)
                values.Add(args[i], args[i + 1]);
            var request = new BuildRequest(Path.GetFullPath(values["--project"]),
                Path.GetFullPath(values["--manifest"]), values["--configuration"], Path.GetFullPath(values["--output"]));
            string identity = $"{ProtocolVersion}|{ToolIdentity.Value}|{AppContext.BaseDirectory}|{request.Project}|{request.Configuration}";
            string pipeName = "Plasma.CSharp." + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity)))[..24];
            BuildResponse response = await Request(pipeName, request);
            Console.Write(response.Diagnostics);
            return response.Success ? 0 : response.CanFallback ? 2 : 1;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"C# compiler service failed: {exception.Message}");
            return 2;
        }
    }

    private static async Task<BuildResponse> Request(string name, BuildRequest request)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            using var pipe = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(attempt == 0 ? 100 : 15000);
                using var writer = new StreamWriter(pipe, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
                using var reader = new StreamReader(pipe, Encoding.UTF8, leaveOpen: true);
                await writer.WriteLineAsync(JsonSerializer.Serialize(request));
                string result = await reader.ReadLineAsync().WaitAsync(TimeSpan.FromMinutes(5))
                    ?? throw new IOException("Compiler service disconnected before returning a result.");
                return JsonSerializer.Deserialize<BuildResponse>(result)
                    ?? throw new InvalidDataException("Compiler service returned an invalid response.");
            }
            catch (TimeoutException) when (attempt == 0)
            {
                string server = StageServer();
                if (OperatingSystem.IsWindows())
                {
                    StartDetachedWindows(server, name);
                    continue;
                }
                var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true };
                start.ArgumentList.Add(server);
                start.ArgumentList.Add("--serve");
                start.ArgumentList.Add(name);
                using var process = Process.Start(start) ?? throw new IOException("Could not start compiler service.");
            }
        }
        throw new IOException("Could not connect to compiler service.");
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public uint Size;
        public nint Reserved, Desktop, Title;
        public uint X, Y, Width, Height, XChars, YChars, Fill, Flags;
        public ushort ShowWindow, ReservedBytes;
        public nint ReservedPointer, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInfo { public nint Process, Thread; public uint ProcessId, ThreadId; }
    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcess(string? application, StringBuilder command, nint processAttributes,
        nint threadAttributes, [MarshalAs(UnmanagedType.Bool)] bool inheritHandles, uint flags,
        nint environment, string? directory, ref StartupInfo startup, out ProcessInfo process);
    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(nint handle);
    private static void StartDetachedWindows(string assembly, string pipe)
    {
        // Process.Start on Windows can inherit other inheritable handles in addition to the
        // redirected standard handles. In particular that keeps the native importer's capture
        // pipe alive. This child must inherit no handles at all.
        string executable = Path.Combine(Path.GetDirectoryName(Environment.ProcessPath)!, "dotnet.exe");
        if (!File.Exists(executable)) executable = "dotnet.exe";
        var command = new StringBuilder($"\"{executable}\" \"{assembly}\" --serve \"{pipe}\"");
        var startup = new StartupInfo { Size = (uint)Marshal.SizeOf<StartupInfo>() };
        if (!CreateProcess(null, command, 0, 0, false, 0x08000000, 0, null, ref startup, out var process))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not start the detached C# compiler.");
        CloseHandle(process.Thread); CloseHandle(process.Process);
    }

    private static string StageServer()
    {
        // A service outlives an import. Run an immutable copy so rebuilding/reinstalling the
        // package can replace its tools without waiting for the service's idle timeout.
        string identity = ToolIdentity.Value + "." +
            Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(AppContext.BaseDirectory)))[..12];
        string root = Path.Combine(Path.GetTempPath(), "Plasma", "CSharp", "Compiler", identity);
        using var staging = new Mutex(false, "Plasma.CSharp.Stage." + identity);
        bool acquired;
        try { acquired = staging.WaitOne(TimeSpan.FromSeconds(30)); }
        catch (AbandonedMutexException) { acquired = true; }
        if (!acquired) throw new TimeoutException("Compiler deployment is busy.");
        try
        {
            Directory.CreateDirectory(root);
            foreach (string source in Directory.EnumerateFiles(AppContext.BaseDirectory, "*", SearchOption.AllDirectories))
            {
                string target = Path.Combine(root, Path.GetRelativePath(AppContext.BaseDirectory, source));
                if (File.Exists(target)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                string temporary = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try { File.Copy(source, temporary); File.Move(temporary, target); }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
            }
            return Path.Combine(root, Path.GetFileName(typeof(Program).Assembly.Location));
        }
        finally { staging.ReleaseMutex(); }
    }

    private static async Task<int> Serve(string name)
    {
        // Only one server owns a project's compiler state. Concurrent clients queue on the pipe.
        using var owner = new Semaphore(1, 1, name + ".Owner");
        if (!owner.WaitOne(0))
            return 0;
        var session = new CompilerSession();
        try
        {
            while (true)
            {
                using var pipe = new NamedPipeServerStream(name, PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                using var idle = new CancellationTokenSource(TimeSpan.FromMinutes(15));
                try { await pipe.WaitForConnectionAsync(idle.Token); }
                catch (OperationCanceledException) { return 0; }
                using var reader = new StreamReader(pipe, Encoding.UTF8, leaveOpen: true);
                using var writer = new StreamWriter(pipe, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
                try
                {
                    string input = await reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(30))
                        ?? throw new InvalidDataException("Empty build request.");
                    var request = JsonSerializer.Deserialize<BuildRequest>(input)
                        ?? throw new InvalidDataException("Invalid build request.");
                    BuildResponse response = session.Build(request);
                    await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                }
                catch (Exception exception)
                {
                    try { await writer.WriteLineAsync(JsonSerializer.Serialize(new BuildResponse(false, exception + "\n", CanFallback: true))); }
                    catch (IOException) { }
                    session.Dispose();
                    session = new CompilerSession();
                }
            }
        }
        finally { session.Dispose(); owner.Release(); }
    }
}
