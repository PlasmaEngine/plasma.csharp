using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;
using Plasma;
using Plasma.ManagedHost;
using Plasma.ScriptCompiler;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static unsafe class Program
{
    [StructLayout(LayoutKind.Sequential)]
    private struct UpdateApi
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<CSharpGenerationLoadDesc*, ulong*, CSharpStatus> TryUpdate;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct StateApi
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ulong, CSharpBuffer*, CSharpStatus> Capture;
        public delegate* unmanaged[Cdecl]<ulong, CSharpUtf8Span, CSharpStatus> Restore;
    }
    public static int Main(string[] args)
    {
        if (args.Length != 2) throw new ArgumentException("Pass the BrickBreaker sample directory and an empty test output directory.");
        if (!System.Reflection.Metadata.MetadataUpdater.IsSupported)
            throw new InvalidOperationException("Start this test with DOTNET_MODIFIABLE_ASSEMBLIES=debug.");
        string sample = Path.GetFullPath(args[0]); string root = Path.GetFullPath(args[1]);
        if (Directory.Exists(root)) throw new IOException("Use a fresh fixture directory; existing files are preserved.");
        Directory.CreateDirectory(Path.Combine(root, "Scripts"));
        if (File.Exists(Path.Combine(sample, "global.json"))) File.Copy(Path.Combine(sample, "global.json"), Path.Combine(root, "global.json"));
        foreach (string source in Directory.GetFiles(Path.Combine(sample, "Scripts"), "*.cs", SearchOption.AllDirectories))
        {
            string destination = Path.Combine(root, Path.GetRelativePath(sample, source));
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(source, destination);
        }
        string script = Path.Combine(root, "Scripts", "BrickBreaker.cs");
        File.WriteAllText(script, File.ReadAllText(script).Replace("  private BrickBreakerSimulation", "  public override void OnActivated() { double increment = 1; m_Accumulator += increment; }\n\n  private BrickBreakerSimulation"));
        var project = XDocument.Load(Path.Combine(sample, "BrickBreaker.csproj"));
        string packages = File.ReadAllText(Path.Combine(sample, "Editor/Packages.ddl"));
        string version = System.Text.RegularExpressions.Regex.Match(packages, "%Id\\s*\\{\\s*\"plasma\\.csharp\"\\s*\\}\\s*string\\s*%Version\\s*\\{\\s*\"([^\"]+)\"").Groups[1].Value;
        if (version.Length == 0) throw new InvalidDataException("No plasma.csharp package version in the sample.");
        project.Descendants("PropertyGroup").First().Add(new XElement("CSharpPayload", Path.Combine(sample, "Plugins/plasma.csharp", version, "Bin/Windows_x64_Development/CSharp")));
        project.Descendants("PropertyGroup").First().Add(new XElement("PlasmaSeparateBindings", "true"));
        project.Save(Path.Combine(root, "BrickBreaker.csproj"));
        string manifest = Path.Combine(sample, "Intermediate/PlasmaBindings.json");
        var compiler = new CompilerSession();
        BuildRequest Request(int revision) => new(Path.Combine(root, "BrickBreaker.csproj"), manifest, "Development", Path.Combine(root, "r" + revision));
        BuildResponse build = compiler.Build(Request(0)); Check(build.Success, build.Diagnostics); Console.Write(build.Diagnostics);
        var native = new NativeApiV1 { Size = (uint)sizeof(NativeApiV1), Version = 1, PointerSize = (uint)sizeof(void*), ValueLayoutVersion = 2 };
        var api = new ManagedApiV1 { Size = (uint)sizeof(ManagedApiV1) };
        delegate* unmanaged[Cdecl]<NativeApiV1*, ManagedApiV1*, CSharpStatus> initialize = &Bootstrap.Initialize;
        Check(initialize(&native, &api) == CSharpStatus.Success, "Initialize host");
        try { Exercise(api, compiler, Request, script); }
        finally { _ = api.Shutdown(); }
        Console.WriteLine("PASS: BrickBreaker compiler cache, 12 in-place edits with mapped locals, generated lifecycle dispatch, private game state, compile-error recovery, skipped revisions, structural fallback and full-reload state migration.");
        return 0;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Exercise(ManagedApiV1 api, CompilerSession compiler, Func<int, BuildRequest> request, string scriptPath)
    {
        ulong generation = Load(api, Path.Combine(request(0).Output, "BrickBreaker.dll"));
        using var descriptors = JsonDocument.Parse(File.ReadAllText(Path.Combine(request(0).Output, "Plasma.ScriptDescriptors.json")));
        ulong typeId = ulong.Parse(descriptors.RootElement.GetProperty("classes")[0].GetProperty("typeId").GetString()!, NumberStyles.HexNumber);
        var create = new CSharpInstanceCreateDesc { Size = (uint)sizeof(CSharpInstanceCreateDesc), Version = 1, Generation = generation, TypeId = typeId };
        ulong instanceId = 0; Check(api.CreateInstance(&create, &instanceId) == CSharpStatus.Success, "Create BrickBreaker script");
        var instances = (IDictionary)typeof(Bootstrap).GetField("Instances", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        object script = instances[instanceId]!.GetType().GetProperty("Script")!.GetValue(instances[instanceId])!;
        FieldInfo accumulator = script.GetType().GetField("m_Accumulator", BindingFlags.NonPublic | BindingFlags.Instance)!;
        object game = script.GetType().GetField("m_Game", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(script)!;
        game.GetType().GetField("Score")!.SetValue(game, 123);
        game.GetType().GetField("Lives")!.SetValue(game, 2);
        CSharpValue value = default;
        Check(api.InvokeMethod(instanceId, (ulong)ScriptLifecycleMethod.OnActivated, null, 0, &value) == CSharpStatus.Success, "Baseline lifecycle");
        double expected = 1;
        byte[] name = Encoding.UTF8.GetBytes("Plasma.HotReload"); void* pointer = null;
        fixed (byte* text = name) Check(api.QueryExtension(new() { Data = text, Length = (uint)name.Length }, 1, &pointer) == CSharpStatus.Success, "Hot reload extension");
        var updates = (UpdateApi*)pointer;
        for (int i = 2; i <= 13; ++i)
        {
            File.WriteAllText(scriptPath, File.ReadAllText(scriptPath).Replace($"double increment = {i - 1};", $"double increment = {i};"));
            BuildResponse build = compiler.Build(request(i)); Check(build.Success, build.Diagnostics); Console.Write(build.Diagnostics);
            // Skip one import, then apply its delta together with the next one.
            if (i == 6) continue;
            ulong updated = Update(updates, Path.Combine(request(i).Output, "BrickBreaker.dll"));
            Check(updated == generation, "Generation identity retained");
            Check(api.InvokeMethod(instanceId, (ulong)ScriptLifecycleMethod.OnActivated, null, 0, &value) == CSharpStatus.Success, "Updated cached lifecycle delegate");
            expected += i;
            Check((double)accumulator.GetValue(script)! == expected, "Private accumulator retained");
            Check((int)game.GetType().GetField("Score")!.GetValue(game)! == 123 && (int)game.GetType().GetField("Lives")!.GetValue(game)! == 2, "Score and lives retained");
        }
        BuildResponse cached = compiler.Build(request(20)); Check(cached.Success && cached.Diagnostics.Contains("cached artifact"), "Unchanged build cached");
        string updatePath = Path.Combine(request(20).Output, "Plasma.HotReload.json");
        string originalUpdates = File.ReadAllText(updatePath);
        var forkedUpdates = System.Text.Json.Nodes.JsonNode.Parse(originalUpdates)!;
        foreach (var chain in forkedUpdates["Chains"]!.AsArray())
        {
            var deltas = chain!["Deltas"]!.AsArray();
            if (deltas.Count == 0) continue;
            byte[] forkedIl = Convert.FromBase64String(deltas[0]!["IL"]!.GetValue<string>());
            forkedIl[^1] ^= 1;
            deltas[0]!["IL"] = Convert.ToBase64String(forkedIl);
        }
        File.WriteAllText(updatePath, forkedUpdates.ToJsonString());
        Check(Update(updates, Path.Combine(request(20).Output, "BrickBreaker.dll")) == 0,
            "A different applied prefix from the same baseline is rejected before patching");
        Check((double)accumulator.GetValue(script)! == expected, "Rejected prefix preserves gameplay state");
        File.WriteAllText(updatePath, originalUpdates);
        Check(Update(updates, Path.Combine(request(20).Output, "BrickBreaker.dll")) == generation,
            "The original chain remains usable after a prefix mismatch");
        Console.WriteLine("PASS: divergent delta-chain prefix rejected without poisoning the live generation.");
        string good = File.ReadAllText(scriptPath);
        File.WriteAllText(scriptPath, good + "\nsyntax error\n");
        BuildResponse failed = compiler.Build(request(21)); Check(!failed.Success, "Syntax error rejected");
        Check(!File.Exists(Path.Combine(request(21).Output, "BrickBreaker.dll")), "Failed build did not publish");
        Check(api.InvokeMethod(instanceId, (ulong)ScriptLifecycleMethod.OnActivated, null, 0, &value) == CSharpStatus.Success, "Last good assembly survives error");
        File.WriteAllText(scriptPath, good.Replace("double increment = 13;", "double increment = 14;"));
        BuildResponse recovered = compiler.Build(request(22)); Check(recovered.Success, recovered.Diagnostics);
        Check(Update(updates, Path.Combine(request(22).Output, "BrickBreaker.dll")) == generation, "Error recovery retains baseline");
        File.WriteAllText(scriptPath, File.ReadAllText(scriptPath).Replace("  private double m_Accumulator", "  public int AddedField;\n  private double m_Accumulator"));
        BuildResponse structural = compiler.Build(request(23)); Check(structural.Success, structural.Diagnostics);
        Check(Update(updates, Path.Combine(request(23).Output, "BrickBreaker.dll")) == 0, "Structural edit uses full reload");
        ulong replacement = Load(api, Path.Combine(request(23).Output, "BrickBreaker.dll"));
        Check(replacement != generation, "Replacement assembly loads");
        name = Encoding.UTF8.GetBytes("Plasma.ReloadState");
        fixed (byte* text = name) Check(api.QueryExtension(new() { Data = text, Length = (uint)name.Length }, 1, &pointer) == CSharpStatus.Success, "State migration extension");
        var stateApi = (StateApi*)pointer;
        CSharpBuffer state = default;
        Check(stateApi->Capture(instanceId, &state) == CSharpStatus.Success, "Capture private gameplay state");
        create.Generation = replacement;
        ulong candidate = 0; Check(api.CreateInstance(&create, &candidate) == CSharpStatus.Success, "Create full-reload candidate");
        Check(stateApi->Restore(candidate, new() { Data = (byte*)state.Data, Length = state.Size }) == CSharpStatus.Success, "Restore private gameplay state");
        CSharpBuffer restored = default;
        Check(stateApi->Capture(candidate, &restored) == CSharpStatus.Success, "Capture restored state");
        Check(new ReadOnlySpan<byte>(state.Data, (int)state.Size).SequenceEqual(new ReadOnlySpan<byte>(restored.Data, (int)restored.Size)), "Full reload preserves exact gameplay values");
        _ = api.FreeBuffer(&state); _ = api.FreeBuffer(&restored);
        byte[] incompatible = Encoding.UTF8.GetBytes("{\"Version\":99,\"Game\":{},\"Accumulator\":0}");
        fixed (byte* text = incompatible) Check(stateApi->Restore(candidate, new() { Data = text, Length = (uint)incompatible.Length }) != CSharpStatus.Success, "Incompatible state rejected");
        // Continue through the bounded 64-delta window, proving a deliberate full reload
        // renews the baseline without losing the live sample's gameplay state.
        var timings = new List<string> { "iteration,build_ms,apply_ms,mode" };
        double stressExpected = CaptureAccumulator(api, stateApi, candidate);
        int fullReloads = 0;
        for (int increment = 15; increment <= 114; ++increment)
        {
            File.WriteAllText(scriptPath, File.ReadAllText(scriptPath).Replace($"double increment = {increment - 1};", $"double increment = {increment};"));
            var timer = Stopwatch.StartNew();
            BuildRequest stressRequest = request(1000 + increment);
            BuildResponse stressBuild = compiler.Build(stressRequest);
            double buildMs = timer.Elapsed.TotalMilliseconds;
            Check(stressBuild.Success, stressBuild.Diagnostics);
            timer.Restart();
            ulong updated = Update(updates, Path.Combine(stressRequest.Output, "BrickBreaker.dll"));
            bool full = updated == 0;
            if (full)
            {
                CSharpBuffer previous = default;
                Check(stateApi->Capture(candidate, &previous) == CSharpStatus.Success, "Stress state capture");
                try
                {
                    replacement = Load(api, Path.Combine(stressRequest.Output, "BrickBreaker.dll"));
                    create.Generation = replacement;
                    ulong next = 0;
                    Check(api.CreateInstance(&create, &next) == CSharpStatus.Success, "Stress replacement");
                    Check(stateApi->Restore(next, new() { Data = (byte*)previous.Data, Length = previous.Size }) == CSharpStatus.Success, "Stress state restore");
                    Check(api.DestroyInstance(candidate) == CSharpStatus.Success, "Stress old instance cleanup");
                    candidate = next;
                    ++fullReloads;
                }
                finally { api.FreeBuffer(&previous); }
            }
            else Check(updated == replacement, "Stress update reused the live instance's generation");
            Check(api.InvokeMethod(candidate, (ulong)ScriptLifecycleMethod.OnActivated, null, 0, &value) == CSharpStatus.Success, "Stress cached delegate");
            stressExpected += increment;
            Check(CaptureAccumulator(api, stateApi, candidate) == stressExpected, "Stress private state continuity");
            timings.Add(FormattableString.Invariant($"{increment - 14},{buildMs:F3},{timer.Elapsed.TotalMilliseconds:F3},{(full ? "full" : "delta")}"));
        }
        Check(fullReloads >= 1, "Bounded delta window renewed with a full baseline");
        File.WriteAllLines(Path.Combine(Path.GetDirectoryName(request(0).Output)!, "stress-timings.csv"), timings);
        Console.WriteLine($"PASS: 100 further BrickBreaker edits, {fullReloads} bounded full reload(s), exact private-state continuity. Raw timings saved.");
        Check(api.DestroyInstance(candidate) == CSharpStatus.Success, "Destroy candidate");
        Check(api.DestroyInstance(instanceId) == CSharpStatus.Success, "Destroy script");
        TestDependencyOnlyChange(api, updates, stateApi, compiler, request, scriptPath, typeId);
    }
    private static void TestDependencyOnlyChange(ManagedApiV1 api, UpdateApi* updates, StateApi* stateApi,
        CompilerSession compiler, Func<int, BuildRequest> request, string scriptPath, ulong typeId)
    {
        string projectPath = request(0).Project;
        string helper = Path.Combine(Path.GetDirectoryName(projectPath)!, "ReloadHelper.dll");
        void EmitHelper(int result)
        {
            var compilation = CSharpCompilation.Create("ReloadHelper",
                [CSharpSyntaxTree.ParseText($"public static class ReloadHelper {{ public static int Value() => {result}; }}")],
                compiler.MetadataReferences,
                new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, deterministic: true));
            using var pe = new MemoryStream();
            var emitted = compilation.Emit(pe);
            Check(emitted.Success, string.Join('\n', emitted.Diagnostics));
            File.WriteAllBytes(helper, pe.ToArray());
        }
        EmitHelper(1);
        var project = XDocument.Load(projectPath);
        project.Descendants("ItemGroup").First().Add(new XElement("Reference", new XAttribute("Include", "ReloadHelper"),
            new XElement("HintPath", helper), new XElement("Private", "true")));
        project.Save(projectPath);
        File.WriteAllText(scriptPath, File.ReadAllText(scriptPath).Replace("m_Accumulator += increment;", "m_Accumulator += increment + ReloadHelper.Value();"));
        BuildRequest before = request(2000), after = request(2001);
        var build = compiler.Build(before); Check(build.Success, build.Diagnostics);
        ulong generation = Load(api, Path.Combine(before.Output, "BrickBreaker.dll"));
        var create = new CSharpInstanceCreateDesc { Size = (uint)sizeof(CSharpInstanceCreateDesc), Version = 1, Generation = generation, TypeId = typeId };
        ulong instance = 0; Check(api.CreateInstance(&create, &instance) == CSharpStatus.Success, "Dependency test instance");
        CSharpValue value = default;
        Check(api.InvokeMethod(instance, (ulong)ScriptLifecycleMethod.OnActivated, null, 0, &value) == CSharpStatus.Success, "Original helper dispatch");
        Check(CaptureAccumulator(api, stateApi, instance) == 115, "Original helper returns one");
        EmitHelper(2);
        build = compiler.Build(after); Check(build.Success, build.Diagnostics);
        string independent = request(2002).Output;
        Directory.CreateDirectory(independent);
        foreach (string file in Directory.GetFiles(after.Output))
            File.Copy(file, Path.Combine(independent, Path.GetFileName(file)));
        after = after with { Output = independent };
        // Also cover an independently rebuilt helper deployed beside the preceding game PE.
        // A normal project rebuild may change the deterministic game MVID because reference
        // inputs changed, so explicitly retain the original game artifact for this harder case.
        foreach (string retained in new[] { "BrickBreaker.dll", "BrickBreaker.pdb", "Plasma.HotReload.json" })
            File.Copy(Path.Combine(before.Output, retained), Path.Combine(after.Output, retained), overwrite: true);
        Check(File.ReadAllBytes(Path.Combine(before.Output, "BrickBreaker.dll")).SequenceEqual(File.ReadAllBytes(Path.Combine(after.Output, "BrickBreaker.dll"))), "Dependency-only change preserved exact game assembly bytes");
        Check(Update(updates, Path.Combine(after.Output, "BrickBreaker.dll")) == 0, "Dependency-only change cannot patch a stale helper context");
        create.Generation = Load(api, Path.Combine(after.Output, "BrickBreaker.dll"));
        ulong replacement = 0; Check(api.CreateInstance(&create, &replacement) == CSharpStatus.Success, "Replacement helper context");
        CSharpBuffer state = default;
        Check(stateApi->Capture(instance, &state) == CSharpStatus.Success, "Dependency reload capture");
        try { Check(stateApi->Restore(replacement, new() { Data = (byte*)state.Data, Length = state.Size }) == CSharpStatus.Success, "Dependency reload restore"); }
        finally { api.FreeBuffer(&state); }
        Check(api.InvokeMethod(replacement, (ulong)ScriptLifecycleMethod.OnActivated, null, 0, &value) == CSharpStatus.Success, "Updated helper dispatch");
        Check(CaptureAccumulator(api, stateApi, replacement) == 231, "New dependency actually executes while private state is preserved");
        Check(api.DestroyInstance(instance) == CSharpStatus.Success && api.DestroyInstance(replacement) == CSharpStatus.Success, "Dependency instances cleaned up");
        Console.WriteLine("PASS: dependency-only DLL change with identical game PE, stale-context rejection, updated helper execution and retained game state.");
    }
    private static double CaptureAccumulator(ManagedApiV1 api, StateApi* stateApi, ulong instance)
    {
        CSharpBuffer buffer = default;
        Check(stateApi->Capture(instance, &buffer) == CSharpStatus.Success, "Capture stress state");
        try
        {
            using var document = JsonDocument.Parse(new ReadOnlySpan<byte>(buffer.Data, (int)buffer.Size).ToArray());
            return document.RootElement.GetProperty("Accumulator").GetDouble();
        }
        finally { api.FreeBuffer(&buffer); }
    }
    private static ulong Load(ManagedApiV1 api, string path)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(path); ulong generation = 0;
        fixed (byte* text = bytes)
        {
            var desc = new CSharpGenerationLoadDesc { Size = (uint)sizeof(CSharpGenerationLoadDesc), Version = 1, AssemblyPath = new() { Data = text, Length = (uint)bytes.Length } };
            Check(api.LoadGeneration(&desc, &generation) == CSharpStatus.Success, "Load game assembly");
        }
        return generation;
    }
    private static ulong Update(UpdateApi* api, string path)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(path); ulong generation = 0;
        fixed (byte* text = bytes)
        {
            var desc = new CSharpGenerationLoadDesc { Size = (uint)sizeof(CSharpGenerationLoadDesc), Version = 1, AssemblyPath = new() { Data = text, Length = (uint)bytes.Length } };
            CSharpStatus status = api->TryUpdate(&desc, &generation);
            if (status != CSharpStatus.Success) return 0;
        }
        return generation;
    }
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
