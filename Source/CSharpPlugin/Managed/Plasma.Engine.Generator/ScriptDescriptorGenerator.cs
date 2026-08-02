using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Plasma.Engine.Generator;

[Generator(LanguageNames.CSharp)]
public sealed class ScriptDescriptorGenerator : IIncrementalGenerator
{
    private static readonly Guid ScriptPersistentIdNamespace =
        new("6ba7b811-9dad-11d1-80b4-00c04fd430c8");

    private const string ScriptAttributeName = "Plasma.PlasmaScriptAttribute";
    private const string ExposeAttributeName = "Plasma.ExposeAttribute";
    private const string MessageAttributeName = "Plasma.MessageHandlerAttribute";
    private const string CustomMessageAttributeName = "Plasma.PlasmaMessageAttribute";
    private const string GeneratedMessageAttributeName = "Plasma.GeneratedMessageAttribute";
    private const string ComponentScriptName = "Plasma.ComponentScript";

    private static readonly DiagnosticDescriptor MustBePartial = new(
        "PLCS001",
        "Script class must be partial",
        "Plasma script '{0}' must be declared partial so its descriptor can be generated",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidShape = new(
        "PLCS002",
        "Invalid script class shape",
        "Plasma script '{0}' must be a concrete, sealed, non-generic class",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidBase = new(
        "PLCS003",
        "Invalid script base class",
        "Plasma script '{0}' must derive from Plasma.ComponentScript",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor MissingConstructor = new(
        "PLCS004",
        "Script needs an accessible parameterless constructor",
        "Plasma script '{0}' must have an accessible parameterless constructor",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidPersistentId = new(
        "PLCS005",
        "Invalid persistent script ID",
        "Plasma script '{0}' has invalid explicit persistent ID override '{1}'; omit Id for automatic identity or supply a non-empty GUID",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidExposedMember = new(
        "PLCS006",
        "Invalid exposed member",
        "Exposed member '{0}.{1}' cannot be generated: {2}",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidMessageHandler = new(
        "PLCS007",
        "Invalid message handler",
        "Message handler '{0}.{1}' must be an instance void method with exactly one generated native message or [PlasmaMessage] value or 'in' parameter",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor DuplicateMessageHandler = new(
        "PLCS008",
        "Duplicate message handler",
        "Plasma script '{0}' declares more than one handler for message type '{1}'",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor DuplicatePersistentId = new(
        "PLCS009",
        "Duplicate persistent script ID",
        "Plasma script '{0}' shares persistent ID '{1}' with another script in this assembly",
        "Plasma.Scripting",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<INamedTypeSymbol> scripts = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                ScriptAttributeName,
                static (node, _) => node is ClassDeclarationSyntax,
                static (attributeContext, _) => (INamedTypeSymbol)attributeContext.TargetSymbol);

        IncrementalValuesProvider<ManifestMessageCatalog> manifestMessages =
            context.AdditionalTextsProvider
                .Where(static file =>
                    Path.GetFileName(file.Path).Equals(
                        "PlasmaBindings.json",
                        StringComparison.OrdinalIgnoreCase))
                .Select(static (file, cancellationToken) =>
                    ManifestMessageCatalog.Parse(file.GetText(cancellationToken)?.ToString()));

        IncrementalValueProvider<string> sourceRoot =
            context.AnalyzerConfigOptionsProvider.Select(static (options, _) =>
                options.GlobalOptions.TryGetValue(
                    "build_property.PlasmaSourceRoot",
                    out string? configuredRoot)
                    ? configuredRoot
                    : string.Empty);

        context.RegisterSourceOutput(
            scripts.Collect().Combine(manifestMessages.Collect()).Combine(sourceRoot),
            static (sourceContext, inputs) =>
                EmitDescriptors(
                    sourceContext,
                    inputs.Left.Left,
                    inputs.Left.Right,
                    inputs.Right));
    }

    private static void EmitDescriptors(
        SourceProductionContext context,
        ImmutableArray<INamedTypeSymbol> scriptSymbols,
        ImmutableArray<ManifestMessageCatalog> manifestCatalogs,
        string sourceRoot)
    {
        var manifestMessages = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (ManifestMessageCatalog catalog in manifestCatalogs)
        {
            foreach (KeyValuePair<string, string> message in catalog.Messages)
            {
                manifestMessages[message.Key] = message.Value;
            }
        }

        INamedTypeSymbol[] uniqueScripts = scriptSymbols
            .OrderBy(static symbol => symbol.ToDisplayString(), StringComparer.Ordinal)
            .ToArray();

        var validScripts = new List<ScriptModel>(uniqueScripts.Length);
        foreach (INamedTypeSymbol script in uniqueScripts)
        {
            ScriptModel? model = BuildModel(context, script, manifestMessages, sourceRoot);
            if (model is not null)
            {
                validScripts.Add(model);
            }
        }

        string[] duplicateIds = validScripts
            .GroupBy(static model => model.PersistentId, StringComparer.OrdinalIgnoreCase)
            .Where(static group => group.Count() > 1)
            .Select(static group => group.Key)
            .ToArray();
        foreach (ScriptModel script in validScripts.Where(model =>
                     duplicateIds.Contains(model.PersistentId, StringComparer.OrdinalIgnoreCase)))
        {
            context.ReportDiagnostic(Diagnostic.Create(
                DuplicatePersistentId,
                script.Location,
                script.SimpleName,
                script.PersistentId));
        }
        validScripts.RemoveAll(model =>
            duplicateIds.Contains(model.PersistentId, StringComparer.OrdinalIgnoreCase));

        context.AddSource(
            "PlasmaGeneratedScriptDescriptors.g.cs",
            SourceText.From(Render(validScripts), Encoding.UTF8));
    }

    private static ScriptModel? BuildModel(
        SourceProductionContext context,
        INamedTypeSymbol script,
        IReadOnlyDictionary<string, string> manifestMessages,
        string sourceRoot)
    {
        bool valid = true;
        Location location = script.Locations.FirstOrDefault() ?? Location.None;

        bool isPartial = script.DeclaringSyntaxReferences
            .Select(static reference => reference.GetSyntax())
            .OfType<ClassDeclarationSyntax>()
            .Any(static declaration => declaration.Modifiers.Any(SyntaxKind.PartialKeyword));
        if (!isPartial)
        {
            context.ReportDiagnostic(Diagnostic.Create(MustBePartial, location, script.Name));
            valid = false;
        }

        if (!script.IsSealed || script.IsAbstract || script.IsStatic || script.Arity != 0 ||
            script.ContainingType is not null)
        {
            context.ReportDiagnostic(Diagnostic.Create(InvalidShape, location, script.Name));
            valid = false;
        }

        if (!DerivesFrom(script, ComponentScriptName))
        {
            context.ReportDiagnostic(Diagnostic.Create(InvalidBase, location, script.Name));
            valid = false;
        }

        bool hasConstructor = script.InstanceConstructors.Any(static constructor =>
            constructor.Parameters.Length == 0 &&
            constructor.DeclaredAccessibility is Accessibility.Public or Accessibility.Internal);
        if (!hasConstructor)
        {
            context.ReportDiagnostic(Diagnostic.Create(MissingConstructor, location, script.Name));
            valid = false;
        }

        string managedName = script.ToDisplayString(
            SymbolDisplayFormat.CSharpErrorMessageFormat);
        string qualifiedName = script.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
        string metadataName = script.ContainingNamespace.IsGlobalNamespace
            ? script.MetadataName
            : script.ContainingNamespace.ToDisplayString() + "." + script.MetadataName;
        string persistentId = CreateAutomaticPersistentId(
                script.ContainingAssembly.Identity.Name,
                metadataName)
            .ToString("D", CultureInfo.InvariantCulture);

        AttributeData? scriptAttribute = FindAttribute(script, ScriptAttributeName);
        KeyValuePair<string, TypedConstant>? idOverride = scriptAttribute?.NamedArguments
            .Where(static pair => pair.Key == "Id")
            .Select(static pair => (KeyValuePair<string, TypedConstant>?)pair)
            .FirstOrDefault();
        if (idOverride.HasValue)
        {
            string? id = idOverride.Value.Value.Value as string;
            if (string.IsNullOrWhiteSpace(id) ||
                !Guid.TryParse(id, out Guid parsed) ||
                parsed == Guid.Empty)
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidPersistentId,
                    location,
                    script.Name,
                    id ?? "<null>"));
                valid = false;
            }
            else
            {
                persistentId = parsed.ToString("D", CultureInfo.InvariantCulture);
            }
        }

        LifecycleModel[] lifecycle = new[]
        {
            "Initialize",
            "Deinitialize",
            "OnActivated",
            "OnDeactivated",
            "OnSimulationStarted",
            "Update",
        }.Select(methodName => CreateLifecycleModel(script, methodName))
         .Where(static model => model is not null)
         .Cast<LifecycleModel>()
         .ToArray();

        var fields = new List<FieldModel>();
        foreach (ISymbol member in script.GetMembers()
                     .Where(static member => FindAttribute(member, ExposeAttributeName) is not null)
                     .OrderBy(static member => member.Name, StringComparer.Ordinal))
        {
            ITypeSymbol? valueType = null;
            bool isField = false;
            string? invalidReason = null;

            if (member is IFieldSymbol field)
            {
                valueType = field.Type;
                isField = true;

                if (field.IsStatic)
                {
                    invalidReason = "fields must be instance members";
                }
                else if (field.IsConst || field.IsReadOnly)
                {
                    invalidReason = "fields must be writable";
                }
                else if (!IsAccessibleFromDescriptorProvider(field))
                {
                    invalidReason =
                        "the field must be public, internal, or protected internal";
                }
            }
            else if (member is IPropertySymbol property)
            {
                valueType = property.Type;

                if (property.IsStatic)
                {
                    invalidReason = "properties must be instance members";
                }
                else if (property.IsIndexer)
                {
                    invalidReason = "indexers are not supported";
                }
                else if (!property.ExplicitInterfaceImplementations.IsEmpty)
                {
                    invalidReason = "explicit interface implementations are not supported";
                }
                else if (property.GetMethod is null || property.SetMethod is null)
                {
                    invalidReason = "properties must have both a getter and a setter";
                }
                else if (property.SetMethod.IsInitOnly)
                {
                    invalidReason = "init-only properties are not writable after construction";
                }
                else if (!IsAccessibleFromDescriptorProvider(property) ||
                         !IsAccessibleFromDescriptorProvider(property.GetMethod) ||
                         !IsAccessibleFromDescriptorProvider(property.SetMethod))
                {
                    invalidReason =
                        "the property and both accessors must be public, internal, or protected internal";
                }
            }
            else
            {
                invalidReason = "only fields and properties can be exposed";
            }

            if (invalidReason is null &&
                valueType is not null &&
                !IsSupportedExposedType(valueType))
            {
                invalidReason =
                    $"type '{valueType.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat)}' is not supported";
            }

            if (invalidReason is not null || valueType is null)
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidExposedMember,
                    member.Locations.FirstOrDefault() ?? location,
                    script.Name,
                    member.Name,
                    invalidReason ?? "the member is not a field or property"));
                valid = false;
                continue;
            }

            fields.Add(new FieldModel(
                member.Name,
                valueType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                isField,
                GetEditorMetadata(member)));
        }

        var messages = new List<MessageModel>();
        var handledMessages = new HashSet<string>(StringComparer.Ordinal);
        foreach (IMethodSymbol method in script.GetMembers()
                     .OfType<IMethodSymbol>()
                     .Where(static method => FindAttribute(method, MessageAttributeName) is not null)
                     .OrderBy(static method => method.Name, StringComparer.Ordinal))
        {
            bool signatureValid =
                !method.IsStatic &&
                method.ReturnsVoid &&
                method.Parameters.Length == 1 &&
                method.Parameters[0].RefKind is RefKind.None or RefKind.In;
            IParameterSymbol? parameter = signatureValid ? method.Parameters[0] : null;
            AttributeData? generatedMessage = parameter is null
                ? null
                : FindAttribute(parameter.Type, GeneratedMessageAttributeName);
            AttributeData? customMessage = parameter is null
                ? null
                : FindAttribute(parameter.Type, CustomMessageAttributeName);
            string? nativeTypeName =
                generatedMessage?.ConstructorArguments.Length == 1 &&
                generatedMessage.ConstructorArguments[0].Value is string name
                    ? name
                    : null;
            ulong messageId = 0;
            string? messageIdentity = null;
            string? messageDisplayName = null;
            string? qualifiedParameterType = parameter?.Type.ToDisplayString(
                SymbolDisplayFormat.FullyQualifiedFormat);
            if (parameter?.Type.TypeKind == TypeKind.Error &&
                manifestMessages.TryGetValue(parameter.Type.Name, out string manifestNativeName))
            {
                nativeTypeName = manifestNativeName;
                qualifiedParameterType =
                    "global::Plasma.@" + ManagedTypeName(manifestNativeName);
            }

            if (signatureValid && customMessage is not null &&
                parameter!.Type is INamedTypeSymbol customMessageType &&
                !customMessageType.IsGenericType)
            {
                string customMetadataName = GetRuntimeMetadataName(customMessageType);
                messageId = StableHash(
                    $"pl-csharp/custom-message/v1:{customMessageType.ContainingAssembly.Identity.Name}|{customMetadataName}");
                nativeTypeName = "plMsgDeliverCSharpMsg";
                messageIdentity = $"custom:{messageId:X16}";
                messageDisplayName = customMessageType.ToDisplayString(
                    SymbolDisplayFormat.CSharpErrorMessageFormat);
            }
            else if (signatureValid && !string.IsNullOrWhiteSpace(nativeTypeName))
            {
                messageId = StableHash(
                    $"pl-csharp/message/v1:{managedName}|{method.Name}|{nativeTypeName}");
                messageIdentity = "native:" + nativeTypeName;
                messageDisplayName = nativeTypeName;
            }

            if (!signatureValid || string.IsNullOrWhiteSpace(nativeTypeName) ||
                string.IsNullOrWhiteSpace(messageIdentity))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidMessageHandler,
                    method.Locations.FirstOrDefault() ?? location,
                    script.Name,
                    method.Name));
                valid = false;
                continue;
            }

