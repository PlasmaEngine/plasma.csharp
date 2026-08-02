using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class CSharpPluginModule : ModuleRules
{
    public CSharpPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageDotNet.EnsureHostPack(context);
        var nativeHostDirectory = PlasmaPackageDotNet.GetHostNativeDirectory(context);

        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/CSharpPlugin", "CSharpPluginPCH.h",
            "BUILDSYSTEM_BUILDING_CSHARPPLUGIN_LIB");

        PrivateIncludePaths.Add(nativeHostDirectory);
        PrivateLibraries.Add(Path.Combine(nativeHostDirectory, "nethost.lib"));

        // nethost.dll sits beside the plugin. The managed assemblies and the private .NET runtime
        // are produced into the same directory by the pre-build step on the target.
        PlasmaPackageDotNet.StageNativeHost(this, context, nativeHostDirectory);
    }
}
