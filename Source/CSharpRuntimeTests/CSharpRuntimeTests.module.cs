using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class CSharpRuntimeTestsModule : ModuleRules
{
    public CSharpRuntimeTestsModule(BuildContext context) : base(context)
    {
        Type = ModuleType.Runtime;
        SourceDirectory = "Source/CSharpRuntimeTests";
        CppStandard = CppStandard.Cpp20;
        PlasmaPackageSdk.ConfigureSdkConsumer(this, context);
        PublicIncludePaths.Add(Path.Combine(context.ProjectRoot, "Source"));
        PrivateLibraries.Add(Path.Combine(PlasmaPackageSdk.PackageBinaryDirectory(context), "plCSharpPlugin.lib"));
    }
}
