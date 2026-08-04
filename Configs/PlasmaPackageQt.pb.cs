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

    private const string QtDownloadUrl =
        "https://github.com/PlasmaEngine/thirdparty/releases/download/Qt6-6.11.0-vs144-x64/Qt6-6.11.0-vs144-x64.7z";

    private static string s_ResolvedQtPath;

    /// \brief Where a downloaded Qt is kept so every engine and package on this machine shares one copy.
    ///
    /// Deliberately outside both the SDK and the package: an SDK installed by the launcher is
    /// replaced wholesale on upgrade, and a package is a git checkout that should not gain a
    /// half-gigabyte of Qt. `PLASMA_THIRDPARTY_DIR` overrides it for CI, where a writable, pre-warmed
    /// cache is usually mounted somewhere specific.
    private static string SharedThirdPartyRoot
    {
        get
        {
            var custom = Environment.GetEnvironmentVariable("PLASMA_THIRDPARTY_DIR");
            if (!string.IsNullOrWhiteSpace(custom))
            {
                return custom;
            }

            // LocalApplicationData rather than roaming: this is a large binary cache, and a roaming
            // profile would try to sync it.
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "PlasmaBuild", "ThirdParty");
        }
    }

    private static bool HasQt(string dir)
    {
        return !string.IsNullOrWhiteSpace(dir) && File.Exists(Path.Combine(dir, "bin", "moc.exe"));
    }

    /// \brief Qt for code generation, downloaded into the shared cache if it is not already somewhere.
    ///
    /// Order: QTDIR, then the SDK's own copy (a source-built engine already has one, and reusing it
    /// avoids a second download), then the shared cache, then fetch. Only ever reached when a module
    /// actually asks for Qt code generation, so a package with no editor plugin never triggers it.
    public static string QtPath
    {
        get
        {
            if (s_ResolvedQtPath != null)
            {
                return s_ResolvedQtPath;
            }

            var qtDir = Environment.GetEnvironmentVariable("QTDIR");
            if (!string.IsNullOrWhiteSpace(qtDir))
            {
                s_ResolvedQtPath = qtDir;
                return s_ResolvedQtPath;
            }

            var fromSdk = Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "ThirdParty", QtVersionDirectory);
            if (HasQt(fromSdk))
            {
                s_ResolvedQtPath = fromSdk;
                return s_ResolvedQtPath;
            }

            var shared = Path.Combine(SharedThirdPartyRoot, QtVersionDirectory);
            if (HasQt(shared))
            {
                s_ResolvedQtPath = shared;
                return s_ResolvedQtPath;
            }

            if (string.Equals(Environment.GetEnvironmentVariable("PLASMA_NO_DOWNLOAD"), "1", StringComparison.Ordinal))
            {
                throw new DirectoryNotFoundException(
                    $"No Qt found for package code generation, and PLASMA_NO_DOWNLOAD is set. Looked in '{fromSdk}' " +
                    $"and '{shared}'. Set QTDIR, or clear PLASMA_NO_DOWNLOAD to fetch it.");
            }

            DownloadQt(shared);

            if (!HasQt(shared))
            {
                throw new DirectoryNotFoundException(
                    $"Qt was downloaded but '{Path.Combine(shared, "bin", "moc.exe")}' is still missing - the archive " +
                    "layout is not what this build expects.");
            }

            s_ResolvedQtPath = shared;
            return s_ResolvedQtPath;
        }
    }

    /// \brief Fetches and extracts the shared Qt. Mirrors what the engine's own qt.module.cs does.
    private static void DownloadQt(string targetDirectory)
    {
        var root = SharedThirdPartyRoot;
        Directory.CreateDirectory(root);

        var archivePath = Path.Combine(root, QtVersionDirectory + ".7z");
        var extractMarker = targetDirectory + ".extracted";

        if (File.Exists(extractMarker) && HasQt(targetDirectory))
        {
            return;
        }

        // 7z ships with the SDK, including a launcher-installed one, so nothing else has to be present.
        var sevenZip = Path.Combine(PlasmaPackageSdk.Root, "Data", "Tools", "Precompiled", "7z.exe");
        if (!File.Exists(sevenZip))
        {
            throw new FileNotFoundException(
                $"Qt has to be downloaded, but 7z.exe is missing from the SDK at '{sevenZip}'. Set QTDIR to a Qt "
                + "install instead.", sevenZip);
        }

        if (!File.Exists(archivePath))
        {
            Console.WriteLine($"Downloading Qt to the shared cache at '{root}' - this happens once per machine.");

            // Download beside the final name and move, so an interrupted transfer is not mistaken for
            // a complete archive on the next build.
            var partial = archivePath + ".partial";

            using (var httpClient = new System.Net.Http.HttpClient())
            {
                httpClient.Timeout = TimeSpan.FromMinutes(30);

                using var response = httpClient
                    .GetAsync(QtDownloadUrl, System.Net.Http.HttpCompletionOption.ResponseHeadersRead)
                    .GetAwaiter().GetResult();

                response.EnsureSuccessStatusCode();

                using var source = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
                using var target = File.Create(partial);
                source.CopyTo(target);
            }

            File.Move(partial, archivePath, true);
        }

        Console.WriteLine($"Extracting Qt into '{targetDirectory}'.");

        var startInfo = new ProcessStartInfo(sevenZip)
        {
            Arguments = $"x \"{archivePath}\" -o\"{root}\" -y",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        using (var process = Process.Start(startInfo))
        {
            var stdout = process.StandardOutput.ReadToEnd();
            var stderr = process.StandardError.ReadToEnd();
            process.WaitForExit();

            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException(
                    $"7z extraction of Qt failed with exit code {process.ExitCode}.\n{stdout}\n{stderr}");
            }
        }

        File.WriteAllText(extractMarker, QtDownloadUrl);
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
