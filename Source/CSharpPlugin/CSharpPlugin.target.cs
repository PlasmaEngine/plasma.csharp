using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class CSharpPluginTarget : TargetRules
{
    public CSharpPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plCSharpPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("CSharpPluginModule");

        // Builds the managed assemblies and lays out the private .NET runtime beside the plugin.
        PlasmaPackageDotNet.AddManagedM0BuildSteps(this, context);
    }
}
