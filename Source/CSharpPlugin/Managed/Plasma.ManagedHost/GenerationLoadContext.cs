using System.Reflection;
using System.Runtime.Loader;

namespace Plasma.ManagedHost;

internal sealed class GenerationLoadContext : AssemblyLoadContext
{
    private readonly AssemblyDependencyResolver _resolver;
    private readonly Assembly _sharedScriptCore = typeof(ComponentScript).Assembly;
    private readonly string _directory;

    public GenerationLoadContext(string mainAssemblyPath)
        : base($"PlasmaScript-{Path.GetFileNameWithoutExtension(mainAssemblyPath)}", isCollectible: true)
    {
        _resolver = new AssemblyDependencyResolver(mainAssemblyPath);
        _directory = Path.GetDirectoryName(mainAssemblyPath)!;
    }

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        if (AssemblyName.ReferenceMatchesDefinition(assemblyName, _sharedScriptCore.GetName()))
        {
            return _sharedScriptCore;
        }

        string? assemblyPath = _resolver.ResolveAssemblyToPath(assemblyName);
        if (assemblyPath is null && assemblyName.Name == "Plasma.ProjectBindings")
        {
            string bindings = Path.Combine(_directory, "Plasma.ProjectBindings.dll");
            if (File.Exists(bindings)) assemblyPath = bindings;
        }
        return assemblyPath is null ? null : LoadFromAssemblyPath(assemblyPath);
    }

    protected override nint LoadUnmanagedDll(string unmanagedDllName)
    {
        string? libraryPath = _resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
        return libraryPath is null ? 0 : LoadUnmanagedDllFromPath(libraryPath);
    }
}
