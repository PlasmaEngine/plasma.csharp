// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaCSharpWorkspace : WorkspaceRules
{
    public PlasmaCSharpWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("CSharpPlugin");
        TargetNames.Add("EditorPluginCSharp");
    }
}
