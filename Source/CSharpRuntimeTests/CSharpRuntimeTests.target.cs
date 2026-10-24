using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class CSharpRuntimeTestsTarget : TargetRules
{
    public CSharpRuntimeTestsTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "CSharpRuntimeTests";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        TargetDependencies.Add("CSharpPlugin");
        ExtraModules.Add("CSharpRuntimeTestsModule");
    }
}
