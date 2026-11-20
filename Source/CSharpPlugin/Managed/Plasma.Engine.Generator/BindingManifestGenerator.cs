using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace Plasma.Engine.Generator;

[Generator(LanguageNames.CSharp)]
public sealed class BindingManifestGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor InvalidManifest = new(
        "PLB001",
        "Invalid Plasma binding manifest",
        "Could not generate Plasma engine bindings: {0}",
        "Plasma.Bindings",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor UnsupportedBinding = new(
        "PLB002",
        "Reflected binding coverage note",
        "Reflected binding '{0}' was not generated: {1}",
        "Plasma.Bindings",
        DiagnosticSeverity.Info,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ManifestBindingError = new(
        "PLB003",
        "Invalid reflected binding",
        "Binding manifest diagnostic {0} at '{1}': {2}",
        "Plasma.Bindings",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ManifestBindingWarning = new(
        "PLB004",
        "Suspicious reflected binding",
        "Binding manifest diagnostic {0} at '{1}': {2}",
        "Plasma.Bindings",
        DiagnosticSeverity.Warning,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ManifestBindingInfo = new(
        "PLB005",
        "Reflected binding information",
        "Binding manifest diagnostic {0} at '{1}': {2}",
        "Plasma.Bindings",
        DiagnosticSeverity.Info,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<AdditionalText> manifests = context.AdditionalTextsProvider
            .Where(static file =>
                Path.GetFileName(file.Path).Equals(
                    "PlasmaBindings.json",
                    StringComparison.OrdinalIgnoreCase));

        // The engine's API descriptions, written beside the manifest by the editor; optional.
        IncrementalValueProvider<ImmutableArray<AdditionalText>> docs = context.AdditionalTextsProvider
            .Where(static file =>
                Path.GetFileName(file.Path).Equals(
                    "PlasmaScriptDocs.json",
                    StringComparison.OrdinalIgnoreCase))
            .Collect();

        context.RegisterSourceOutput(manifests.Combine(docs),
            static (context, input) => Emit(context, input.Left, input.Right.FirstOrDefault()));
    }

    private static void Emit(SourceProductionContext context, AdditionalText manifestFile, AdditionalText? docsFile)
    {
        try
        {
            SourceText? text = manifestFile.GetText(context.CancellationToken);
            if (text is null)
            {
                throw new InvalidDataException($"Could not read '{manifestFile.Path}'.");
            }

            BindingManifest manifest = BindingManifest.Parse(text.ToString());
            foreach (BindingDiagnostic diagnostic in manifest.Diagnostics)
            {
                DiagnosticDescriptor descriptor = diagnostic.Severity switch
                {
                    "error" => ManifestBindingError,
                    "warning" => ManifestBindingWarning,
                    _ => ManifestBindingInfo,
                };
                context.ReportDiagnostic(Diagnostic.Create(
                    descriptor,
                    Location.None,
                    diagnostic.Code,
                    diagnostic.Location,
                    diagnostic.Message));
            }
            if (manifest.Diagnostics.Any(static diagnostic => diagnostic.Severity == "error"))
            {
                return;
            }

            var renderer = new BindingRenderer(manifest, context, ApiDocs.Parse(docsFile?.GetText(context.CancellationToken)?.ToString()));
            context.AddSource(
                "PlasmaEngineBindings.g.cs",
                SourceText.From(renderer.Render(), Encoding.UTF8));
        }
        catch (Exception exception)
        {
            context.ReportDiagnostic(Diagnostic.Create(
                InvalidManifest,
                Location.None,
                exception.Message));
        }
    }

    private sealed class BindingRenderer
    {
        private readonly BindingManifest _manifest;
        private readonly SourceProductionContext _context;
        private readonly Dictionary<string, BindingType> _typesById;
        private readonly Dictionary<string, BindingType> _typesByName;
        private readonly Dictionary<string, BindingProperty> _propertiesById;
        private readonly ApiDocs _docs;

        public BindingRenderer(BindingManifest manifest, SourceProductionContext context, ApiDocs docs)
        {
            _manifest = manifest;
            _context = context;
            _docs = docs;
            _typesById = manifest.Types.ToDictionary(static type => type.Id, StringComparer.Ordinal);
            _typesByName = manifest.Types.ToDictionary(static type => type.NativeName, StringComparer.Ordinal);
            _propertiesById = manifest.Properties.ToDictionary(
                static property => property.Id,
                StringComparer.Ordinal);
        }

        public string Render()
        {
            var source = new StringBuilder("// <auto-generated />\n#nullable enable\nnamespace Plasma\n{\n");
            RenderEnums(source);
            RenderMessages(source);
            RenderObjectBindings(source);
            RenderExtensionBindings(source);
            source.AppendLine("}");
            return source.ToString();
        }

        private void RenderEnums(StringBuilder source)
        {
            foreach (BindingType type in _manifest.Types
                         .Where(static type => (type.Flags & (2 | 4)) != 0)
                         .OrderBy(static type => type.NativeName, StringComparer.Ordinal))
            {
                string name = TypeName(type.NativeName);
                bool bitflags = (type.Flags & 4) != 0;
                if (bitflags)
                {
                    source.AppendLine("    [global::System.Flags]");
                }
                source.Append("    public enum ")
                    .Append(Identifier(name))
                    .Append(" : ")
                    .Append(EnumStorageType(type.EnumStorageTypeName, type.TypeSize, bitflags))
                    .AppendLine();
                source.AppendLine("    {");

                BindingProperty[] constants = _manifest.Properties
                    .Where(property =>
                        property.DeclaringTypeId == type.Id &&
                        property.Category == 0 &&
                        property.HasDefaultValue)
                    .OrderBy(static property => property.NativeName, StringComparer.Ordinal)
                    .ToArray();
                foreach (BindingProperty constant in constants)
                {
                    source.Append("        ")
                        .Append(Identifier(TrimEnumConstant(type.NativeName, constant.NativeName)))
                        .Append(" = ")
                        .Append(RenderIntegralDefault(constant.DefaultValue))
                        .AppendLine(",");
                }

                source.AppendLine("    }");
            }
        }

        private void RenderMessages(StringBuilder source)
        {
            foreach (BindingMessage message in _manifest.Messages
                         .OrderBy(static message => message.TypeName, StringComparer.Ordinal))
            {
                source.Append("    [global::Plasma.GeneratedMessage(\"")
                    .Append(Escape(message.TypeName))
                    .AppendLine("\")]");
                source.Append("    public readonly partial record struct ")
                    .Append(Identifier(TypeName(message.TypeName)))
                    .AppendLine();
                source.AppendLine("    {");
                foreach (string propertyId in message.Properties)
                {
                    if (!_propertiesById.TryGetValue(propertyId, out BindingProperty? property))
                    {
                        continue;
                    }

                    string? propertyType = MapType(property.ValueTypeName, property.Flags);
                    if (propertyType is null)
                    {
                        ReportUnsupported(
                            $"{message.TypeName}.{property.NativeName}",
                            $"property type '{property.ValueTypeName}' is unsupported");
                        continue;
                    }

                    source.Append("        public ")
                        .Append(propertyType)
                        .Append(" ")
                        .Append(Identifier(PropertyName(property.NativeName)))
                        .AppendLine(" { get; init; }");
                }
                source.AppendLine("    }");
            }
        }

        private void RenderObjectBindings(StringBuilder source)
        {
            var typeIds = new HashSet<string>(StringComparer.Ordinal);
            foreach (BindingType type in _manifest.Types.Where(type =>
                         (type.Flags & 8) != 0 &&
                         !type.Hidden &&
                         !type.ExcludedFromScript &&
                         IsNativeObjectHandleType(type)))
            {
                typeIds.Add(type.Id);
            }
            foreach (BindingFunction function in _manifest.Functions.Where(static function =>
                         function.FunctionType == 0))
            {
                typeIds.Add(function.DeclaringTypeId);
            }
            foreach (BindingProperty property in _manifest.Properties.Where(static property =>
                         property.Category == 1))
            {
                typeIds.Add(property.DeclaringTypeId);
            }

            foreach (string typeId in typeIds.OrderBy(static id => id, StringComparer.Ordinal))
            {
                if (!_typesById.TryGetValue(typeId, out BindingType? type) ||
                    type.Hidden ||
                    type.ExcludedFromScript ||
                    type.ScriptExtensionName.Length > 0 ||
                    _manifest.Messages.Any(message => message.TypeName == type.NativeName))
                {
                    continue;
                }

                if (!IsNativeObjectHandleType(type))
                {
                    // Value/configuration types and unmanaged native objects are expected to
                    // appear in the shared reflection manifest. Their members are emitted only
                    // after the type gains a managed value representation or a safe lifetime
                    // handle, so this is coverage metadata rather than a user-script warning.
                    continue;
                }

                string typeName = TypeName(type.NativeName);
                bool wellKnown = typeName is "World" or "GameObject" or "Component";
                bool componentWrapper = !wellKnown && GetTypeHierarchy(type).Any(parent => parent.NativeName == "plComponent");
                RenderDoc(source, "    ", _docs.Type(type.NativeName));
                if (wellKnown)
                {
                    source.Append("    public static partial class ")
                        .Append(Identifier(typeName + "Bindings"))
                        .AppendLine();
                }
                else
                {
                    source.Append("    public readonly partial record struct ")
                        .Append(Identifier(typeName))
                        .Append("(global::Plasma.NativeObject Native)");
                    if (componentWrapper)
                        source.Append(" : global::Plasma.INativeComponent<").Append(Identifier(typeName)).Append('>');
                    source.AppendLine();
                }
                source.AppendLine("    {");
                if (componentWrapper)
                {
                    source.Append("        public static string NativeTypeName => \"").Append(Escape(type.NativeName)).AppendLine("\";");
                    source.Append("        public static ").Append(Identifier(typeName)).AppendLine(" FromNative(global::Plasma.NativeObject native) => new(native);");
                }

                foreach (BindingProperty property in GetInheritedProperties(type))
                {
                    RenderProperty(source, property, wellKnown ? typeName : null);
                }
                foreach (BindingFunction function in GetInheritedFunctions(type))
                {
                    RenderFunction(
                        source,
                        function,
                        instanceMethod: true,
                        extensionReceiverType: wellKnown ? typeName : null);
                }

                if (typeName == "GameObject" &&
                    _manifest.Functions.Any(function =>
                        function.DeclaringTypeId == type.Id &&
                        function.PublicName == "FindComponentByTypeName") &&
                    _manifest.Functions.Any(function =>
                        function.DeclaringTypeId == type.Id &&
                        function.PublicName == "CreateComponentByTypeName"))
                {
                    source.AppendLine(
                        """
                                public static bool TryGetComponent<T>(this global::Plasma.GameObject self, out T component)
                                    where T : struct, global::Plasma.INativeComponent<T>
                                {
                                    global::Plasma.Component nativeComponent =
                                        self.FindComponentByTypeName(T.NativeTypeName);
                                    if (!nativeComponent.IsValid)
                                    {
                                        component = default;
                                        return false;
                                    }

                                    component = T.FromNative(nativeComponent.Native);
                                    return true;
                                }

                                public static T CreateComponent<T>(this global::Plasma.GameObject self)
                                    where T : struct, global::Plasma.INativeComponent<T>
                                {
                                    global::Plasma.Component nativeComponent =
                                        self.CreateComponentByTypeName(T.NativeTypeName);
                                    if (!nativeComponent.IsValid)
                                    {
                                        throw new global::System.InvalidOperationException(
                                            $"Native component type '{T.NativeTypeName}' could not be created.");
                                    }

                                    return T.FromNative(nativeComponent.Native);
                                }
                        """);
                }

                source.AppendLine("    }");
            }
        }

        private void RenderExtensionBindings(StringBuilder source)
        {
            foreach (IGrouping<string, BindingFunction> group in _manifest.Functions
                         .Where(function =>
                             function.FunctionType == 1 &&
                             _typesById.TryGetValue(function.DeclaringTypeId, out BindingType? type) &&
                             !type.Hidden &&
                             !type.ExcludedFromScript)
                         .GroupBy(function =>
                         {
                             BindingType type = _typesById[function.DeclaringTypeId];
                             return type.ScriptExtensionName.Length > 0
                                 ? TypeName(type.ScriptExtensionName)
                                 : TypeName(type.NativeName);
                         }, StringComparer.Ordinal)
                         .OrderBy(static group => group.Key, StringComparer.Ordinal))
            {
                BindingType declaringType = _typesById[group.First().DeclaringTypeId];
                string groupName = IsManagedCoreType(group.Key) ||
                                   declaringType.ScriptExtensionName.Length == 0 &&
                                   IsNativeObjectHandleType(declaringType)
                    ? group.Key + "Bindings"
                    : group.Key;
                source.Append("    public static partial class ")
                    .Append(Identifier(groupName))
                    .AppendLine();
                source.AppendLine("    {");
                foreach (BindingFunction function in group.OrderBy(
                             static function => function.CanonicalIdentity,
                             StringComparer.Ordinal))
                {
                    RenderFunction(
                        source,
                        function,
                        instanceMethod: false,
                        extensionReceiverType: null);
                }
                source.AppendLine("    }");
            }
        }

        private static bool IsManagedCoreType(string typeName) =>
            typeName is
                "TempHashedString" or
                "Time" or
                "Angle" or
                "Vec2" or
                "Vec2U32" or
                "Vec3" or
                "Vec4" or
                "Quat" or
                "Color" or
                "Transform" or
                "World" or
                "GameObject" or
                "Component" or
                "Log";

        /// <summary>A one-paragraph XML doc comment, or nothing for an empty description.</summary>
        private static void RenderDoc(StringBuilder source, string indent, string description)
        {
            if (description.Length == 0)
                return;

            string text = description
                .Replace("&", "&amp;")
                .Replace("<", "&lt;")
                .Replace(">", "&gt;")
                .Replace("\r", " ")
                .Replace("\n", " ");
            source.Append(indent).Append("/// <summary>").Append(text).AppendLine("</summary>");
        }

        private void RenderProperty(
            StringBuilder source,
            BindingProperty property,
            string? extensionReceiverType)
        {
            string? type = MapType(property.ValueTypeName, property.Flags);
            if (type is null)
            {
                ReportUnsupported(
                    $"{property.DeclaringTypeName}.{property.NativeName}",
                    $"property type '{property.ValueTypeName}' is unsupported");
                return;
            }

            string propertyName = PropertyName(property.NativeName);
            string bindingName = $"{TypeName(property.DeclaringTypeName)}.{propertyName}";
            string target = extensionReceiverType is null ? "Native" : "self.Native";
            ValueCodec codec = Classify(type, property.ValueTypeName);
            bool writable = (property.Flags & 256) == 0;
            RenderDoc(source, "        ", _docs.Member(property.DeclaringTypeName, property.NativeName));

            if (extensionReceiverType is not null)
            {
                source.Append("        public static ").Append(type)
                    .Append(" ").Append(Identifier("Get" + propertyName))
                    .Append("(this global::Plasma.").Append(Identifier(extensionReceiverType))
                    .AppendLine(" self)");
                RenderPropertyGetter(source, "        ", property.Id, bindingName, target, codec);

                if (writable)
                {
                    source.Append("        public static void ")
                        .Append(Identifier("Set" + propertyName))
                        .Append("(this global::Plasma.").Append(Identifier(extensionReceiverType))
                        .Append(" self, ").Append(type).AppendLine(" value)");
                    RenderPropertySetter(source, "        ", property.Id, bindingName, target, codec);
                }
                return;
            }

            source.Append("        public ").Append(type).Append(" ")
                .Append(Identifier(propertyName)).AppendLine();
            source.AppendLine("        {");
            source.AppendLine("            get");
            RenderPropertyGetter(source, "            ", property.Id, bindingName, target, codec);
            if (writable)
            {
                source.AppendLine("            set");
                RenderPropertySetter(source, "            ", property.Id, bindingName, "Native", codec);
            }
            source.AppendLine("        }");
        }

        private static void RenderPropertyGetter(
            StringBuilder source, string indent, string id, string bindingName, string target, ValueCodec codec)
        {
            source.Append(indent).AppendLine("{");
            source.Append(indent).AppendLine("    global::System.Span<global::Plasma.NativeValue> __plArgs = default;");
            source.Append(indent).Append("    using global::Plasma.EngineCall __plCall = new(0x").Append(id).Append("UL, \"")
                .Append(Escape(bindingName + " getter")).Append("\", ").Append(target).AppendLine(", __plArgs);");
            source.Append(indent).AppendLine("    __plCall.Invoke();");
            source.Append(indent).Append("    return ").Append(ResultExpression(codec, "-1")).AppendLine(";");
            source.Append(indent).AppendLine("}");
        }

        private static void RenderPropertySetter(
            StringBuilder source, string indent, string id, string bindingName, string target, ValueCodec codec)
        {
            source.Append(indent).AppendLine("{");
            source.Append(indent).AppendLine("    global::System.Span<global::Plasma.NativeValue> __plArgs = stackalloc global::Plasma.NativeValue[1];");
            source.Append(indent).Append("    using global::Plasma.EngineCall __plCall = new(0x").Append(id).Append("UL, \"")
                .Append(Escape(bindingName + " setter")).Append("\", ").Append(target).AppendLine(", __plArgs);");
            RenderArgument(source, indent + "    ", codec, 0, "value", "in");
            source.Append(indent).AppendLine("    __plCall.Invoke();");
            source.Append(indent).AppendLine("}");
        }

        /// <summary>How one value crosses the native boundary: which EngineCall method writes it and reads it back.</summary>
        private sealed class ValueCodec
        {
            public ValueCodec(string kind, string type, string detail)
            {
                Kind = kind;
                Type = type;
                Detail = detail;
            }

            public string Kind { get; }
            public string Type { get; }
            public string Detail { get; }
        }

        private ValueCodec Classify(string type, string nativeName)
        {
            switch (type)
            {
                case "bool":
                    return new ValueCodec("Bool", type, "");
                case "sbyte":
                case "short":
                case "int":
                case "long":
                    return new ValueCodec("Signed", type, "");
                case "byte":
                case "ushort":
                case "uint":
                case "ulong":
                    return new ValueCodec("Unsigned", type, "");
                case "float":
                case "double":
                    return new ValueCodec("Floating", type, "");
                case "string":
                    return new ValueCodec("String", type, "");
                case "object":
                    return new ValueCodec("Variant", type, "");
                case "global::Plasma.TempHashedString":
                    return new ValueCodec("Hashed", type, "");
                case "global::Plasma.Time":
                case "global::Plasma.Angle":
                case "global::Plasma.Vec2":
                case "global::Plasma.Vec2U32":
                case "global::Plasma.Vec3":
                case "global::Plasma.Vec4":
                case "global::Plasma.Quat":
                case "global::Plasma.Color":
                case "global::Plasma.Transform":
                    return new ValueCodec("Bytes", type, type.Substring("global::Plasma.".Length));
                case "global::Plasma.World":
                    return new ValueCodec("Object", type, "World");
                case "global::Plasma.GameObject":
                    return new ValueCodec("Object", type, "GameObject");
                case "global::Plasma.Component":
                    return new ValueCodec("Object", type, "Component");
            }

            string normalized = nativeName.Trim();
            if (_typesByName.TryGetValue(normalized, out BindingType? reflected) && (reflected.Flags & (2 | 4)) != 0)
                return new ValueCodec("Enum", type, "");

            return new ValueCodec("Object", type, ObjectKindOf(normalized));
        }

        /// <summary>The NativeObjectKind of a reflected class, from its nearest engine object base.</summary>
        private string ObjectKindOf(string nativeName)
        {
            var visited = new HashSet<string>(StringComparer.Ordinal);
            for (BindingType? current = _typesByName.TryGetValue(nativeName, out BindingType? start) ? start : null;
                 current is not null && visited.Add(current.NativeName);
                 current = current.ParentNativeName.Length > 0 && _typesByName.TryGetValue(current.ParentNativeName, out BindingType? parent) ? parent : null)
            {
                switch (current.NativeName)
                {
                    case "plWorld":
                        return "World";
                    case "plGameObject":
                        return "GameObject";
                    case "plComponent":
                        return "Component";
                }
            }

            return "ReflectedObject";
        }

        private static void RenderArgument(
            StringBuilder source, string indent, ValueCodec codec, int index, string expression, string direction)
        {
            if (direction == "out")
            {
                source.Append(indent).Append("__plCall.Out(").Append(index).AppendLine(");");
                return;
            }

            source.Append(indent);
            switch (codec.Kind)
            {
                case "Bool":
                    source.Append("__plCall.Bool(").Append(index).Append(", ").Append(expression).AppendLine(");");
                    break;
                case "Signed":
                    source.Append("__plCall.Int64(").Append(index).Append(", ").Append(expression).AppendLine(");");
                    break;
                case "Unsigned":
                    source.Append("__plCall.UInt64(").Append(index).Append(", ").Append(expression).AppendLine(");");
                    break;
                case "Floating":
                    source.Append("__plCall.Double(").Append(index).Append(", ").Append(expression).AppendLine(");");
                    break;
                case "String":
                    source.Append("__plCall.String(").Append(index).Append(", ").Append(expression)
                        .AppendLine(", stackalloc byte[global::Plasma.EngineCall.StringScratchBytes]);");
                    break;
                case "Hashed":
                    source.Append("__plCall.UInt64(").Append(index).Append(", ").Append(expression).AppendLine(".Hash);");
                    break;
                case "Bytes":
                    // A local copy: an inout argument may refer to a field the GC can move during the call.
                    source.Append(codec.Type).Append(" __plValue").Append(index).Append(" = ").Append(expression).AppendLine(";");
                    source.Append(indent).Append("__plCall.Bytes(").Append(index).Append(", in __plValue").Append(index)
                        .Append(", global::Plasma.NativeByteSpanType.").Append(codec.Detail).AppendLine(");");
                    break;
                case "Variant":
                    source.Append("__plCall.Variant(").Append(index).Append(", ").Append(expression).AppendLine(");");
                    break;
                case "Enum":
                    source.Append("__plCall.Int64(").Append(index).Append(", unchecked((long)").Append(expression).AppendLine("));");
                    break;
                default:
                    source.Append("__plCall.Object(").Append(index).Append(", ").Append(expression).AppendLine(".Native);");
                    break;
            }
        }

        private static string ResultExpression(ValueCodec codec, string index) => codec.Kind switch
        {
            "Bool" => $"__plCall.ResultBool({index})",
            "Signed" => $"unchecked(({codec.Type})__plCall.ResultInt64({index}))",
            "Unsigned" => $"unchecked(({codec.Type})__plCall.ResultUInt64({index}))",
            "Floating" => $"({codec.Type})__plCall.ResultDouble({index})",
            "String" => $"__plCall.ResultString({index})",
            "Hashed" => $"new global::Plasma.TempHashedString(__plCall.ResultUInt64({index}))",
            "Bytes" => $"__plCall.ResultBytes<{codec.Type}>({index})",
            "Variant" => $"__plCall.ResultVariant({index})!",
            "Enum" => $"unchecked(({codec.Type})__plCall.ResultInt64({index}))",
            _ => $"new {codec.Type}(__plCall.ResultObject(global::Plasma.NativeObjectKind.{codec.Detail}, {index}))",
        };

        private void RenderFunction(
            StringBuilder source,
            BindingFunction function,
            bool instanceMethod,
            string? extensionReceiverType)
        {
            var visibleParameters = new List<RenderedParameter>();
            int nativeIndex = 0;
            foreach (BindingParameter parameter in function.Parameters)
            {
                string? type = parameter.TypeName == "plTempHashedString"
                    ? "string"
                    : MapType(parameter.TypeName, parameter.Flags);
                if (type is null)
                {
                    ReportUnsupported(
                        $"{function.DeclaringTypeName}.{function.NativeName}",
                        $"parameter '{parameter.Name}' has unsupported type '{parameter.TypeName}'");
                    return;
                }

                // The native bridge injects the current scoped world for this special parameter.
                if (parameter.TypeName == "plWorld" &&
                    (parameter.Flags & (32 | 64)) != 0)
                {
                    ++nativeIndex;
                    continue;
                }

                visibleParameters.Add(new RenderedParameter(parameter, type, nativeIndex++));
            }

            bool canRenderDefault = true;
            for (int index = visibleParameters.Count - 1; index >= 0; --index)
            {
                RenderedParameter parameter = visibleParameters[index];
                string? defaultLiteral = RenderDefault(parameter.Parameter, parameter.Type);
                if (!canRenderDefault ||
                    parameter.Parameter.Direction != "in" ||
                    defaultLiteral is null)
                {
                    parameter.DefaultLiteral = null;
                    canRenderDefault = false;
                }
                else
                {
                    parameter.DefaultLiteral = defaultLiteral;
                }
            }

            string? returnType = IsVoid(function)
                ? "void"
                : MapType(function.ReturnTypeName, function.ReturnFlags);
            if (returnType is null)
            {
                ReportUnsupported(
                    $"{function.DeclaringTypeName}.{function.NativeName}",
                    $"return type '{function.ReturnTypeName}' is unsupported");
                return;
            }

            RenderDoc(source, "        ", _docs.Member(function.DeclaringTypeName, function.NativeName));
            source.Append("        public ");
            if (!instanceMethod || extensionReceiverType is not null)
            {
                source.Append("static ");
            }
            source.Append(returnType).Append(" ")
                .Append(Identifier(function.PublicName))
                .Append("(");
            bool hasReceiver = extensionReceiverType is not null;
            if (hasReceiver)
            {
                source.Append("this global::Plasma.")
                    .Append(Identifier(extensionReceiverType!))
                    .Append(" self");
            }
            for (int index = 0; index < visibleParameters.Count; ++index)
            {
                if (index > 0 || hasReceiver)
                {
                    source.Append(", ");
                }

                RenderedParameter parameter = visibleParameters[index];
                source.Append(DirectionKeyword(parameter.Parameter.Direction))
                    .Append(parameter.Type)
                    .Append(" ")
                    .Append(Identifier(parameter.Parameter.Name));
                if (parameter.DefaultLiteral is not null)
                {
                    source.Append(" = ").Append(parameter.DefaultLiteral);
                }
            }
            source.AppendLine(")");
            source.AppendLine("        {");

            string target = extensionReceiverType is not null
                ? "self.Native"
                : instanceMethod
                    ? "Native"
                    : "default";
            string bindingOwner = _typesById.TryGetValue(function.DeclaringTypeId, out BindingType? declaringType) &&
                                  declaringType.ScriptExtensionName.Length > 0
                ? TypeName(declaringType.ScriptExtensionName)
                : TypeName(function.DeclaringTypeName);
            string bindingName = $"{bindingOwner}.{function.PublicName}";

            // Typed call: argument values in stack memory, no boxing, no argument arrays.
            source.Append("            global::System.Span<global::Plasma.NativeValue> __plArgs = ");
            source.AppendLine(visibleParameters.Count == 0
                ? "default;"
                : $"stackalloc global::Plasma.NativeValue[{visibleParameters.Count}];");
            source.Append("            using global::Plasma.EngineCall __plCall = new(0x").Append(function.Id)
                .Append("UL, \"").Append(Escape(bindingName)).Append("\", ").Append(target).AppendLine(", __plArgs);");

            var codecs = new List<ValueCodec>(visibleParameters.Count);
            for (int index = 0; index < visibleParameters.Count; ++index)
            {
                RenderedParameter parameter = visibleParameters[index];
                ValueCodec codec = Classify(parameter.Type, parameter.Parameter.TypeName);
                codecs.Add(codec);
                RenderArgument(source, "            ", codec, index, Identifier(parameter.Parameter.Name), parameter.Parameter.Direction);
            }

            source.AppendLine("            __plCall.Invoke();");

            for (int index = 0; index < visibleParameters.Count; ++index)
            {
                RenderedParameter parameter = visibleParameters[index];
                if (parameter.Parameter.Direction is "out" or "inout")
                {
                    source.Append("            ").Append(Identifier(parameter.Parameter.Name)).Append(" = ")
                        .Append(ResultExpression(codecs[index], index.ToString(CultureInfo.InvariantCulture))).AppendLine(";");
                }
            }

            if (returnType != "void")
            {
                source.Append("            return ")
                    .Append(ResultExpression(Classify(returnType, function.ReturnTypeName), "-1")).AppendLine(";");
            }
            source.AppendLine("        }");
        }

        private string? MapType(string nativeName, int flags)
        {
            string normalized = nativeName.Trim();
            switch (normalized)
            {
                case "":
                case "void":
                    return "void";
                case "bool":
                    return "bool";
                case "plInt8":
                case "int8":
                    return "sbyte";
                case "plUInt8":
                case "uint8":
                    return "byte";
                case "plInt16":
                case "int16":
                    return "short";
                case "plUInt16":
                case "uint16":
                    return "ushort";
                case "plInt32":
                case "int":
                case "int32":
                    return "int";
                case "plUInt32":
                case "uint":
                case "uint32":
                    return "uint";
                case "plInt64":
                case "int64":
                    return "long";
                case "plUInt64":
                case "uint64":
                    return "ulong";
                case "float":
                    return "float";
                case "double":
                    return "double";
                case "plString":
                case "plStringView":
                case "plHashedString":
                case "plConstCharPtr":
                    return "string";
                case "plVariant":
                    return "object";
                case "plTempHashedString":
                    return "global::Plasma.TempHashedString";
                case "plTime":
                    return "global::Plasma.Time";
                case "plAngle":
                    return "global::Plasma.Angle";
                case "plVec2":
                    return "global::Plasma.Vec2";
                case "plVec2U32":
                    return "global::Plasma.Vec2U32";
                case "plVec3":
                    return "global::Plasma.Vec3";
                case "plVec4":
                    return "global::Plasma.Vec4";
                case "plQuat":
                    return "global::Plasma.Quat";
                case "plColor":
                case "plColorGammaUB":
                    return "global::Plasma.Color";
                case "plTransform":
                    return "global::Plasma.Transform";
                case "plWorld":
                    return "global::Plasma.World";
                case "plGameObject":
                    return "global::Plasma.GameObject";
                case "plComponent":
                    return "global::Plasma.Component";
                case "plGameObjectHandle":
                    return "global::Plasma.GameObject";
                case "plComponentHandle":
                    return "global::Plasma.Component";
            }

            if (_typesByName.TryGetValue(normalized, out BindingType? reflectedType) &&
                !reflectedType.Hidden &&
                !reflectedType.ExcludedFromScript &&
                ((reflectedType.Flags & (2 | 4)) != 0 ||
                 ((reflectedType.Flags & 8) != 0 &&
                  IsNativeObjectHandleType(reflectedType))))
            {
                return "global::Plasma." + Identifier(TypeName(normalized));
            }

            // Pointer/reference flags on an unknown class have no safe ownership representation.
            _ = flags;
            return null;
        }

        private bool IsNativeObjectHandleType(BindingType type)
        {
            var visited = new HashSet<string>(StringComparer.Ordinal);
            BindingType? current = type;
            while (current is not null && visited.Add(current.NativeName))
            {
                if (current.NativeName is "plWorld" or "plGameObject" or "plComponent")
                {
                    return true;
                }

                current = current.ParentNativeName.Length > 0 &&
                          _typesByName.TryGetValue(current.ParentNativeName, out BindingType? parent)
                    ? parent
                    : null;
            }

            return false;
        }

        private IEnumerable<BindingProperty> GetInheritedProperties(BindingType type)
        {
            var propertyNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (BindingType current in GetTypeHierarchy(type))
            {
                foreach (BindingProperty property in _manifest.Properties
                             .Where(property =>
                                 property.DeclaringTypeId == current.Id &&
                                 property.Category == 1)
                             .OrderBy(static property => property.NativeName, StringComparer.Ordinal))
                {
                    if (propertyNames.Add(PropertyName(property.NativeName)))
                    {
                        yield return property;
                    }
                }
            }
        }

        private IEnumerable<BindingFunction> GetInheritedFunctions(BindingType type)
        {
            var signatures = new HashSet<string>(StringComparer.Ordinal);
            foreach (BindingType current in GetTypeHierarchy(type))
            {
                foreach (BindingFunction function in _manifest.Functions
                             .Where(function =>
                                 function.DeclaringTypeId == current.Id &&
                                 function.FunctionType == 0)
                             .OrderBy(static function => function.CanonicalIdentity, StringComparer.Ordinal))
                {
                    string signature = function.PublicName + "|" +
                                       string.Join("|", function.Parameters.Select(
                                           static parameter => parameter.Direction + ":" + parameter.TypeName));
                    if (signatures.Add(signature))
                    {
                        yield return function;
                    }
                }
            }
        }

        private IEnumerable<BindingType> GetTypeHierarchy(BindingType type)
        {
            var visited = new HashSet<string>(StringComparer.Ordinal);
            BindingType? current = type;
            while (current is not null && visited.Add(current.NativeName))
            {
                yield return current;
                current = current.ParentNativeName.Length > 0 &&
                          _typesByName.TryGetValue(current.ParentNativeName, out BindingType? parent)
                    ? parent
                    : null;
            }
        }

        private void ReportUnsupported(string binding, string reason)
        {
            _context.ReportDiagnostic(Diagnostic.Create(
                UnsupportedBinding,
                Location.None,
                binding,
                reason));
        }

        private static bool IsVoid(BindingFunction function) =>
            function.ReturnTypeName.Length == 0 ||
            function.ReturnTypeName == "void" ||
            function.ReturnFlags == 0 && function.ReturnTypeId == "0000000000000000";

        private static string DirectionKeyword(string direction) => direction switch
        {
            "out" => "out ",
            "inout" => "ref ",
            _ => string.Empty,
        };

        private string? RenderDefault(BindingParameter parameter, string type)
        {
            if (!parameter.HasDefaultValue || parameter.DefaultValue.ValueKind == JsonValueKind.Undefined)
            {
                return null;
            }

            JsonElement value = parameter.DefaultValue;
            if (value.ValueKind == JsonValueKind.Object &&
                value.TryGetProperty("value", out JsonElement wrapped))
            {
                value = wrapped;
            }

            if (value.ValueKind == JsonValueKind.Number)
            {
                string raw = value.GetRawText();
                if (_typesByName.TryGetValue(parameter.TypeName, out BindingType? enumType) &&
                    (enumType.Flags & (2 | 4)) != 0)
                {
                    return $"({type})({raw}{IntegralSuffix(enumType)})";
                }

                if (type == "bool")
                {
                    return value.TryGetDouble(out double booleanValue) && booleanValue == 0.0
                        ? "false"
                        : "true";
                }

                if (type.StartsWith("global::Plasma.", StringComparison.Ordinal))
                {
                    return value.TryGetDouble(out double numericValue) && numericValue == 0.0
                        ? "default"
                        : null;
                }

                return type switch
                {
                    "float" => raw + "F",
                    "double" => raw + "D",
                    "uint" when raw.StartsWith("-", StringComparison.Ordinal) =>
                        $"unchecked((uint)({raw}L))",
                    "uint" => raw + "U",
                    "long" => raw + "L",
                    "ulong" when raw.StartsWith("-", StringComparison.Ordinal) =>
                        $"unchecked((ulong)({raw}L))",
                    "ulong" => raw + "UL",
                    _ => raw,
                };
            }

            return value.ValueKind switch
            {
                JsonValueKind.True => "true",
                JsonValueKind.False => "false",
                JsonValueKind.String when type == "string" =>
                    "\"" + Escape(value.GetString() ?? string.Empty) + "\"",
                JsonValueKind.Null when type == "string" => "null",
                _ => null,
            };
        }

        private static string IntegralSuffix(BindingType type)
        {
            switch (type.EnumStorageTypeName)
            {
                case "plUInt32":
                case "uint":
                case "uint32":
                    return "U";
                case "plInt64":
                case "int64":
                    return "L";
                case "plUInt64":
                case "uint64":
                    return "UL";
            }

            bool bitflags = (type.Flags & 4) != 0;
            return bitflags
                ? type.TypeSize == 8 ? "UL" : type.TypeSize == 4 ? "U" : string.Empty
                : type.TypeSize == 8 ? "L" : string.Empty;
        }

        private static string RenderIntegralDefault(JsonElement value)
        {
            if (value.ValueKind == JsonValueKind.Object &&
                value.TryGetProperty("value", out JsonElement wrapped))
            {
                value = wrapped;
            }
            return value.ValueKind == JsonValueKind.Number ? value.GetRawText() : "0";
        }

        private static string EnumStorageType(
            string nativeStorageType,
            int typeSize,
            bool bitflags)
        {
            switch (nativeStorageType)
            {
                case "plInt8":
                case "int8":
                    return "sbyte";
                case "plUInt8":
                case "uint8":
                    return "byte";
                case "plInt16":
                case "int16":
                    return "short";
                case "plUInt16":
                case "uint16":
                    return "ushort";
                case "plInt32":
                case "int":
                case "int32":
                    return "int";
                case "plUInt32":
                case "uint":
                case "uint32":
                    return "uint";
                case "plInt64":
                case "int64":
                    return "long";
                case "plUInt64":
                case "uint64":
                    return "ulong";
            }

            return (typeSize, bitflags) switch
            {
                (1, false) => "sbyte",
                (2, false) => "short",
                (4, false) => "int",
                (8, false) => "long",
                (1, true) => "byte",
                (2, true) => "ushort",
                (4, true) => "uint",
                (8, true) => "ulong",
                _ => bitflags ? "ulong" : "long",
            };
        }

        private static string TypeName(string nativeName)
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
            if (value.StartsWith("ScriptExtensionClass_", StringComparison.Ordinal))
            {
                value = value.Substring("ScriptExtensionClass_".Length);
            }
            return Sanitize(value);
        }

        private static string PropertyName(string nativeName)
        {
            string value = nativeName.StartsWith("m_", StringComparison.Ordinal)
                ? nativeName.Substring(2)
                : nativeName;
            return Sanitize(value);
        }

        private static string TrimEnumConstant(string typeName, string constantName)
        {
            string value = constantName;
            string shortType = TypeName(typeName);
            if (value.StartsWith(typeName + "::", StringComparison.Ordinal))
            {
                value = value.Substring(typeName.Length + 2);
            }
            else if (value.StartsWith(shortType + "::", StringComparison.Ordinal))
            {
                value = value.Substring(shortType.Length + 2);
            }
            else if (value.StartsWith(typeName + "_", StringComparison.Ordinal))
            {
                value = value.Substring(typeName.Length + 1);
            }
            else if (value.StartsWith(shortType + "_", StringComparison.Ordinal))
            {
                value = value.Substring(shortType.Length + 1);
            }
            return Sanitize(value);
        }

        private static string Sanitize(string value)
        {
            if (value.Length == 0)
            {
                return "Unnamed";
            }

            var result = new StringBuilder(value.Length + 1);
            if (!char.IsLetter(value[0]) && value[0] != '_')
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

        private static string Identifier(string value) => "@" + Sanitize(value);
        private static string Escape(string value) =>
            value.Replace("\\", "\\\\").Replace("\"", "\\\"");

        private sealed class RenderedParameter
        {
            public RenderedParameter(BindingParameter parameter, string type, int nativeIndex)
            {
                Parameter = parameter;
                Type = type;
                NativeIndex = nativeIndex;
            }

            public BindingParameter Parameter { get; }
            public string Type { get; }
            public int NativeIndex { get; }
            public string? DefaultLiteral { get; set; }
        }
    }

    private sealed class BindingManifest
    {
        public List<BindingType> Types { get; } = new();
        public List<BindingProperty> Properties { get; } = new();
        public List<BindingFunction> Functions { get; } = new();
        public List<BindingMessage> Messages { get; } = new();
        public List<BindingDiagnostic> Diagnostics { get; } = new();

        public static BindingManifest Parse(string json)
        {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            if (root.GetProperty("manifestVersion").GetInt32() != 1)
            {
                throw new InvalidDataException("Only Plasma binding manifest version 1 is supported.");
            }

            var manifest = new BindingManifest();
            foreach (JsonElement item in root.GetProperty("types").EnumerateArray())
            {
                manifest.Types.Add(BindingType.Parse(item));
            }
            foreach (JsonElement item in root.GetProperty("properties").EnumerateArray())
            {
                manifest.Properties.Add(BindingProperty.Parse(item));
            }
            foreach (JsonElement item in root.GetProperty("functions").EnumerateArray())
            {
                manifest.Functions.Add(BindingFunction.Parse(item));
            }
            foreach (JsonElement item in root.GetProperty("messages").EnumerateArray())
            {
                manifest.Messages.Add(BindingMessage.Parse(item));
            }
            foreach (JsonElement item in root.GetProperty("diagnostics").EnumerateArray())
            {
                manifest.Diagnostics.Add(BindingDiagnostic.Parse(item));
            }
            return manifest;
        }
    }

    /// <summary>The engine's API descriptions (ScriptApiDocs.json, copied as PlasmaScriptDocs.json):
    /// types by native name, members by "Type::Member".</summary>
    private sealed class ApiDocs
    {
        private readonly Dictionary<string, string> _types = new(StringComparer.Ordinal);
        private readonly Dictionary<string, string> _members = new(StringComparer.Ordinal);

        public static ApiDocs Parse(string? json)
        {
            var docs = new ApiDocs();
            if (string.IsNullOrEmpty(json))
                return docs;

            try
            {
                using JsonDocument document = JsonDocument.Parse(json!);
                Read(document.RootElement, "types", docs._types);
                Read(document.RootElement, "members", docs._members);
            }
            catch (JsonException)
            {
                // Bindings without doc comments are still correct bindings.
            }

            return docs;
        }

        public string Type(string nativeName) =>
            _types.TryGetValue(nativeName, out string? text) ? text : string.Empty;

        /// <summary>Reflected names often differ from the C++ ones by a "Reflection_" prefix.</summary>
        public string Member(string nativeType, string nativeMember)
        {
            if (_members.TryGetValue(nativeType + "::" + nativeMember, out string? text))
                return text;

            const string reflectionPrefix = "Reflection_";
            if (nativeMember.StartsWith(reflectionPrefix, StringComparison.Ordinal) &&
                _members.TryGetValue(nativeType + "::" + nativeMember.Substring(reflectionPrefix.Length), out text))
                return text;

            return string.Empty;
        }

        private static void Read(JsonElement root, string name, Dictionary<string, string> target)
        {
            if (!root.TryGetProperty(name, out JsonElement entries) || entries.ValueKind != JsonValueKind.Object)
                return;

            foreach (JsonProperty entry in entries.EnumerateObject())
            {
                if (entry.Value.ValueKind == JsonValueKind.String)
                    target[entry.Name] = entry.Value.GetString() ?? string.Empty;
            }
        }
    }

    private sealed class BindingType
    {
        public string Id { get; private set; } = string.Empty;
        public string NativeName { get; private set; } = string.Empty;
        public string ParentNativeName { get; private set; } = string.Empty;
        public string ScriptExtensionName { get; private set; } = string.Empty;
        public int Flags { get; private set; }
        public int TypeSize { get; private set; }
        public string EnumStorageTypeName { get; private set; } = string.Empty;
        public bool Hidden { get; private set; }
        public bool ExcludedFromScript { get; private set; }

        public static BindingType Parse(JsonElement item) => new()
        {
            Id = Hex(item, "id"),
            NativeName = String(item, "nativeName"),
            ParentNativeName = String(item, "parentNativeName"),
            ScriptExtensionName = String(item, "scriptExtensionName"),
            Flags = item.GetProperty("flags").GetInt32(),
            TypeSize = item.GetProperty("typeSize").GetInt32(),
            EnumStorageTypeName = String(item, "enumStorageTypeName"),
            Hidden = item.GetProperty("hidden").GetBoolean(),
            ExcludedFromScript = item.GetProperty("excludedFromScript").GetBoolean(),
        };
    }

    private sealed class BindingProperty
    {
        public string Id { get; private set; } = string.Empty;
        public string DeclaringTypeId { get; private set; } = string.Empty;
        public string DeclaringTypeName { get; private set; } = string.Empty;
        public string NativeName { get; private set; } = string.Empty;
        public int Category { get; private set; }
        public int Flags { get; private set; }
        public string ValueTypeName { get; private set; } = string.Empty;
        public bool HasDefaultValue { get; private set; }
        public JsonElement DefaultValue { get; private set; }

        public static BindingProperty Parse(JsonElement item) => new()
        {
            Id = Hex(item, "id"),
            DeclaringTypeId = Hex(item, "declaringTypeId"),
            DeclaringTypeName = String(item, "declaringTypeName"),
            NativeName = String(item, "nativeName"),
            Category = item.GetProperty("category").GetInt32(),
            Flags = item.GetProperty("flags").GetInt32(),
            ValueTypeName = String(item, "valueTypeName"),
            HasDefaultValue = item.GetProperty("hasDefaultValue").GetBoolean(),
            DefaultValue = item.TryGetProperty("defaultValue", out JsonElement value)
                ? value.Clone()
                : default,
        };
    }

    private sealed class BindingFunction
    {
        public string Id { get; private set; } = string.Empty;
        public string CanonicalIdentity { get; private set; } = string.Empty;
        public string DeclaringTypeId { get; private set; } = string.Empty;
        public string DeclaringTypeName { get; private set; } = string.Empty;
        public string NativeName { get; private set; } = string.Empty;
        public string PublicName { get; private set; } = string.Empty;
        public int FunctionType { get; private set; }
        public string ReturnTypeId { get; private set; } = string.Empty;
        public string ReturnTypeName { get; private set; } = string.Empty;
        public int ReturnFlags { get; private set; }
        public List<BindingParameter> Parameters { get; } = new();

        public static BindingFunction Parse(JsonElement item)
        {
            var value = new BindingFunction
            {
                Id = Hex(item, "id"),
                CanonicalIdentity = String(item, "canonicalIdentity"),
                DeclaringTypeId = Hex(item, "declaringTypeId"),
                DeclaringTypeName = String(item, "declaringTypeName"),
                NativeName = String(item, "nativeName"),
                PublicName = String(item, "publicName"),
                FunctionType = item.GetProperty("functionType").GetInt32(),
                ReturnTypeId = Hex(item, "returnTypeId"),
                ReturnTypeName = String(item, "returnTypeName"),
                ReturnFlags = item.GetProperty("returnFlags").GetInt32(),
            };
            foreach (JsonElement parameter in item.GetProperty("parameters").EnumerateArray())
            {
                value.Parameters.Add(BindingParameter.Parse(parameter));
            }
            return value;
        }
    }

    private sealed class BindingParameter
    {
        public string Name { get; private set; } = string.Empty;
        public string Direction { get; private set; } = "in";
        public string TypeName { get; private set; } = string.Empty;
        public int Flags { get; private set; }
        public bool HasDefaultValue { get; private set; }
        public JsonElement DefaultValue { get; private set; }

        public static BindingParameter Parse(JsonElement item) => new()
        {
            Name = String(item, "name"),
            Direction = String(item, "direction"),
            TypeName = String(item, "typeName"),
            Flags = item.GetProperty("flags").GetInt32(),
            HasDefaultValue = item.GetProperty("hasDefaultValue").GetBoolean(),
            DefaultValue = item.TryGetProperty("defaultValue", out JsonElement value)
                ? value.Clone()
                : default,
        };
    }

    private sealed class BindingMessage
    {
        public string TypeName { get; private set; } = string.Empty;
        public List<string> Properties { get; } = new();
        public List<string> WritableProperties { get; } = new();

        public static BindingMessage Parse(JsonElement item)
        {
            var value = new BindingMessage { TypeName = String(item, "typeName") };
            value.Properties.AddRange(item.GetProperty("properties").EnumerateArray()
                .Select(static id => id.GetString() ?? string.Empty));
            value.WritableProperties.AddRange(item.GetProperty("writableProperties").EnumerateArray()
                .Select(static id => id.GetString() ?? string.Empty));
            return value;
        }
    }

    private sealed class BindingDiagnostic
    {
        public string Severity { get; private set; } = string.Empty;
        public string Code { get; private set; } = string.Empty;
        public string TypeName { get; private set; } = string.Empty;
        public string MemberName { get; private set; } = string.Empty;
        public string ParameterName { get; private set; } = string.Empty;
        public string Message { get; private set; } = string.Empty;

        public string Location
        {
            get
            {
                string location = TypeName;
                if (MemberName.Length > 0)
                {
                    location += "." + MemberName;
                }
                if (ParameterName.Length > 0)
                {
                    location += "(" + ParameterName + ")";
                }
                return location.Length > 0 ? location : "<manifest>";
            }
        }

        public static BindingDiagnostic Parse(JsonElement item) => new()
        {
            Severity = String(item, "severity"),
            Code = String(item, "code"),
            TypeName = String(item, "typeName"),
            MemberName = String(item, "memberName"),
            ParameterName = String(item, "parameterName"),
            Message = String(item, "message"),
        };
    }

    private static string Hex(JsonElement item, string name) =>
        (item.GetProperty(name).GetString() ?? "0000000000000000")
        .Replace("0x", string.Empty);

    private static string String(JsonElement item, string name) =>
        item.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;
}
