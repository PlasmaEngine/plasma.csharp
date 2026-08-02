using System.Globalization;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using Plasma;

namespace Plasma.ScriptInspector;

internal static class Program
{
    private const uint InspectorVersion = 1;

    public static int Main(string[] args)
    {
        try
        {
            if (!TryReadArguments(args, out string assemblyPath, out string outputPath))
            {
                Console.Error.WriteLine(
                    "Usage: Plasma.ScriptInspector --assembly <absolute-path> --output <absolute-path>");
                return 2;
            }

            Inspect(assemblyPath, outputPath);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static bool TryReadArguments(
        IReadOnlyList<string> args,
        out string assemblyPath,
        out string outputPath)
    {
        assemblyPath = string.Empty;
        outputPath = string.Empty;

        for (int index = 0; index + 1 < args.Count; index += 2)
        {
            switch (args[index])
            {
                case "--assembly":
                    assemblyPath = Path.GetFullPath(args[index + 1]);
                    break;
                case "--output":
                    outputPath = Path.GetFullPath(args[index + 1]);
                    break;
                default:
                    return false;
            }
        }

        return args.Count == 4 &&
            File.Exists(assemblyPath) &&
            !string.IsNullOrWhiteSpace(outputPath);
    }

    private static void Inspect(string assemblyPath, string outputPath)
    {
        string fullAssemblyPath = Path.GetFullPath(assemblyPath);
        var loadContext = new InspectorLoadContext(fullAssemblyPath);

        try
        {
            Assembly assembly = loadContext.LoadFromAssemblyPath(fullAssemblyPath);
            IReadOnlyList<ScriptTypeDescriptor> descriptors = DiscoverDescriptors(assembly);
            object manifest = BuildManifest(Path.GetFileName(fullAssemblyPath), descriptors);

            Directory.CreateDirectory(
                Path.GetDirectoryName(outputPath) ??
                throw new InvalidDataException($"Output path '{outputPath}' has no directory."));

            string temporaryPath = outputPath + ".tmp";
            using (FileStream stream = File.Create(temporaryPath))
            {
                JsonSerializer.Serialize(stream, manifest, new JsonSerializerOptions
                {
                    WriteIndented = true,
                    PropertyNamingPolicy = null,
                });
            }

            File.Move(temporaryPath, outputPath, overwrite: true);
        }
        finally
        {
            loadContext.Unload();
        }
    }

    private static IReadOnlyList<ScriptTypeDescriptor> DiscoverDescriptors(Assembly assembly)
    {
        var descriptorsByTypeId = new SortedDictionary<ulong, ScriptTypeDescriptor>();
        var persistentIds = new HashSet<Guid>();

        foreach (Type providerType in assembly.GetTypes()
                     .Where(static type =>
                         !type.IsAbstract &&
                         typeof(IScriptDescriptorProvider).IsAssignableFrom(type))
                     .OrderBy(static type => type.FullName, StringComparer.Ordinal))
        {
            if (Activator.CreateInstance(providerType) is not IScriptDescriptorProvider provider)
            {
                throw new InvalidDataException(
                    $"Could not create script descriptor provider '{providerType.FullName}'.");
            }

            foreach (ScriptTypeDescriptor descriptor in provider.GetDescriptors()
                         .OrderBy(static descriptor => descriptor.Id))
            {
                if (descriptor.PersistentId == Guid.Empty)
                {
                    throw new InvalidDataException(
                        $"C# script '{descriptor.ManagedName}' has an empty persistent ID. " +
                        "Rebuild it with a compatible Plasma source generator.");
                }

                if (!persistentIds.Add(descriptor.PersistentId))
                {
                    throw new InvalidDataException(
                        $"Duplicate persistent C# script ID '{descriptor.PersistentId:D}'.");
                }

                if (!descriptorsByTypeId.TryAdd(descriptor.Id, descriptor))
                {
                    throw new InvalidDataException(
                        $"Duplicate C# script type ID 0x{descriptor.Id:X16}.");
                }
            }
        }

        if (descriptorsByTypeId.Count == 0)
        {
            throw new InvalidDataException(
                $"Assembly '{assembly.FullName}' contains no Plasma script descriptor provider.");
        }

        return [.. descriptorsByTypeId.Values];
    }

    private static object BuildManifest(
        string entryAssembly,
        IReadOnlyList<ScriptTypeDescriptor> descriptors)
    {
        return new
        {
            inspectorVersion = InspectorVersion,
            entryAssembly,
            classes = descriptors.Select(static descriptor => new
            {
                persistentGuid = descriptor.PersistentId.ToString("D", CultureInfo.InvariantCulture),
                typeId = HexId(descriptor.Id),
                managedName = descriptor.ManagedName,
                sourceFile = descriptor.SourceFile,
                fields = descriptor.Fields.Values
                    .OrderBy(static field => field.Id)
                    .Select(static field => new
                    {
                        id = HexId(field.Id),
                        name = field.Name,
                        managedType = field.ValueType.FullName ?? field.ValueType.Name,
                        defaultValue = ConvertDefaultValue(field.DefaultValue),
                        metadata = field.EditorMetadata
                            .OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                            .ToDictionary(
                                static pair => pair.Key,
                                static pair => pair.Value,
                                StringComparer.Ordinal),
                    }),
            }),
        };
    }

    private static object ConvertDefaultValue(object? value)
    {
        return value switch
        {
            null => new { kind = "null", value = string.Empty, components = (object?)null },
            bool boolean => Scalar("boolean", boolean ? "true" : "false"),
            sbyte or short or int or long =>
                Scalar("signed", Convert.ToInt64(value, CultureInfo.InvariantCulture)
                    .ToString(CultureInfo.InvariantCulture)),
            byte or ushort or uint or ulong =>
                Scalar("unsigned", Convert.ToUInt64(value, CultureInfo.InvariantCulture)
                    .ToString(CultureInfo.InvariantCulture)),
            float or double or decimal =>
                Scalar("floating", Convert.ToDouble(value, CultureInfo.InvariantCulture)
                    .ToString("R", CultureInfo.InvariantCulture)),
            string text => Scalar("string", text),
            Enum enumeration => ConvertEnum(enumeration),
            Time time => Scalar("time", Format(time.Seconds)),
            Angle angle => Scalar("angle", Format(angle.Radians)),
            Vec2 vector => Composite("vec2", new { x = Format(vector.X), y = Format(vector.Y) }),
            Vec3 vector => Composite(
                "vec3",
                new { x = Format(vector.X), y = Format(vector.Y), z = Format(vector.Z) }),
            Vec4 vector => Composite(
                "vec4",
                new
                {
                    x = Format(vector.X),
                    y = Format(vector.Y),
                    z = Format(vector.Z),
                    w = Format(vector.W),
                }),
            Quat quaternion => Composite(
                "quat",
                new
                {
                    x = Format(quaternion.X),
                    y = Format(quaternion.Y),
                    z = Format(quaternion.Z),
                    w = Format(quaternion.W),
                }),
            Color color => Composite(
                "color",
                new
                {
                    r = Format(color.R),
                    g = Format(color.G),
                    b = Format(color.B),
                    a = Format(color.A),
                }),
            Transform transform => Composite(
                "transform",
                new
                {
                    px = Format(transform.Position.X),
                    py = Format(transform.Position.Y),
                    pz = Format(transform.Position.Z),
                    rx = Format(transform.Rotation.X),
                    ry = Format(transform.Rotation.Y),
                    rz = Format(transform.Rotation.Z),
                    rw = Format(transform.Rotation.W),
                    sx = Format(transform.Scale.X),
                    sy = Format(transform.Scale.Y),
                    sz = Format(transform.Scale.Z),
                }),
            NativeObject native => NativeReference("nativeObject", native),
            World world => NativeReference("world", world.Native),
            GameObject gameObject => NativeReference("gameObject", gameObject.Native),
            Component component => NativeReference("component", component.Native),
            _ => ConvertResourceHandle(value),
        };
    }

    private static object NativeReference(string kind, NativeObject value) =>
        Composite(
            kind,
            new
            {
                id = value.Id.ToString(CultureInfo.InvariantCulture),
                generation = value.Generation.ToString(CultureInfo.InvariantCulture),
                objectKind = ((uint)value.Kind).ToString(CultureInfo.InvariantCulture),
            });

    private static object ConvertResourceHandle(object value)
    {
        Type valueType = value.GetType();
        if (valueType.IsGenericType &&
            valueType.GetGenericTypeDefinition() == typeof(ResourceHandle<>))
        {
            ulong id = (ulong)(valueType.GetProperty(nameof(ResourceHandle<object>.Id))?.GetValue(value)
                ?? throw new InvalidDataException($"Resource handle '{valueType}' has no ID."));
            ulong generation =
                (ulong)(valueType.GetProperty(nameof(ResourceHandle<object>.Generation))?.GetValue(value)
                    ?? throw new InvalidDataException($"Resource handle '{valueType}' has no generation."));
            return Composite(
                "resourceHandle",
                new
                {
                    id = id.ToString(CultureInfo.InvariantCulture),
                    generation = generation.ToString(CultureInfo.InvariantCulture),
                });
        }

        throw new InvalidDataException(
            $"Exposed default value type '{valueType.FullName}' is not supported by the editor inspector.");
    }

    private static object Scalar(string kind, string value) =>
        new { kind, value, components = (object?)null };

    private static object Composite(string kind, object components) =>
        new { kind, value = string.Empty, components };

    private static string Format(double value) =>
        value.ToString("R", CultureInfo.InvariantCulture);

    private static object ConvertEnum(Enum value)
    {
        Type underlyingType = Enum.GetUnderlyingType(value.GetType());
        bool signed = Type.GetTypeCode(underlyingType) is
            TypeCode.SByte or TypeCode.Int16 or TypeCode.Int32 or TypeCode.Int64;
        return signed
            ? Scalar(
                "enumSigned",
                Convert.ToInt64(value, CultureInfo.InvariantCulture)
                    .ToString(CultureInfo.InvariantCulture))
            : Scalar(
                "enumUnsigned",
                Convert.ToUInt64(value, CultureInfo.InvariantCulture)
                    .ToString(CultureInfo.InvariantCulture));
    }

    private static string HexId(ulong value) =>
        value.ToString("X16", CultureInfo.InvariantCulture);

    private sealed class InspectorLoadContext : AssemblyLoadContext
    {
        private readonly AssemblyDependencyResolver _resolver;

        public InspectorLoadContext(string assemblyPath)
            : base($"Plasma.ScriptInspector:{assemblyPath}", isCollectible: true)
        {
            _resolver = new AssemblyDependencyResolver(assemblyPath);
        }

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            Assembly scriptCore = typeof(IScriptDescriptorProvider).Assembly;
            if (AssemblyName.ReferenceMatchesDefinition(scriptCore.GetName(), assemblyName))
            {
                return scriptCore;
            }

            string? resolvedPath = _resolver.ResolveAssemblyToPath(assemblyName);
            return resolvedPath is null ? null : LoadFromAssemblyPath(resolvedPath);
        }
    }
}