            if (!handledMessages.Add(messageIdentity!))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    DuplicateMessageHandler,
                    method.Locations.FirstOrDefault() ?? location,
                    script.Name,
                    messageDisplayName));
                valid = false;
                continue;
            }

            IParameterSymbol messageParameter = parameter!;
            messages.Add(new MessageModel(
                messageId,
                method.Name,
                nativeTypeName!,
                qualifiedParameterType!,
                messageParameter.RefKind == RefKind.In));
        }

        return valid
            ? new ScriptModel(
                managedName,
                qualifiedName,
                script.ContainingNamespace.IsGlobalNamespace
                    ? string.Empty
                    : script.ContainingNamespace.ToDisplayString(),
                script.Name,
                persistentId,
                MakeSourceFile(location, sourceRoot),
                location,
                lifecycle,
                fields,
                messages)
            : null;
    }

    private static string GetRuntimeMetadataName(INamedTypeSymbol type)
    {
        var names = new Stack<string>();
        for (INamedTypeSymbol? current = type; current is not null; current = current.ContainingType)
        {
            names.Push(current.MetadataName);
        }

        string nestedName = string.Join("+", names);
        return type.ContainingNamespace.IsGlobalNamespace
            ? nestedName
            : type.ContainingNamespace.ToDisplayString() + "." + nestedName;
    }

    private static bool DerivesFrom(INamedTypeSymbol symbol, string metadataName)
    {
        for (INamedTypeSymbol? current = symbol.BaseType; current is not null; current = current.BaseType)
        {
            if (current.ToDisplayString() == metadataName)
            {
                return true;
            }
        }

        return false;
    }

    private static string MakeSourceFile(Location location, string sourceRoot)
    {
        string sourceFile = location.SourceTree?.FilePath ?? string.Empty;
        if (sourceFile.Length == 0)
        {
            return string.Empty;
        }

        if (!string.IsNullOrWhiteSpace(sourceRoot))
        {
            string cleanRoot = sourceRoot.TrimEnd(
                Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar);
            if (sourceFile.StartsWith(cleanRoot, StringComparison.OrdinalIgnoreCase) &&
                sourceFile.Length > cleanRoot.Length &&
                (sourceFile[cleanRoot.Length] == Path.DirectorySeparatorChar ||
                 sourceFile[cleanRoot.Length] == Path.AltDirectorySeparatorChar))
            {
                sourceFile = sourceFile.Substring(cleanRoot.Length + 1);
            }
        }
        else
        {
            sourceFile = Path.GetFileName(sourceFile);
        }

        return sourceFile.Replace('\\', '/');
    }

    private static LifecycleModel? CreateLifecycleModel(
        INamedTypeSymbol script,
        string methodName)
    {
        for (INamedTypeSymbol? current = script;
             current is not null && current.ToDisplayString() != ComponentScriptName;
             current = current.BaseType)
        {
            foreach (IMethodSymbol method in current.GetMembers(methodName).OfType<IMethodSymbol>())
            {
                for (IMethodSymbol? overridden = method.IsOverride ? method.OverriddenMethod : null;
                     overridden is not null;
                     overridden = overridden.OverriddenMethod)
                {
                    if (overridden.ContainingType.ToDisplayString() == ComponentScriptName)
                    {
                        Location? location = method.Locations.FirstOrDefault(static candidate => candidate.IsInSource);
                        if (location is null)
                        {
                            return new LifecycleModel(methodName, string.Empty, 1, 1);
                        }

                        FileLinePositionSpan lineSpan = location.GetLineSpan();
                        return new LifecycleModel(
                            methodName,
                            lineSpan.Path,
                            lineSpan.StartLinePosition.Line + 1,
                            lineSpan.StartLinePosition.Character + 1);
                    }
                }
            }
        }

        return null;
    }

    private static bool IsSupportedExposedType(ITypeSymbol type)
    {
        if (type.TypeKind == TypeKind.Enum)
        {
            return true;
        }

        if (type.SpecialType is
            SpecialType.System_Boolean or
            SpecialType.System_Byte or
            SpecialType.System_SByte or
            SpecialType.System_Int16 or
            SpecialType.System_UInt16 or
            SpecialType.System_Int32 or
            SpecialType.System_UInt32 or
            SpecialType.System_Int64 or
            SpecialType.System_UInt64 or
            SpecialType.System_Single or
            SpecialType.System_Double or
            SpecialType.System_String)
        {
            return true;
        }

        string name = type.ToDisplayString();
        return name is
            "Plasma.Time" or
            "Plasma.Angle" or
            "Plasma.Vec2" or
            "Plasma.Vec3" or
            "Plasma.Vec4" or
            "Plasma.Quat" or
            "Plasma.Color" or
            "Plasma.Transform" or
            "Plasma.NativeObject" or
            "Plasma.World" or
            "Plasma.GameObject" or
            "Plasma.Component";
    }

    private static bool IsAccessibleFromDescriptorProvider(ISymbol symbol) =>
        symbol.DeclaredAccessibility is
            Accessibility.Public or
            Accessibility.Internal or
            Accessibility.ProtectedOrInternal;

    private static IReadOnlyDictionary<string, string> GetEditorMetadata(ISymbol member)
    {
        var values = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (AttributeData attribute in member.GetAttributes())
        {
            string? attributeName = attribute.AttributeClass?.ToDisplayString();
            if (attributeName == "Plasma.RangeAttribute" && attribute.ConstructorArguments.Length == 2)
            {
                values["Range.Minimum"] = Convert.ToString(
                    attribute.ConstructorArguments[0].Value,
                    CultureInfo.InvariantCulture) ?? string.Empty;
                values["Range.Maximum"] = Convert.ToString(
                    attribute.ConstructorArguments[1].Value,
                    CultureInfo.InvariantCulture) ?? string.Empty;
            }
            else if (attributeName == "Plasma.CategoryAttribute" &&
                     attribute.ConstructorArguments.FirstOrDefault().Value is string category)
            {
                values["Category"] = category;
            }
            else if (attributeName == "Plasma.DisplayNameAttribute" &&
                     attribute.ConstructorArguments.FirstOrDefault().Value is string displayName)
            {
                values["DisplayName"] = displayName;
            }
        }

        return values;
    }

    private static AttributeData? FindAttribute(ISymbol symbol, string metadataName) =>
        symbol.GetAttributes().FirstOrDefault(attribute =>
            attribute.AttributeClass?.ToDisplayString() == metadataName);

    private static string Render(IReadOnlyList<ScriptModel> scripts)
    {
        var source = new StringBuilder("// <auto-generated />\n#nullable enable\n");

        foreach (ScriptModel script in scripts)
        {
            RenderMessageBridges(source, script);
        }

        source.Append(
            """
            namespace Plasma.Generated
            {
                [global::System.CodeDom.Compiler.GeneratedCode("Plasma.Engine.Generator", "1.0")]
                public sealed class PlasmaGeneratedScriptDescriptorProvider : global::Plasma.IScriptDescriptorProvider
                {
                    public global::System.Collections.Generic.IReadOnlyList<global::Plasma.ScriptTypeDescriptor> GetDescriptors()
                    {
                        return new global::Plasma.ScriptTypeDescriptor[]
                        {
            """);

        foreach (ScriptModel script in scripts)
        {
            RenderScript(source, script);
        }

        source.Append(
            """
                        };
                    }
                }
            }
            """);
        return source.ToString();
    }

    private static void RenderMessageBridges(StringBuilder source, ScriptModel script)
    {
        if (script.Messages.Count == 0)
        {
            return;
        }

        if (script.Namespace.Length > 0)
        {
            source.Append("namespace ").Append(script.Namespace).AppendLine();
            source.AppendLine("{");
        }

        source.Append("    partial class ").Append(EscapeIdentifier(script.SimpleName)).AppendLine();
        source.AppendLine("    {");
        foreach (MessageModel message in script.Messages)
        {
            ulong id = message.Id;
            source.Append("        internal void __PlasmaDispatchMessage_")
                .Append(id.ToString("X16", CultureInfo.InvariantCulture))
                .AppendLine("(global::System.ReadOnlySpan<object?> values)");
            source.AppendLine("        {");
            source.Append("            var message = (")
                .Append(message.QualifiedParameterType)
                .AppendLine(")values[0]!;");
            source.Append("            ")
                .Append(EscapeIdentifier(message.MethodName))
                .Append("(")
                .Append(message.IsIn ? "in " : string.Empty)
                .AppendLine("message);");
            source.AppendLine("        }");
        }
        source.AppendLine("    }");

        if (script.Namespace.Length > 0)
        {
            source.AppendLine("}");
        }
    }

    private static void RenderScript(StringBuilder source, ScriptModel script)
    {
        string typeId = StableHash($"pl-csharp/type/v1:{script.ManagedName}")
            .ToString("X16", CultureInfo.InvariantCulture);

        source.AppendLine("                new global::Plasma.ScriptTypeDescriptor");
        source.AppendLine("                {");
        source.Append("                    Id = 0x").Append(typeId).AppendLine("UL,");
        source.Append("                    PersistentId = new global::System.Guid(\"")
            .Append(script.PersistentId).AppendLine("\"),");
        source.Append("                    ManagedName = \"").Append(Escape(script.ManagedName)).AppendLine("\",");
        source.Append("                    SourceFile = \"").Append(Escape(script.SourceFile)).AppendLine("\",");
        source.Append("                    ManagedType = typeof(").Append(script.QualifiedName).AppendLine("),");
        source.Append("                    Create = static () => new ").Append(script.QualifiedName).AppendLine("(),");
        source.AppendLine("                    Lifecycle = global::Plasma.ScriptTypeDescriptor.Freeze(");
        source.AppendLine("                        new global::System.Collections.Generic.Dictionary<global::Plasma.ScriptLifecycleMethod, global::Plasma.ScriptLifecycleInvoker>");
        source.AppendLine("                        {");
        foreach (LifecycleModel lifecycle in script.Lifecycle)
        {
            string invocation = lifecycle.Name == "Update"
                ? "instance.Update(deltaTime)"
                : $"instance.{lifecycle.Name}()";
            RenderLifecycle(source, script, lifecycle, invocation);
        }
        source.AppendLine("                        }),");

        source.AppendLine("                    Fields = global::Plasma.ScriptTypeDescriptor.Freeze(");
        source.AppendLine("                        new global::System.Collections.Generic.Dictionary<ulong, global::Plasma.ScriptFieldDescriptor>");
        source.AppendLine("                        {");
        foreach (FieldModel field in script.Fields)
        {
            RenderField(source, script, field);
        }
        source.AppendLine("                        }),");

        source.AppendLine("                    Messages = global::Plasma.ScriptTypeDescriptor.Freeze(");
        source.AppendLine("                        new global::System.Collections.Generic.Dictionary<ulong, global::Plasma.ScriptMessageDescriptor>");
        source.AppendLine("                        {");
        foreach (MessageModel message in script.Messages)
        {
            RenderMessage(source, script, message);
        }
        source.AppendLine("                        }),");
        source.AppendLine("                },");
    }

    private static void RenderLifecycle(
        StringBuilder source,
        ScriptModel script,
        LifecycleModel lifecycle,
        string invocation)
    {
        source.Append("                            [global::Plasma.ScriptLifecycleMethod.")
            .Append(lifecycle.Name)
            .AppendLine("] = static (baseInstance, deltaTime) =>")
            .AppendLine("                            {")
            .Append("                                var instance = (")
            .Append(script.QualifiedName)
            .AppendLine(")baseInstance;");
        if (lifecycle.SourcePath.Length == 0)
        {
            source.Append("                                ")
                .Append(invocation)
                .AppendLine(";");
        }
        else
        {
            source.AppendLine("                                try")
                .AppendLine("                                {")
                .Append("                                    ")
                .Append(invocation)
                .AppendLine(";")
                .AppendLine("                                }")
                .AppendLine("                                catch (global::System.Exception exception)")
                .AppendLine("                                {")
                .Append("                                    exception.Data[\"Plasma.ScriptSourceFile\"] = \"")
                .Append(Escape(lifecycle.SourcePath))
                .AppendLine("\";")
                .Append("                                    exception.Data[\"Plasma.ScriptSourceLine\"] = ")
                .Append(lifecycle.Line)
                .AppendLine(";")
                .Append("                                    exception.Data[\"Plasma.ScriptSourceColumn\"] = ")
                .Append(lifecycle.Column)
                .AppendLine(";")
                .AppendLine("                                    throw;")
                .AppendLine("                                }");
        }
        source.AppendLine("                            },");
    }

    private static void RenderField(StringBuilder source, ScriptModel script, FieldModel field)
    {
        ulong id = StableHash($"pl-csharp/field/v1:{script.ManagedName}|{field.Name}");
        string identifier = EscapeIdentifier(field.Name);

        source.Append("                            [0x")
            .Append(id.ToString("X16", CultureInfo.InvariantCulture))
            .AppendLine("UL] = new global::Plasma.ScriptFieldDescriptor(");
        source.Append("                                0x")
            .Append(id.ToString("X16", CultureInfo.InvariantCulture))
            .Append("UL, \"")
            .Append(Escape(field.Name))
            .Append("\", typeof(")
            .Append(field.QualifiedType)
            .AppendLine("),");
        source.Append("                                new ")
            .Append(script.QualifiedName)
            .Append("().")
            .Append(identifier)
            .AppendLine(",");
        source.Append("                                static baseInstance => ((")
            .Append(script.QualifiedName)
            .Append(")baseInstance).")
            .Append(identifier)
            .AppendLine(",");
        source.Append("                                static (baseInstance, value) => ((")
            .Append(script.QualifiedName)
            .Append(")baseInstance).")
            .Append(identifier)
            .Append(" = (")
            .Append(field.QualifiedType)
            .AppendLine(")value!,");
        source.AppendLine("                                new global::System.Collections.Generic.Dictionary<string, string>");
        source.AppendLine("                                {");
        foreach (KeyValuePair<string, string> pair in field.Metadata)
        {
            source.Append("                                    [\"")
                .Append(Escape(pair.Key))
                .Append("\"] = \"")
                .Append(Escape(pair.Value))
                .AppendLine("\",");
        }
        source.AppendLine("                                }),");
    }

    private static void RenderMessage(StringBuilder source, ScriptModel script, MessageModel message)
    {
        ulong id = message.Id;
        source.Append("                            [0x")
            .Append(id.ToString("X16", CultureInfo.InvariantCulture))
            .AppendLine("UL] = new global::Plasma.ScriptMessageDescriptor(");
        source.Append("                                0x")
            .Append(id.ToString("X16", CultureInfo.InvariantCulture))
            .Append("UL, \"")
            .Append(Escape(message.NativeTypeName))
            .Append("\", typeof(")
            .Append(message.QualifiedParameterType)
            .AppendLine("),");
        source.Append("                                static (baseInstance, values) => ((")
            .Append(script.QualifiedName)
            .Append(")baseInstance).__PlasmaDispatchMessage_")
            .Append(id.ToString("X16", CultureInfo.InvariantCulture))
            .AppendLine("(values)),");
    }

    private static ulong StableHash(string value)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;

        ulong hash = offset;
        foreach (byte item in Encoding.UTF8.GetBytes(value))
        {
            hash ^= item;
            hash *= prime;
        }

        return hash;
    }

    private static Guid CreateAutomaticPersistentId(
        string assemblyName,
        string fullyQualifiedMetadataTypeName)
    {
        string canonicalIdentity =
            $"pl-csharp/script-persistent-id/v1\0{assemblyName}\0{fullyQualifiedMetadataTypeName}";
        byte[] namespaceBytes = ScriptPersistentIdNamespace.ToByteArray();
        SwapGuidByteOrder(namespaceBytes);

        byte[] nameBytes = Encoding.UTF8.GetBytes(canonicalIdentity);
        byte[] input = new byte[namespaceBytes.Length + nameBytes.Length];
        Buffer.BlockCopy(namespaceBytes, 0, input, 0, namespaceBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, input, namespaceBytes.Length, nameBytes.Length);

        byte[] hash;
        using (SHA1 sha1 = SHA1.Create())
        {
            hash = sha1.ComputeHash(input);
        }

        byte[] guidBytes = new byte[16];
        Buffer.BlockCopy(hash, 0, guidBytes, 0, guidBytes.Length);
        guidBytes[6] = (byte)((guidBytes[6] & 0x0F) | 0x50);
        guidBytes[8] = (byte)((guidBytes[8] & 0x3F) | 0x80);
        SwapGuidByteOrder(guidBytes);
        return new Guid(guidBytes);
    }

    private static void SwapGuidByteOrder(byte[] bytes)
    {
        byte value = bytes[0];
        bytes[0] = bytes[3];
        bytes[3] = value;
        value = bytes[1];
        bytes[1] = bytes[2];
        bytes[2] = value;
        value = bytes[4];
        bytes[4] = bytes[5];
        bytes[5] = value;
        value = bytes[6];
        bytes[6] = bytes[7];
        bytes[7] = value;
    }

    private static string Escape(string value) =>
        value.Replace("\\", "\\\\").Replace("\"", "\\\"");

    private static string EscapeIdentifier(string value) => "@" + value;

    private static string ManagedTypeName(string nativeName)
    {
        string value = nativeName;
        int separator = value.LastIndexOf("::", StringComparison.Ordinal);
        if (separator >= 0)
        {
            value = value.Substring(separator + 2);
        }
        if (value.StartsWith("pl", StringComparison.Ordinal) && value.Length > 2)
        {
            value = value.Substring(2);
        }

        var result = new StringBuilder(value.Length + 1);
        if (value.Length == 0 || !char.IsLetter(value[0]) && value[0] != '_')
        {
            result.Append('_');
        }
        foreach (char character in value)
        {
            result.Append(char.IsLetterOrDigit(character) || character == '_'
                ? character
                : '_');
        }
        return result.ToString();
    }

    private sealed class ManifestMessageCatalog
    {
        private ManifestMessageCatalog(IReadOnlyDictionary<string, string> messages)
        {
            Messages = messages;
        }

        public IReadOnlyDictionary<string, string> Messages { get; }

        public static ManifestMessageCatalog Parse(string? json)
        {
            var messages = new SortedDictionary<string, string>(StringComparer.Ordinal);
            if (string.IsNullOrWhiteSpace(json))
            {
                return new ManifestMessageCatalog(messages);
            }

            try
            {
                using JsonDocument document = JsonDocument.Parse(json!);
                if (!document.RootElement.TryGetProperty("messages", out JsonElement entries))
                {
                    return new ManifestMessageCatalog(messages);
                }

                foreach (JsonElement entry in entries.EnumerateArray())
                {
                    if (entry.TryGetProperty("typeName", out JsonElement typeNameElement) &&
                        typeNameElement.ValueKind == JsonValueKind.String &&
                        typeNameElement.GetString() is string nativeName &&
                        nativeName.Length > 0)
                    {
                        messages[ManagedTypeName(nativeName)] = nativeName;
                    }
                }
            }
            catch (JsonException)
            {
                // BindingManifestGenerator owns malformed-manifest diagnostics.
            }

            return new ManifestMessageCatalog(messages);
        }
    }

    private sealed class ScriptModel
    {
        public ScriptModel(
            string managedName,
            string qualifiedName,
            string @namespace,
            string simpleName,
            string persistentId,
            string sourceFile,
            Location location,
            IReadOnlyList<LifecycleModel> lifecycle,
            IReadOnlyList<FieldModel> fields,
            IReadOnlyList<MessageModel> messages)
        {
            ManagedName = managedName;
            QualifiedName = qualifiedName;
            Namespace = @namespace;
            SimpleName = simpleName;
            PersistentId = persistentId;
            SourceFile = sourceFile;
            Location = location;
            Lifecycle = lifecycle;
            Fields = fields;
            Messages = messages;
        }

        public string ManagedName { get; }
        public string QualifiedName { get; }
        public string Namespace { get; }
        public string SimpleName { get; }
        public string PersistentId { get; }
        public string SourceFile { get; }
        public Location Location { get; }
        public IReadOnlyList<LifecycleModel> Lifecycle { get; }
        public IReadOnlyList<FieldModel> Fields { get; }
        public IReadOnlyList<MessageModel> Messages { get; }
    }

    private sealed class LifecycleModel
    {
        public LifecycleModel(string name, string sourcePath, int line, int column)
        {
            Name = name;
            SourcePath = sourcePath;
            Line = line;
            Column = column;
        }

        public string Name { get; }
        public string SourcePath { get; }
        public int Line { get; }
        public int Column { get; }
    }

    private sealed class FieldModel
    {
        public FieldModel(
            string name,
            string qualifiedType,
            bool isField,
            IReadOnlyDictionary<string, string> metadata)
        {
            Name = name;
            QualifiedType = qualifiedType;
            IsField = isField;
            Metadata = metadata;
        }

        public string Name { get; }
        public string QualifiedType { get; }
        public bool IsField { get; }
        public IReadOnlyDictionary<string, string> Metadata { get; }
    }

    private sealed class MessageModel
    {
        public MessageModel(
            ulong id,
            string methodName,
            string nativeTypeName,
            string qualifiedParameterType,
            bool isIn)
        {
            Id = id;
            MethodName = methodName;
            NativeTypeName = nativeTypeName;
            QualifiedParameterType = qualifiedParameterType;
            IsIn = isIn;
        }

        public ulong Id { get; }
        public string MethodName { get; }
        public string NativeTypeName { get; }
        public string QualifiedParameterType { get; }
        public bool IsIn { get; }
    }
}
