using System.Collections.Immutable;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace Plasma.ScriptCompiler;

internal sealed class CompilerSession : IDisposable
{
    internal IEnumerable<MetadataReference> MetadataReferences => _compilation?.References ?? [];
    public void Dispose() { _hotReload.Dispose(); _loader?.Unload(); _evaluation?.Dispose(); }
    private string? _configurationHash;
    private JsonDocument? _evaluation;
    private CSharpCompilation? _compilation;
    private GeneratorDriver? _driver;
    private readonly Dictionary<string, SyntaxTree> _trees = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string> _sourceHashes = new(StringComparer.OrdinalIgnoreCase);
    private string? _successfulHash;
    private byte[]? _pe;
    private byte[]? _pdb;
    private string? _descriptor;
    private string _diagnostics = string.Empty;
    private AnalyzerLoader? _loader;
    private ImmutableArray<DiagnosticAnalyzer> _analyzers = [];
    private HotReload _hotReload = new();
    private string? _updates;
    private ImmutableArray<AdditionalText> _additional = [];
    private string? _additionalHash;
    private ISourceGenerator? _bindingGenerator;
    private byte[]? _bindingPe;
    private string? _bindingHash;
    private MetadataReference? _bindingReference;

    public BuildResponse Build(BuildRequest request)
    {
        var timer = Stopwatch.StartNew();
        if (!request.Configuration.Equals("Development", StringComparison.OrdinalIgnoreCase))
            throw new NotSupportedException("Persistent compilation and in-place updates are limited to Development. Other configurations use the project's normal CLI settings.");
        if (!File.Exists(request.Project) || !File.Exists(request.Manifest))
            return new(false, "The C# project or binding manifest is missing.\n");
        string projectDirectory = Path.GetDirectoryName(request.Project)!;
        string configurationHash = ConfigurationHash(projectDirectory, request);
        if (_compilation is null || configurationHash != _configurationHash || ReferenceInputsChanged())
        {
            Evaluate(request);
            _configurationHash = configurationHash;
            _successfulHash = null;
            _hotReload.Dispose();
            _hotReload = new();
        }

        var inputs = new List<string>();
        bool changed = false;
        foreach (string source in ItemPaths("Compile"))
        {
            byte[] bytes = File.ReadAllBytes(source);
            string hash = Convert.ToHexString(SHA256.HashData(bytes));
            inputs.Add(source + "|" + hash);
            if (_sourceHashes.TryGetValue(source, out string? previous) && previous == hash)
                continue;
            SyntaxTree tree = CSharpSyntaxTree.ParseText(SourceText.From(new MemoryStream(bytes), encoding: Encoding.UTF8),
                (CSharpParseOptions)_compilation!.SyntaxTrees.First().Options, source);
            _compilation = _trees.TryGetValue(source, out SyntaxTree? old)
                ? _compilation.ReplaceSyntaxTree(old, tree)
                : _compilation.AddSyntaxTrees(tree);
            _trees[source] = tree;
            _sourceHashes[source] = hash;
            changed = true;
        }
        foreach (string additional in ItemPaths("AdditionalFiles"))
            inputs.Add(additional + "|" + HashFile(additional));
        string additionalHash = HashText(string.Join('|', ItemPaths("AdditionalFiles").Select(path => path + "|" + HashFile(path))));
        string inputHash = HashText(configurationHash + "|" + string.Join('|', inputs));
        bool reused = inputHash == _successfulHash && _pe is not null;
        if (!reused)
        {
            if (additionalHash != _additionalHash)
            {
                _additional = ItemPaths("AdditionalFiles").Select(path => (AdditionalText)new FileText(path)).ToImmutableArray();
                _driver = _driver!.ReplaceAdditionalTexts(_additional);
                _additionalHash = additionalHash;
            }
            if (_bindingGenerator is not null && _bindingHash != additionalHash)
                CompileBindings(additionalHash, projectDirectory);
            var stage = Stopwatch.StartNew();
            _driver = _driver!.RunGeneratorsAndUpdateCompilation(_compilation!, out Compilation generated, out var generatorDiagnostics);
            double generatorsMs = stage.Elapsed.TotalMilliseconds; stage.Restart();
            var allDiagnostics = CompilationWithAnalyzers.GetEffectiveDiagnostics(generatorDiagnostics, generated).Concat(generated.GetDiagnostics()).ToList();
            double diagnosticsMs = stage.Elapsed.TotalMilliseconds; stage.Restart();
            if (!_analyzers.IsEmpty)
                allDiagnostics.AddRange(generated.WithAnalyzers(_analyzers,
                    new AnalyzerOptions(_additional,
                        new ProjectOptions(projectDirectory))).GetAnalyzerDiagnosticsAsync().GetAwaiter().GetResult());
            double analyzersMs = stage.Elapsed.TotalMilliseconds; stage.Restart();
            using var pe = new MemoryStream();
            using var pdb = new MemoryStream();
            EmitResult emitted = generated.Emit(pe, pdb, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
            allDiagnostics.AddRange(emitted.Diagnostics);
            _diagnostics = string.Join('\n', allDiagnostics.Where(d => d.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning)
                .Select(d => d.ToString()).Distinct()) + "\n";
            if (!emitted.Success || allDiagnostics.Any(d => d.Severity == DiagnosticSeverity.Error))
                return new(false, _diagnostics);
            _pe = pe.ToArray();
            _pdb = pdb.ToArray();
            double emitMs = stage.Elapsed.TotalMilliseconds; stage.Restart();
            _updates = JsonSerializer.Serialize(_hotReload.Compile((CSharpCompilation)generated, _pe, _pdb));
            _diagnostics += $"Compiler stages: generators {generatorsMs:F1} ms, diagnostics {diagnosticsMs:F1} ms, analyzers {analyzersMs:F1} ms, emit {emitMs:F1} ms, deltas {stage.Elapsed.TotalMilliseconds:F1} ms.\n";
            _descriptor = null;
            _successfulHash = inputHash;
        }

        Directory.CreateDirectory(request.Output);
        if (_sourceHashes.Any(pair => HashFile(pair.Key) != pair.Value) || ReferenceInputsChanged() ||
            ConfigurationHash(projectDirectory, request) != configurationHash ||
            ItemPaths("AdditionalFiles").Any(path => !inputs.Contains(path + "|" + HashFile(path))))
            return new(false, "C# inputs changed during compilation. Retry the build with the latest revision.\n");
        string target = Path.Combine(request.Output, Property("TargetFileName"));
        File.WriteAllBytes(target, _pe!);
        File.WriteAllBytes(Path.ChangeExtension(target, ".pdb"), _pdb!);
        CopyDependencies(request.Output);
        if (_bindingPe is not null) File.WriteAllBytes(Path.Combine(request.Output, "Plasma.ProjectBindings.dll"), _bindingPe);
        string descriptors = Path.Combine(request.Output, "Plasma.ScriptDescriptors.json");
        if (_descriptor is null)
        {
            Plasma.ScriptInspector.Program.Inspect(target, descriptors);
            _descriptor = File.ReadAllText(descriptors);
        }
        else File.WriteAllText(descriptors, _descriptor);
        File.WriteAllText(Path.Combine(request.Output, "Plasma.HotReload.json"), _updates!);
        return new(true, _diagnostics + $"C# compiler: {(reused ? "cached artifact" : changed ? "incremental compilation" : "initial compilation")}, {timer.Elapsed.TotalMilliseconds:F1} ms.\n");
    }

    private void Evaluate(BuildRequest request)
    {
        // A retained Roslyn compilation cannot execute arbitrary user build targets. Let MSBuild
        // own these projects rather than silently omitting custom generation or packaging steps.
        var projectXml = System.Xml.Linq.XDocument.Load(request.Project);
        if (projectXml.Descendants().Any(element => element.Name.LocalName is "Target" or "Import"))
            throw new NotSupportedException("Custom project imports or targets require the CLI build.");
        for (DirectoryInfo? ancestor = new(Path.GetDirectoryName(request.Project)!); ancestor is not null; ancestor = ancestor.Parent)
        {
            foreach (string name in new[] { "Directory.Build.props", "Directory.Build.targets" })
            {
                string path = Path.Combine(ancestor.FullName, name);
                if (File.Exists(path) && System.Xml.Linq.XDocument.Load(path).Descendants().Any(element => element.Name.LocalName is "Target" or "Import"))
                    throw new NotSupportedException("Custom directory build imports or targets require the CLI build.");
            }
        }
        var start = new ProcessStartInfo("dotnet") { WorkingDirectory = Path.GetDirectoryName(request.Project)!,
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string arg in new[] { "msbuild", request.Project, "-nologo", "-restore",
            "-target:ResolveReferences;GenerateGlobalUsings;GenerateAssemblyInfo;GenerateBuildDependencyFile",
            "-getProperty:TargetFileName,AssemblyName,DefineConstants,Nullable,AllowUnsafeBlocks,LangVersion,MSBuildAllProjects,OutputType,ProjectDepsFilePath,TreatWarningsAsErrors,WarningsAsErrors,WarningsNotAsErrors,NoWarn,WarningLevel,CheckForOverflowUnderflow,SignAssembly,DelaySign,ApplicationIcon,Win32Resource,PlasmaSeparateBindings",
            "-getItem:Compile,ReferencePath,Analyzer,AdditionalFiles,ProjectReference", "-property:Configuration=" + request.Configuration,
            "-property:PlasmaBindingManifest=" + request.Manifest, "-property:Optimize=false", "-property:DebugSymbols=true", "-property:DebugType=portable" })
            start.ArgumentList.Add(arg);
        using var process = Process.Start(start) ?? throw new IOException("Could not evaluate C# project.");
        Task<string> output = process.StandardOutput.ReadToEndAsync();
        Task<string> errors = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(120000)) { process.Kill(entireProcessTree: true); throw new TimeoutException("C# project evaluation timed out."); }
        string json = output.GetAwaiter().GetResult();
        if (process.ExitCode != 0) throw new InvalidDataException(json + errors.GetAwaiter().GetResult());
        int beginning = json.IndexOf('{');
        if (beginning < 0) throw new InvalidDataException("MSBuild did not return evaluated inputs.");
        _evaluation?.Dispose();
        _evaluation = JsonDocument.Parse(json[beginning..]);
        if (Property("OutputType") != "Library") throw new NotSupportedException("Persistent script compilation requires a library project.");
        if (ItemPaths("ProjectReference").Any())
            throw new NotSupportedException("Project references require the CLI build so referenced projects are built first.");
        foreach (string source in ItemPaths("Compile"))
            for (DirectoryInfo? folder = new(Path.GetDirectoryName(source)!); folder is not null; folder = folder.Parent)
                if (File.Exists(Path.Combine(folder.FullName, ".editorconfig")))
                    throw new NotSupportedException("Custom analyzer configuration currently requires the CLI compiler.");
        for (DirectoryInfo? ancestor = new(Path.GetDirectoryName(request.Project)!); ancestor is not null; ancestor = ancestor.Parent)
            if (File.Exists(Path.Combine(ancestor.FullName, ".editorconfig")))
                throw new NotSupportedException("Custom analyzer configuration currently requires the CLI compiler.");
        _loader?.Unload();
        _loader = new AnalyzerLoader();
        var references = ItemPaths("ReferencePath").Select(path => MetadataReference.CreateFromFile(path)).ToArray();
        if (Property("SignAssembly") == "true" || Property("DelaySign") == "true" ||
            Property("ApplicationIcon").Length != 0 || Property("Win32Resource").Length != 0)
            throw new NotSupportedException("This project's signing or native resources require the CLI compiler.");
        if (!LanguageVersionFacts.TryParse(Property("LangVersion"), out LanguageVersion language))
            throw new NotSupportedException("The project's C# language version is unsupported.");
        var parse = new CSharpParseOptions(language,
            preprocessorSymbols: Property("DefineConstants").Split(';', StringSplitOptions.RemoveEmptyEntries));
        _trees.Clear();
        _additionalHash = null;
        _bindingGenerator = null; _bindingReference = null; _bindingPe = null; _bindingHash = null;
        _sourceHashes.Clear();
        foreach (string source in ItemPaths("Compile"))
        {
            byte[] bytes = File.ReadAllBytes(source);
            _trees[source] = CSharpSyntaxTree.ParseText(SourceText.From(new MemoryStream(bytes), Encoding.UTF8), parse, source);
            _sourceHashes[source] = Convert.ToHexString(SHA256.HashData(bytes));
        }
        if (_trees.Count == 0) throw new InvalidDataException("C# project has no source files.");
        var diagnosticOptions = new Dictionary<string, ReportDiagnostic>();
        foreach (string id in Property("WarningsAsErrors").Split([';', ','], StringSplitOptions.RemoveEmptyEntries))
            diagnosticOptions[DiagnosticId(id)] = ReportDiagnostic.Error;
        foreach (string id in Property("WarningsNotAsErrors").Split([';', ','], StringSplitOptions.RemoveEmptyEntries))
            diagnosticOptions[DiagnosticId(id)] = ReportDiagnostic.Warn;
        foreach (string id in Property("NoWarn").Split([';', ','], StringSplitOptions.RemoveEmptyEntries))
            diagnosticOptions[DiagnosticId(id)] = ReportDiagnostic.Suppress;
        NullableContextOptions nullable = Property("Nullable") switch
        {
            "enable" => NullableContextOptions.Enable,
            "warnings" => NullableContextOptions.Warnings,
            "annotations" => NullableContextOptions.Annotations,
            _ => NullableContextOptions.Disable
        };
        _compilation = CSharpCompilation.Create(Property("AssemblyName"), _trees.Values, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Debug,
                allowUnsafe: Property("AllowUnsafeBlocks") == "true", deterministic: true,
                checkOverflow: Property("CheckForOverflowUnderflow") == "true",
                warningLevel: int.TryParse(Property("WarningLevel"), out int warningLevel) ? warningLevel : 4,
                generalDiagnosticOption: Property("TreatWarningsAsErrors") == "true" ? ReportDiagnostic.Error : ReportDiagnostic.Default,
                specificDiagnosticOptions: diagnosticOptions, nullableContextOptions: nullable));
        string[] analyzerFiles = ItemPaths("Analyzer").ToArray();
        foreach (string path in analyzerFiles) _loader.AddDependencyLocation(path);
        var analyzerReferences = analyzerFiles.Select(path => new AnalyzerFileReference(path, _loader)).ToArray();
        foreach (var reference in analyzerReferences)
            reference.AnalyzerLoadFailed += (_, failure) => throw new InvalidDataException("Analyzer loading failed: " + failure.Message);
        _analyzers = analyzerReferences.SelectMany(reference => reference.GetAnalyzers(LanguageNames.CSharp)).ToImmutableArray();
        var generators = new List<ISourceGenerator>();
        foreach (var reference in analyzerReferences)
        {
            if (Property("PlasmaSeparateBindings") == "true" && Path.GetFileName(reference.FullPath) == "Plasma.Engine.Generator.dll")
            {
                // This is our packaged generator contract, not an inspection of Roslyn internals.
                foreach (Type type in _loader.LoadFromPath(reference.FullPath).GetTypes().Where(t => t.IsDefined(typeof(GeneratorAttribute))))
                {
                    object generator = Activator.CreateInstance(type)!;
                    ISourceGenerator source = generator is IIncrementalGenerator incremental ? incremental.AsSourceGenerator() : (ISourceGenerator)generator;
                    if (type.FullName == "Plasma.Engine.Generator.BindingManifestGenerator") _bindingGenerator = source;
                    else generators.Add(source);
                }
            }
            else generators.AddRange(reference.GetGenerators(LanguageNames.CSharp));
        }
        _driver = CSharpGeneratorDriver.Create(generators,
            ItemPaths("AdditionalFiles").Select(path => (AdditionalText)new FileText(path)), parse,
            new ProjectOptions(Path.GetDirectoryName(request.Project)!));
        _referenceHashes = ItemPaths("ReferencePath").Concat(analyzerFiles)
            .Concat(Property("MSBuildAllProjects").Split(';', StringSplitOptions.RemoveEmptyEntries))
            .Distinct(StringComparer.OrdinalIgnoreCase).ToDictionary(path => path, HashFile, StringComparer.OrdinalIgnoreCase);
    }

    private Dictionary<string, string> _referenceHashes = new(StringComparer.OrdinalIgnoreCase);
    private void CompileBindings(string hash, string root)
    {
        var parse = (CSharpParseOptions)_compilation!.SyntaxTrees.First().Options;
        var bindings = CSharpCompilation.Create("Plasma.ProjectBindings", references: _compilation.References.Where(r => r != _bindingReference),
            options: _compilation.Options.WithOptimizationLevel(OptimizationLevel.Release));
        GeneratorDriver driver = CSharpGeneratorDriver.Create([_bindingGenerator!], _additional, parse, new ProjectOptions(root));
        driver.RunGeneratorsAndUpdateCompilation(bindings, out Compilation generated, out var diagnostics);
        using var pe = new MemoryStream();
        var result = generated.Emit(pe);
        if (!result.Success || diagnostics.Any(d => d.Severity == DiagnosticSeverity.Error))
            throw new InvalidDataException(string.Join('\n', diagnostics.Concat(result.Diagnostics)));
        if (_bindingReference is not null) _compilation = _compilation.RemoveReferences(_bindingReference);
        _bindingPe = pe.ToArray();
        _bindingReference = MetadataReference.CreateFromImage(_bindingPe);
        _compilation = _compilation.AddReferences(_bindingReference);
        _bindingHash = hash;
        _hotReload.Dispose(); _hotReload = new();
    }
    private static string DiagnosticId(string id) => int.TryParse(id.Trim(), out int number) ? $"CS{number:D4}" : id.Trim();
    private bool ReferenceInputsChanged() => _referenceHashes.Any(pair => HashFile(pair.Key) != pair.Value);
    private string Property(string name) => _evaluation!.RootElement.GetProperty("Properties").GetProperty(name).GetString() ?? string.Empty;
    private IEnumerable<string> ItemPaths(string name) => _evaluation!.RootElement.GetProperty("Items").GetProperty(name)
        .EnumerateArray().Select(item => item.GetProperty("FullPath").GetString()!).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal);

    private void CopyDependencies(string output)
    {
        foreach (var reference in _evaluation!.RootElement.GetProperty("Items").GetProperty("ReferencePath").EnumerateArray())
        {
            if (!reference.TryGetProperty("CopyLocal", out var local) || !local.GetString()!.Equals("true", StringComparison.OrdinalIgnoreCase)) continue;
            string path = reference.GetProperty("FullPath").GetString()!;
            File.Copy(path, Path.Combine(output, Path.GetFileName(path)), overwrite: true);
        }
        string deps = Property("ProjectDepsFilePath");
        if (File.Exists(deps)) File.Copy(deps, Path.Combine(output, Path.GetFileName(deps)), overwrite: true);
    }

    private static string ConfigurationHash(string directory, BuildRequest request)
    {
        var entries = new List<string> { request.Project, request.Manifest, request.Configuration };
        void Visit(string folder)
        {
            foreach (string child in Directory.EnumerateDirectories(folder).Order(StringComparer.Ordinal))
            {
                if (Path.GetFileName(child) is "Plugins" or "Intermediate" or "AssetCache" or "bin" or "obj" or ".git" or ".vs") continue;
                Visit(child);
            }
            foreach (string file in Directory.EnumerateFiles(folder).Order(StringComparer.Ordinal))
            {
                string extension = Path.GetExtension(file);
                if (extension is ".csproj" or ".props" or ".targets" or ".editorconfig" or ".config" ||
                    Path.GetFileName(file) is "global.json" or "packages.lock.json" or "Packages.ddl")
                    entries.Add(file + "|" + HashFile(file));
                else if (extension == ".cs") entries.Add(file);
            }
        }
        Visit(directory);
        for (DirectoryInfo? ancestor = new DirectoryInfo(directory).Parent; ancestor is not null; ancestor = ancestor.Parent)
            foreach (string name in new[] { "global.json", "Directory.Build.props", "Directory.Build.targets", "Directory.Packages.props", "NuGet.Config" })
            {
                string file = Path.Combine(ancestor.FullName, name);
                entries.Add(file + "|" + HashFile(file));
            }
        return HashText(string.Join('|', entries));
    }

    private static string HashText(string value) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value)));
    private static string HashFile(string path) => File.Exists(path) ? Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))) : "missing";

    private sealed class FileText(string path) : AdditionalText
    {
        private readonly SourceText _text = SourceText.From(File.ReadAllText(path), Encoding.UTF8);
        public override string Path => path;
        public override SourceText GetText(CancellationToken cancellationToken = default) => _text;
    }

    private sealed class ProjectOptions(string root) : AnalyzerConfigOptionsProvider
    {
        private sealed class Options(string root) : AnalyzerConfigOptions
        {
            public override bool TryGetValue(string key, out string value)
            {
                value = key.Equals("build_property.PlasmaSourceRoot", StringComparison.OrdinalIgnoreCase) ? root : string.Empty;
                return value.Length != 0;
            }
        }
        private readonly Options _options = new(root);
        public override AnalyzerConfigOptions GlobalOptions => _options;
        public override AnalyzerConfigOptions GetOptions(SyntaxTree tree) => _options;
        public override AnalyzerConfigOptions GetOptions(AdditionalText textFile) => _options;
    }

    private sealed class AnalyzerLoader() : AssemblyLoadContext(isCollectible: true), IAnalyzerAssemblyLoader
    {
        private readonly Dictionary<string, string> _paths = new(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, Assembly> _loaded = new(StringComparer.OrdinalIgnoreCase);
        public void AddDependencyLocation(string fullPath) => _paths[Path.GetFileNameWithoutExtension(fullPath)] = fullPath;
        public Assembly LoadFromPath(string fullPath)
        {
            if (_loaded.TryGetValue(fullPath, out Assembly? loaded)) return loaded;
            using var stream = File.OpenRead(fullPath);
            return _loaded[fullPath] = LoadFromStream(stream);
        }
        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name is "Microsoft.CodeAnalysis" or "Microsoft.CodeAnalysis.CSharp")
                return Default.Assemblies.First(assembly => assembly.GetName().Name == name.Name);
            return name.Name is not null && _paths.TryGetValue(name.Name, out string? path) ? LoadFromPath(path) : null;
        }
    }
}
