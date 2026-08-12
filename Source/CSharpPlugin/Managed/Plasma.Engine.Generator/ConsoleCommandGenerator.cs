using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Plasma.Engine.Generator;

/// <summary>
/// Emits a provider for every <c>[ConsoleCommand]</c> method so the host can register them without
/// reflecting over the assembly at load time.
/// </summary>
/// <remarks>
/// Deliberately a separate generator from <see cref="ScriptDescriptorGenerator"/>: console commands
/// are not script types, they are found on plain static methods anywhere in the project, and keeping
/// the two pipelines apart means neither can break the other.
/// </remarks>
[Generator(LanguageNames.CSharp)]
public sealed class ConsoleCommandGenerator : IIncrementalGenerator
{
    private const string ConsoleCommandAttributeName = "Plasma.ConsoleCommandAttribute";

    private static readonly DiagnosticDescriptor MustBeStatic = new(
        "PLCS020",
        "Console command must be static",
        "Console command '{0}' must be a static method",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor MustBeAccessible = new(
        "PLCS021",
        "Console command must be accessible",
        "Console command '{0}' must be public or internal so the generated provider can call it",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor UnsupportedParameter = new(
        "PLCS022",
        "Unsupported console command parameter",
        "Console command '{0}' parameter '{1}' has type '{2}'; the console interpreter only marshals bool, int, uint, float, double and string",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidName = new(
        "PLCS023",
        "Invalid console command name",
        "Console command name '{0}' is invalid; use a non-empty name without whitespace",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor DuplicateName = new(
        "PLCS024",
        "Duplicate console command name",
        "Console command '{0}' is declared more than once; names must be unique because the console resolves by name",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor TooManyParameters = new(
        "PLCS025",
        "Too many console command parameters",
        "Console command '{0}' takes {1} parameters; the console supports at most 6",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private const string ConsoleToolAttributeName = "Plasma.ConsoleToolAttribute";
    private const string ConsoleToolBaseName = "Plasma.ConsoleTool";

    private static readonly DiagnosticDescriptor ToolInvalidShape = new(
        "PLCS026",
        "Invalid console tool shape",
        "Console tool '{0}' must be a concrete, non-generic class deriving from Plasma.ConsoleTool with a public parameterless constructor",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ToolInvalidName = new(
        "PLCS027",
        "Invalid console tool name",
        "Console tool name '{0}' is invalid; use a non-empty name",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ToolDuplicateName = new(
        "PLCS028",
        "Duplicate console tool name",
        "Console tool '{0}' is declared more than once; the console saves window state by name",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<IMethodSymbol> commands = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                ConsoleCommandAttributeName,
                static (node, _) => node is MethodDeclarationSyntax,
                static (attributeContext, _) => (IMethodSymbol)attributeContext.TargetSymbol);

        context.RegisterSourceOutput(commands.Collect(), static (sourceContext, symbols) => Emit(sourceContext, symbols));

        IncrementalValuesProvider<INamedTypeSymbol> tools = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                ConsoleToolAttributeName,
                static (node, _) => node is ClassDeclarationSyntax,
                static (attributeContext, _) => (INamedTypeSymbol)attributeContext.TargetSymbol);

        context.RegisterSourceOutput(tools.Collect(), static (sourceContext, symbols) => EmitTools(sourceContext, symbols));
    }

    private static void EmitTools(SourceProductionContext context, ImmutableArray<INamedTypeSymbol> symbols)
    {
        if (symbols.IsDefaultOrEmpty)
        {
            return;
        }

        var models = new List<ToolModel>();
        var seenNames = new HashSet<string>(System.StringComparer.Ordinal);

        foreach (INamedTypeSymbol type in symbols
                     .OrderBy(symbol => symbol.ToDisplayString(), System.StringComparer.Ordinal))
        {
            AttributeData? attribute = type.GetAttributes().FirstOrDefault(candidate =>
                candidate.AttributeClass?.ToDisplayString() == ConsoleToolAttributeName);
            if (attribute is null)
            {
                continue;
            }

            Location location = type.Locations.FirstOrDefault() ?? Location.None;

            string name = attribute.ConstructorArguments.Length > 0
                ? attribute.ConstructorArguments[0].Value as string ?? string.Empty
                : string.Empty;
            string category = "Tools";
            bool pinned = false;
            foreach (KeyValuePair<string, TypedConstant> named in attribute.NamedArguments)
            {
                if (named.Key == "Category" && named.Value.Value is string categoryValue &&
                    categoryValue.Length > 0)
                {
                    category = categoryValue;
                }
                else if (named.Key == "Pinned" && named.Value.Value is bool pinnedValue)
                {
                    pinned = pinnedValue;
                }
            }

            if (string.IsNullOrWhiteSpace(name))
            {
                context.ReportDiagnostic(Diagnostic.Create(ToolInvalidName, location, name));
                continue;
            }

            if (!seenNames.Add(name))
            {
                context.ReportDiagnostic(Diagnostic.Create(ToolDuplicateName, location, name));
                continue;
            }

            if (type.IsAbstract || type.IsGenericType || type.TypeKind != TypeKind.Class ||
                !DerivesFromConsoleTool(type) || !HasPublicParameterlessConstructor(type))
            {
                context.ReportDiagnostic(Diagnostic.Create(ToolInvalidShape, location, name));
                continue;
            }

            models.Add(new ToolModel(name, category, pinned, type.ToDisplayString()));
        }

        if (models.Count == 0)
        {
            return;
        }

        context.AddSource("PlasmaGeneratedConsoleTools.g.cs", RenderTools(models));
    }

    private static bool DerivesFromConsoleTool(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? current = type.BaseType; current is not null; current = current.BaseType)
        {
            if (current.ToDisplayString() == ConsoleToolBaseName)
            {
                return true;
            }
        }

        return false;
    }

    private static bool HasPublicParameterlessConstructor(INamedTypeSymbol type) =>
        type.InstanceConstructors.Any(constructor =>
            constructor.Parameters.Length == 0 &&
            constructor.DeclaredAccessibility == Accessibility.Public);

    private static string RenderTools(IReadOnlyList<ToolModel> tools)
    {
        var source = new StringBuilder("// <auto-generated />\n#nullable enable\n");
        source.AppendLine("namespace Plasma.Generated");
        source.AppendLine("{");
        source.AppendLine("    [global::System.CodeDom.Compiler.GeneratedCode(\"Plasma.Engine.Generator\", \"1.0\")]");
        source.AppendLine("    public sealed class PlasmaGeneratedConsoleToolProvider : global::Plasma.IScriptToolProvider");
        source.AppendLine("    {");
        source.AppendLine("        public global::System.Collections.Generic.IReadOnlyList<global::Plasma.ScriptToolDescriptor> GetTools()");
        source.AppendLine("        {");
        source.AppendLine("            return new global::Plasma.ScriptToolDescriptor[]");
        source.AppendLine("            {");

        foreach (ToolModel tool in tools)
        {
            source.AppendLine("                new global::Plasma.ScriptToolDescriptor(");
            source.Append("                    global::Plasma.StableId.ConsoleTool(\"")
                .Append(Escape(tool.Name)).AppendLine("\"),");
            source.Append("                    \"").Append(Escape(tool.Name)).AppendLine("\",");
            source.Append("                    \"").Append(Escape(tool.Category)).AppendLine("\",");
            source.Append("                    ").Append(tool.Pinned ? "true" : "false").AppendLine(",");
            source.Append("                    static () => new global::").Append(tool.TypeName).AppendLine("()),");
        }

        source.AppendLine("            };");
        source.AppendLine("        }");
        source.AppendLine("    }");
        source.AppendLine("}");
        return source.ToString();
    }

    private sealed class ToolModel
    {
        public ToolModel(string name, string category, bool pinned, string typeName)
        {
            Name = name;
            Category = category;
            Pinned = pinned;
            TypeName = typeName;
        }

        public string Name { get; }
        public string Category { get; }
        public bool Pinned { get; }
        public string TypeName { get; }
    }

    private static void Emit(SourceProductionContext context, ImmutableArray<IMethodSymbol> symbols)
    {
        if (symbols.IsDefaultOrEmpty)
        {
            return;
        }

        var models = new List<CommandModel>();
        var seenNames = new HashSet<string>(System.StringComparer.Ordinal);

        foreach (IMethodSymbol method in symbols
                     .OrderBy(symbol => symbol.ToDisplayString(), System.StringComparer.Ordinal))
        {
            if (TryBuild(context, method, seenNames, out CommandModel model))
            {
                models.Add(model);
            }
        }

        if (models.Count == 0)
        {
            return;
        }

        context.AddSource("PlasmaGeneratedConsoleCommands.g.cs", Render(models));
    }

    private static bool TryBuild(
        SourceProductionContext context,
        IMethodSymbol method,
        HashSet<string> seenNames,
        out CommandModel model)
    {
        model = default!;

        AttributeData? attribute = method.GetAttributes().FirstOrDefault(candidate =>
            candidate.AttributeClass?.ToDisplayString() == ConsoleCommandAttributeName);
        if (attribute is null)
        {
            return false;
        }

        string name = attribute.ConstructorArguments.Length > 0
            ? attribute.ConstructorArguments[0].Value as string ?? string.Empty
            : string.Empty;
        string help = attribute.ConstructorArguments.Length > 1
            ? attribute.ConstructorArguments[1].Value as string ?? string.Empty
            : string.Empty;

        Location location = method.Locations.FirstOrDefault() ?? Location.None;

        if (string.IsNullOrWhiteSpace(name) || name.Any(char.IsWhiteSpace))
        {
            context.ReportDiagnostic(Diagnostic.Create(InvalidName, location, name));
            return false;
        }

        if (!seenNames.Add(name))
        {
            context.ReportDiagnostic(Diagnostic.Create(DuplicateName, location, name));
            return false;
        }

        if (!method.IsStatic)
        {
            context.ReportDiagnostic(Diagnostic.Create(MustBeStatic, location, name));
            return false;
        }

        if (method.DeclaredAccessibility is Accessibility.Private or Accessibility.Protected or
            Accessibility.ProtectedAndInternal)
        {
            context.ReportDiagnostic(Diagnostic.Create(MustBeAccessible, location, name));
            return false;
        }

        if (method.Parameters.Length > 6)
        {
            context.ReportDiagnostic(
                Diagnostic.Create(TooManyParameters, location, name, method.Parameters.Length));
            return false;
        }

        var parameters = new List<ParameterModel>();
        foreach (IParameterSymbol parameter in method.Parameters)
        {
            string? kind = MapParameterKind(parameter.Type);
            if (kind is null || parameter.RefKind != RefKind.None)
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    UnsupportedParameter, location, name, parameter.Name, parameter.Type.ToDisplayString()));
                return false;
            }

            parameters.Add(new ParameterModel(kind, parameter.Type.ToDisplayString()));
        }

        model = new CommandModel(
            name,
            help,
            method.ContainingType.ToDisplayString(),
            method.Name,
            parameters);
        return true;
    }

    private static string? MapParameterKind(ITypeSymbol type) => type.SpecialType switch
    {
        SpecialType.System_Boolean => "Bool",
        SpecialType.System_Int32 => "Int",
        SpecialType.System_UInt32 => "UInt",
        SpecialType.System_Single => "Float",
        SpecialType.System_Double => "Double",
        SpecialType.System_String => "String",
        _ => null,
    };

    private static string Render(IReadOnlyList<CommandModel> commands)
    {
        var source = new StringBuilder("// <auto-generated />\n#nullable enable\n");
        source.AppendLine("namespace Plasma.Generated");
        source.AppendLine("{");
        source.AppendLine("    [global::System.CodeDom.Compiler.GeneratedCode(\"Plasma.Engine.Generator\", \"1.0\")]");
        source.AppendLine("    public sealed class PlasmaGeneratedConsoleCommandProvider : global::Plasma.IScriptCommandProvider");
        source.AppendLine("    {");
        source.AppendLine("        public global::System.Collections.Generic.IReadOnlyList<global::Plasma.ScriptCommandDescriptor> GetCommands()");
        source.AppendLine("        {");
        source.AppendLine("            return new global::Plasma.ScriptCommandDescriptor[]");
        source.AppendLine("            {");

        foreach (CommandModel command in commands)
        {
            source.AppendLine("                new global::Plasma.ScriptCommandDescriptor(");
            source.Append("                    global::Plasma.StableId.ConsoleCommand(\"")
                .Append(Escape(command.Name)).AppendLine("\"),");
            source.Append("                    \"").Append(Escape(command.Name)).AppendLine("\",");
            source.Append("                    \"").Append(Escape(command.Help)).AppendLine("\",");
            source.Append("                    new global::Plasma.ScriptCommandParameterKind[] { ");
            source.Append(string.Join(", ", command.Parameters.Select(static parameter =>
                "global::Plasma.ScriptCommandParameterKind." + parameter.Kind)));
            source.AppendLine(" },");
            source.AppendLine("                    static values =>");
            source.AppendLine("                    {");
            source.Append("                        global::").Append(command.DeclaringType).Append('.')
                .Append(EscapeIdentifier(command.MethodName)).Append('(');
            source.Append(string.Join(", ", command.Parameters.Select(static (parameter, index) =>
                $"({parameter.TypeName})values[{index}]!")));
            source.AppendLine(");");
            source.AppendLine("                    }),");
        }

        source.AppendLine("            };");
        source.AppendLine("        }");
        source.AppendLine("    }");
        source.AppendLine("}");
        return source.ToString();
    }

    private static string Escape(string value) =>
        value.Replace("\\", "\\\\").Replace("\"", "\\\"");

    private static string EscapeIdentifier(string value) => "@" + value;

    // Plain classes rather than records: the generator targets netstandard2.0, which has no
    // IsExternalInit, so init-only setters do not compile here.
    private sealed class ParameterModel
    {
        public ParameterModel(string kind, string typeName)
        {
            Kind = kind;
            TypeName = typeName;
        }

        public string Kind { get; }
        public string TypeName { get; }
    }

    private sealed class CommandModel
    {
        public CommandModel(
            string name,
            string help,
            string declaringType,
            string methodName,
            IReadOnlyList<ParameterModel> parameters)
        {
            Name = name;
            Help = help;
            DeclaringType = declaringType;
            MethodName = methodName;
            Parameters = parameters;
        }

        public string Name { get; }
        public string Help { get; }
        public string DeclaringType { get; }
        public string MethodName { get; }
        public IReadOnlyList<ParameterModel> Parameters { get; }
    }
}
