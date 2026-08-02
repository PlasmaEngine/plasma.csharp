using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginCSharpTarget : TargetRules
{
    public EditorPluginCSharpTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginCSharp";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        TargetDependencies.Add("CSharpPlugin");

        ExtraModules.Add("EditorPluginCSharpModule");

        // Ships the descriptor inspector the C# project asset shells out to.
        PlasmaPackageDotNet.AddInspectorBuildStep(this, context);
    }
}
