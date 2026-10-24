using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginCSharpModule : ModuleRules
{
    public EditorPluginCSharpModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginCSharp",
            "EditorPluginCSharpPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINCSHARP_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/CSharpPlugin", "plCSharpPlugin");

        // The build diagnostics view is engine-rendered (table, text views, tabs).
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");
    }
}
