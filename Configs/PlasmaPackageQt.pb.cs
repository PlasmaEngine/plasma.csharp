// #pl-version 1
//
// Qt code generation for a package built outside the engine checkout.
//
// The engine's PlasmaBuildQt cannot be reused directly: it derives the Qt install, the moc include
// paths and the generated-code directory from context.ProjectRoot, which out here means the package
// rather than the engine. Left alone it would try to download a second Qt into the package and
// preprocess against the wrong headers. The generation itself is the same - moc over headers that
// carry a Qt macro, rcc over .qrc, uic over .ui - so only the paths change.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageQt
{
    // Must match the Qt the SDK was built with: moc output is tied to the Qt version that consumes it.
    private const string QtVersionDirectory = "Qt6-6.11.0-vs144-x64";

    public static string QtPath
    {
        get
        {
            var qtDir = Environment.GetEnvironmentVariable("QTDIR");
            if (!string.IsNullOrWhiteSpace(qtDir))
            {
                return qtDir;
            }

            var fromSdk = Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "ThirdParty", QtVersionDirectory);

            if (!File.Exists(Path.Combine(fromSdk, "bin", "moc.exe")))
            {
                throw new DirectoryNotFoundException(
                    $"No Qt found for package code generation. Looked in '{fromSdk}'. Build the engine once so the " +
                    "SDK fetches Qt, or set QTDIR.");
            }

            return fromSdk;
        }
    }

    /// \brief Root the generated sources are included from, e.g. <MyModule/PlasmaBuildQtGenerated.inl>.
    private static string GeneratedRoot(BuildContext context)
    {
        return Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild", "Generated", "QtCode");
    }

    /// \brief Runs moc/rcc/uic for a module and wires the results into it.
    public static void ConfigureQtModule(ModuleRules rules, BuildContext context, string moduleName,
        string sourceDirectory, params string[] qtModules)
    {
        var qtPath = QtPath;
        var sourceRoot = Path.Combine(context.ProjectRoot, sourceDirectory);
        var includeRoot = GeneratedRoot(context);
        var outputRoot = Path.Combine(includeRoot, moduleName);

        Directory.CreateDirectory(outputRoot);

        var generated = new List<string>();
        generated.AddRange(InvokeMoc(context, qtPath, sourceRoot, outputRoot));
        generated.AddRange(InvokeRcc(context, qtPath, moduleName, sourceRoot, outputRoot));

        // uic output is the one kind the C++ names for itself, as <PluginDir/ui_Foo.h>, so it has to
        // land under a directory named for the plugin rather than for the module. moc and rcc output
        // is reached through the aggregate include by absolute path, so where it sits does not matter.
        InvokeUic(context, qtPath, sourceRoot, Path.Combine(includeRoot, Path.GetFileName(sourceDirectory)));

        WriteAggregateInclude(outputRoot, generated);

        rules.PublicIncludePaths.Add(includeRoot);
        rules.PublicIncludePaths.Add(outputRoot);
        rules.PrivateDefinitions.Add("BUILDSYSTEM_PLASMABUILD_QT_MOC");
        rules.PublicDefinitions.Add("PL_USE_QT");

        rules.EnableRTTI = true;
        rules.AdditionalCompilerFlags.Add("/Zc:__cplusplus");

        foreach (var module in qtModules)
        {
            rules.PublicIncludePaths.Add(Path.Combine(qtPath, "include", "Qt" + module));
            rules.PublicLibraries.Add(Path.Combine(qtPath, "lib", "Qt6" + module + ".lib"));
        }

        rules.PublicIncludePaths.Add(Path.Combine(qtPath, "include"));
    }

    private static IEnumerable<string> InvokeMoc(BuildContext context, string qtPath, string sourceRoot, string outputRoot)
    {
        var moc = Path.Combine(qtPath, "bin", "moc.exe");
        var results = new List<string>();

        foreach (var header in EnumerateQtMetaObjectHeaders(sourceRoot))
        {
            var relative = Path.GetRelativePath(sourceRoot, header);
            var output = Path.Combine(outputRoot, "moc_" + ToGeneratedName(relative) + ".cpp");

            if (NeedsUpdate(output, header))
            {
                var args = MocIncludeArgs(context, qtPath).Concat(new[] { header, "-o", output }).ToArray();
                Run(moc, args, context.ProjectRoot, $"moc failed for '{header}'");
            }

            results.Add(output);
        }

        return results;
    }

    private static IEnumerable<string> InvokeRcc(BuildContext context, string qtPath, string moduleName, string sourceRoot,
        string outputRoot)
    {
        var rcc = Path.Combine(qtPath, "bin", "rcc.exe");
        var results = new List<string>();

        foreach (var resource in Directory.EnumerateFiles(sourceRoot, "*.qrc", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(sourceRoot, resource);
            var output = Path.Combine(outputRoot, "qrc_" + ToGeneratedName(relative) + ".cpp");

            if (NeedsUpdate(output, resource))
            {
                Run(rcc, new[] { "-name", moduleName + "_" + ToGeneratedName(relative), resource, "-o", output },
                    context.ProjectRoot, $"rcc failed for '{resource}'");
            }

            results.Add(output);
        }

        return results;
    }

    private static void InvokeUic(BuildContext context, string qtPath, string sourceRoot, string uicRoot)
    {
        var uic = Path.Combine(qtPath, "bin", "uic.exe");

        Directory.CreateDirectory(uicRoot);

        foreach (var ui in Directory.EnumerateFiles(sourceRoot, "*.ui", SearchOption.AllDirectories))
        {
            var output = Path.Combine(uicRoot, "ui_" + Path.GetFileNameWithoutExtension(ui) + ".h");

            if (NeedsUpdate(output, ui))
            {
                Run(uic, new[] { ui, "-o", output }, context.ProjectRoot, $"uic failed for '{ui}'");
            }
        }
    }

    private static string[] MocIncludeArgs(BuildContext context, string qtPath)
    {
        return new[]
        {
            "-DWIN32",
            "-D_WIN32",
            "-D_WINDOWS",
            "-D_M_X64",
            "-DPL_USE_QT",
            "-I" + Path.Combine(PlasmaPackageSdk.Root, "Code"),
            "-I" + Path.Combine(PlasmaPackageSdk.Root, "Code", "Engine"),
            "-I" + Path.Combine(PlasmaPackageSdk.Root, "Code", "Editor"),
            "-I" + Path.Combine(PlasmaPackageSdk.Root, "Code", "EditorPlugins"),
            "-I" + Path.Combine(PlasmaPackageSdk.Root, "Code", "Tools", "Libs"),
            "-I" + Path.Combine(context.ProjectRoot, "Source"),
            "-I" + Path.Combine(qtPath, "include"),
            "-I" + Path.Combine(qtPath, "include", "QtCore"),
            "-I" + Path.Combine(qtPath, "include", "QtGui"),
            "-I" + Path.Combine(qtPath, "include", "QtWidgets")
        };
    }

    private static IEnumerable<string> EnumerateQtMetaObjectHeaders(string sourceRoot)
    {
        return Directory.EnumerateFiles(sourceRoot, "*.h", SearchOption.AllDirectories)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Where(ContainsQtMetaObjectMacro)
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase);
    }

    private static bool ContainsQtMetaObjectMacro(string path)
    {
        var text = File.ReadAllText(path);
        return text.Contains("Q_OBJECT", StringComparison.Ordinal) ||
               text.Contains("Q_GADGET", StringComparison.Ordinal) ||
               text.Contains("Q_NAMESPACE", StringComparison.Ordinal);
    }

    private static void WriteAggregateInclude(string outputRoot, IReadOnlyList<string> generatedFiles)
    {
        var aggregate = Path.Combine(outputRoot, "PlasmaBuildQtGenerated.inl");
        var content = string.Join(Environment.NewLine,
            generatedFiles.Select(p => "#include \"" + p.Replace("\\", "/") + "\"")) + Environment.NewLine;

        if (File.Exists(aggregate) && File.ReadAllText(aggregate) == content)
        {
            return;
        }

        File.WriteAllText(aggregate, content);
    }

    private static string ToGeneratedName(string relativePath)
    {
        var builder = new StringBuilder(relativePath.Length);
        foreach (var c in relativePath)
        {
            builder.Append(char.IsLetterOrDigit(c) || c == '_' ? c : '_');
        }

        return builder.ToString();
    }

    private static bool NeedsUpdate(string output, string input)
    {
        return !File.Exists(output) || File.GetLastWriteTimeUtc(output) < File.GetLastWriteTimeUtc(input);
    }

    private static void Run(string fileName, IReadOnlyList<string> arguments, string workingDirectory, string failureMessage)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = fileName,
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = new Process { StartInfo = startInfo };
        process.Start();
        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit();

        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"{failureMessage} (exit code {process.ExitCode}).\n{stdout}\n{stderr}");
        }
    }
}
