using System;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageDotNet
{
    public const string RuntimeVersion = "10.0.10";
    public const string SdkVersion = "10.0.301";

    private const string HostPackageName = "microsoft.netcore.app.host.win-x64." + RuntimeVersion + ".nupkg";
    private const string HostPackageUrl =
        "https://api.nuget.org/v3-flatcontainer/microsoft.netcore.app.host.win-x64/" + RuntimeVersion + "/" + HostPackageName;
    private const string HostPackageSha256 = "39713E65938F3BC8CCEE343DD377E01049844C3484731AB9B29085E650BA19BD";

    public static bool IsCSharpHostSupported(BuildContext context, out string disabledReason)
    {
        disabledReason = string.Empty;

        if (context.Platform != TargetPlatform.Windows)
        {
            disabledReason = "The C# scripting M0 host currently supports Windows desktop only.";
            return false;
        }

        if (context.Architecture == TargetArchitecture.X86 || context.Architecture == TargetArchitecture.ARM64)
        {
            disabledReason = "The C# scripting M0 host currently supports Windows x64 only.";
            return false;
        }

        return true;
    }

    public static string GetHostPackRoot(BuildContext context)
    {
        return Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild", "ThirdParty",
            "DotNetHost-win-x64-" + RuntimeVersion);
    }

    public static string GetHostNativeDirectory(BuildContext context)
    {
        return Path.Combine(GetHostPackRoot(context), "runtimes", "win-x64", "native");
    }

    public static void EnsureHostPack(BuildContext context)
    {
        if (!IsCSharpHostSupported(context, out var disabledReason))
        {
            throw new InvalidOperationException(disabledReason);
        }

        var hostRoot = GetHostPackRoot(context);
        if (HasExpectedHostFiles(hostRoot))
        {
            return;
        }

        var thirdPartyRoot = Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild", "ThirdParty");
        var packagePath = Path.Combine(thirdPartyRoot, HostPackageName);
        var markerPath = Path.Combine(hostRoot, ".extracted");

        Directory.CreateDirectory(thirdPartyRoot);

        if (!File.Exists(packagePath) || !HasExpectedHash(packagePath))
        {
            File.Delete(packagePath);
            DownloadFile(HostPackageUrl, packagePath);
        }

        if (!HasExpectedHash(packagePath))
        {
            throw new InvalidDataException(
                $"Downloaded .NET host pack '{packagePath}' did not match the pinned SHA-256 {HostPackageSha256}.");
        }

        if (Directory.Exists(hostRoot) && (!File.Exists(markerPath) || !HasExpectedHostFiles(hostRoot)))
        {
            Directory.Delete(hostRoot, recursive: true);
        }

        if (!Directory.Exists(hostRoot))
        {
            Directory.CreateDirectory(hostRoot);
            ZipFile.ExtractToDirectory(packagePath, hostRoot, overwriteFiles: true);
            File.WriteAllText(markerPath, RuntimeVersion);
        }

        if (!HasExpectedHostFiles(hostRoot))
        {
            throw new InvalidDataException(
                $"The pinned .NET host pack extracted to '{hostRoot}', but required nethost/hostfxr files are missing.");
        }
    }

    /// \brief Publishes Plasma.ScriptInspector into CSharp/Tools beside the editor plugin.
    ///
    /// The editor looks for CSharp/Tools/Plasma.ScriptInspector.dll and, failing that, builds it
    /// from a csproj under the engine's Code/ - a path a package user does not have. Shipping the
    /// built tool means the fallback is never needed.
    public static void AddInspectorBuildStep(TargetRules rules, BuildContext context)
    {
        if (!IsCSharpHostSupported(context, out _))
        {
            return;
        }

        var project = Path.Combine(context.ProjectRoot, "Source", "EditorManaged",
            "Plasma.ScriptInspector", "Plasma.ScriptInspector.csproj");
        var output = Path.Combine(PlasmaPackageSdk.PackageBinaryDirectory(context), "CSharp", "Tools");
        var configuration = context.Configuration == BuildConfiguration.Debug ? "Debug" : "Release";

        rules.PreBuildSteps.Add(new BuildStep
        {
            Description = "Build the managed C# descriptor inspector",
            Command = $"dotnet build {Quote(project)} --nologo --configuration {configuration} --output {Quote(output)}",
            WorkingDirectory = context.ProjectRoot
        });
    }

    /// \brief Copies nethost.dll beside the plugin.
    ///
    /// The engine does this through PlasmaBuildRuntimeFiles, which writes into Binaries/ and leaves
    /// a manifest for the deployment step. Neither exists out here, so the copy is the whole job.
    public static void StageNativeHost(ModuleRules rules, BuildContext context, string nativeHostDirectory)
    {
        var source = Path.Combine(nativeHostDirectory, "nethost.dll");
        var target = Path.Combine(PlasmaPackageSdk.PackageBinaryDirectory(context), "nethost.dll");

        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"Missing nethost.dll at '{source}'.", source);
        }

        if (File.Exists(target))
        {
            var from = new FileInfo(source);
            var to = new FileInfo(target);

            if (from.Length == to.Length && from.LastWriteTimeUtc <= to.LastWriteTimeUtc)
            {
                return;
            }
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target));
        File.Copy(source, target, overwrite: true);
    }

    public static void AddManagedM0BuildSteps(TargetRules rules, BuildContext context)
    {
        if (!IsCSharpHostSupported(context, out _))
        {
            return;
        }

        var binaryRoot = Path.Combine(PlasmaPackageSdk.PackageBinaryDirectory(context), "CSharp");
        var managedConfiguration = context.Configuration == BuildConfiguration.Debug ? "Debug" : "Release";
        var runtimePublishRoot = Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild",
            "DotNetRuntime", $"{context.Platform}_{context.Configuration}");
        var privateRuntimeRoot = Path.Combine(PlasmaPackageSdk.PackageBinaryDirectory(context), "DotNet");
        var buildScript = Path.Combine(context.ProjectRoot, "Scripts", "prepare-csharp-runtime.ps1");
        rules.PreBuildSteps.Add(new BuildStep
        {
            Description = "Build managed C# runtime and private .NET layout",
            Command =
                $"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {Quote(buildScript)} " +
                $"-ProjectRoot {Quote(context.ProjectRoot)} -Configuration {managedConfiguration} " +
                $"-BinaryRoot {Quote(binaryRoot)} -RuntimePublishRoot {Quote(runtimePublishRoot)} " +
                $"-PrivateRuntimeRoot {Quote(privateRuntimeRoot)} " +
                $"-RuntimeVersion {RuntimeVersion}",
            WorkingDirectory = context.ProjectRoot
        });
    }

    private static bool HasExpectedHostFiles(string hostRoot)
    {
        var native = Path.Combine(hostRoot, "runtimes", "win-x64", "native");
        return File.Exists(Path.Combine(native, "nethost.h")) &&
            File.Exists(Path.Combine(native, "hostfxr.h")) &&
            File.Exists(Path.Combine(native, "coreclr_delegates.h")) &&
            File.Exists(Path.Combine(native, "nethost.lib")) &&
            File.Exists(Path.Combine(native, "nethost.dll"));
    }

    private static bool HasExpectedHash(string path)
    {
        if (!File.Exists(path))
        {
            return false;
        }

        using var stream = File.OpenRead(path);
        var actual = Convert.ToHexString(SHA256.HashData(stream));
        return string.Equals(actual, HostPackageSha256, StringComparison.OrdinalIgnoreCase);
    }

    private static void DownloadFile(string url, string targetPath)
    {
        using var client = new HttpClient();
        using var response = client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
        response.EnsureSuccessStatusCode();

        using var source = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
        using var target = File.Create(targetPath);
        source.CopyTo(target);
    }

    private static string Quote(string value)
    {
        return "\"" + value.Replace("\"", "\\\"") + "\"";
    }
}
