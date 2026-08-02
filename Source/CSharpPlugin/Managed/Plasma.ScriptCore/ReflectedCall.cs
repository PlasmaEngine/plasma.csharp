using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace Plasma;

/// <summary>Generic reflected invocation path used by generated engine wrappers.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ReflectedCall
{
    public static void Invoke(
        ulong functionId,
        NativeObject target,
        object?[] arguments,
        Type[] argumentTypes)
    {
        Invoke(functionId, $"binding 0x{functionId:X16}", target, arguments, argumentTypes);
    }

    public static void Invoke(
        ulong functionId,
        string apiName,
        NativeObject target,
        object?[] arguments,
        Type[] argumentTypes)
    {
        _ = InvokeCore(functionId, apiName, target, arguments, argumentTypes, typeof(void));
    }

    public static TResult Invoke<TResult>(
        ulong functionId,
        NativeObject target,
        object?[] arguments,
        Type[] argumentTypes)
    {
        return Invoke<TResult>(
            functionId,
            $"binding 0x{functionId:X16}",
            target,
            arguments,
            argumentTypes);
    }

    public static TResult Invoke<TResult>(
        ulong functionId,
        string apiName,
        NativeObject target,
        object?[] arguments,
        Type[] argumentTypes)
    {
        object? result = InvokeCore(
            functionId,
            apiName,
            target,
            arguments,
            argumentTypes,
            typeof(TResult));
        return result is null ? default! : (TResult)result;
    }

    private static object? InvokeCore(
        ulong functionId,
        string apiName,
        NativeObject target,
        object?[] arguments,
        Type[] argumentTypes,
        Type resultType)
    {
        if (arguments.Length != argumentTypes.Length)
        {
            throw new ArgumentException("Argument values and type metadata must have the same length.");
        }

        var nativeArguments = new NativeValue[arguments.Length];
        var leases = new List<IDisposable>(arguments.Length);
        NativeValue result = default;
        try
        {
            for (int index = 0; index < arguments.Length; ++index)
            {
                nativeArguments[index] = ToNative(arguments[index], argumentTypes[index], leases);
            }

            NativeCallStatus status =
                NativeBridge.InvokeReflected(functionId, target, nativeArguments, out result);
            if (status != NativeCallStatus.Success)
            {
                throw new PlasmaException(
                    DescribeFailure(apiName, functionId, status),
                    (int)status);
            }

            for (int index = 0; index < arguments.Length; ++index)
            {
                arguments[index] = FromNative(nativeArguments[index], argumentTypes[index]);
            }

            return resultType == typeof(void) ? null : FromNative(result, resultType);
        }
        finally
        {
            if ((result.Flags & NativeValueFlags.ManagedOwned) != 0)
            {
                _ = NativeBridge.ReleaseValue(ref result);
            }

            for (int index = 0; index < nativeArguments.Length; ++index)
            {
                if ((nativeArguments[index].Flags & NativeValueFlags.ManagedOwned) != 0)
                {
                    _ = NativeBridge.ReleaseValue(ref nativeArguments[index]);
                }
            }

            foreach (IDisposable lease in leases)
            {
                lease.Dispose();
            }
        }
    }

    private static string DescribeFailure(
        string apiName,
        ulong functionId,
        NativeCallStatus status)
    {
        string guidance = status switch
        {
            NativeCallStatus.MemberNotFound =>
                "This engine API is not available in the running game. Rebuild the C# scripts, " +
                "then make sure the plugin that provides this API is enabled for the project.",
            NativeCallStatus.InstanceNotFound =>
                "The target object no longer exists. Avoid retaining engine objects after they are destroyed.",
            NativeCallStatus.InvalidArgument =>
                "The target or an argument is invalid. Check that the engine object is still valid and the argument values match the API.",
            NativeCallStatus.InvalidValue =>
                "A value could not be converted between C# and the engine. Check the argument and return types.",
            NativeCallStatus.NotInitialized =>
                "C# scripting is not ready. Check the earlier log entries for a runtime initialization error.",
            NativeCallStatus.GenerationNotFound =>
                "This script belongs to an old compiled generation. Rebuild the C# scripts and restart simulation.",
            NativeCallStatus.Unsupported =>
                "This API or value type is not supported by the current C# bridge.",
            _ => $"The engine returned {status}. Check the preceding log entries for more detail.",
        };

        return $"Engine API '{apiName}' failed. {guidance} " +
               $"[Diagnostic: {status}, binding 0x{functionId:X16}]";
    }

    private static NativeValue ToNative(object? value, Type expectedType, ICollection<IDisposable> leases)
    {
        if (value is null)
        {
            return default;
        }

        Type type = Nullable.GetUnderlyingType(expectedType) ?? expectedType;
        if (type == typeof(object))
        {
            // Generated plVariant parameters use object so callers can pass the
            // actual managed value without constructing a bridge-specific box.
            type = value.GetType();
        }
        if (type.IsEnum)
        {
            Type underlyingType = Enum.GetUnderlyingType(type);
            object underlyingValue = Convert.ChangeType(value, underlyingType, CultureInfo.InvariantCulture);
            bool unsigned = underlyingType == typeof(byte) ||
                            underlyingType == typeof(ushort) ||
                            underlyingType == typeof(uint) ||
                            underlyingType == typeof(ulong);
            return new NativeValue
            {
                Kind = unsigned ? NativeValueKind.UInt64 : NativeValueKind.Int64,
                Payload0 = unsigned
                    ? Convert.ToUInt64(underlyingValue, CultureInfo.InvariantCulture)
                    : unchecked((ulong)Convert.ToInt64(underlyingValue, CultureInfo.InvariantCulture)),
            };
        }

        if (value is TempHashedString hashedString)
        {
            return new NativeValue
            {
                Kind = NativeValueKind.UInt64,
                Payload0 = hashedString.Hash,
            };
        }

        switch (Type.GetTypeCode(type))
        {
            case TypeCode.Boolean:
                return new NativeValue
                {
                    Kind = NativeValueKind.Boolean,
                    Payload0 = (bool)value ? 1UL : 0UL,
                };
            case TypeCode.SByte:
            case TypeCode.Int16:
            case TypeCode.Int32:
            case TypeCode.Int64:
                return new NativeValue
                {
                    Kind = NativeValueKind.Int64,
                    Payload0 = unchecked((ulong)Convert.ToInt64(value, CultureInfo.InvariantCulture)),
                };
            case TypeCode.Byte:
            case TypeCode.UInt16:
            case TypeCode.UInt32:
            case TypeCode.UInt64:
                return new NativeValue
                {
                    Kind = NativeValueKind.UInt64,
                    Payload0 = Convert.ToUInt64(value, CultureInfo.InvariantCulture),
                };
            case TypeCode.Single:
            case TypeCode.Double:
            case TypeCode.Decimal:
                return new NativeValue
                {
                    Kind = NativeValueKind.Double,
                    Payload0 = unchecked((ulong)BitConverter.DoubleToInt64Bits(
                        Convert.ToDouble(value, CultureInfo.InvariantCulture))),
                };
            case TypeCode.String:
            {
                var lease = new PinnedBytes(Encoding.UTF8.GetBytes((string)value));
                leases.Add(lease);
                return lease.AsValue(NativeValueKind.Utf8String);
            }
        }

        if (value is NativeObject nativeObject)
        {
            if (!nativeObject.IsValid)
            {
                return default;
            }

            return new NativeValue
            {
                Kind = NativeValueKind.ObjectHandle,
                Flags = ObjectKindFlags(nativeObject.Kind),
                Payload0 = nativeObject.Id,
                Payload1 = nativeObject.Generation,
            };
        }

        NativeObject? wrappedObject = TryGetWrappedNativeObject(value);
        if (wrappedObject is not null)
        {
            if (!wrappedObject.Value.IsValid)
            {
                return default;
            }

            return new NativeValue
            {
                Kind = NativeValueKind.ObjectHandle,
                Flags = ObjectKindFlags(wrappedObject.Value.Kind),
                Payload0 = wrappedObject.Value.Id,
                Payload1 = wrappedObject.Value.Generation,
            };
        }

        if (type.IsValueType)
        {
            int size = Marshal.SizeOf(type);
            byte[] bytes = new byte[size];
            var lease = new PinnedBytes(bytes);
            Marshal.StructureToPtr(value, lease.Pointer, fDeleteOld: false);
            leases.Add(lease);
            return lease.AsValue(NativeValueKind.ByteSpan);
        }

        throw new PlasmaException($"Type '{type}' cannot cross the Plasma native boundary.");
    }

    private static object? FromNative(NativeValue value, Type expectedType)
    {
        Type type = Nullable.GetUnderlyingType(expectedType) ?? expectedType;
        if (value.Kind == NativeValueKind.Null)
        {
            return expectedType.IsValueType && Nullable.GetUnderlyingType(expectedType) is null
                ? Activator.CreateInstance(expectedType)
                : null;
        }

        object primitive = value.Kind switch
        {
            NativeValueKind.Boolean => value.Payload0 != 0,
            NativeValueKind.Int64 => unchecked((long)value.Payload0),
            NativeValueKind.UInt64 => value.Payload0,
            NativeValueKind.Double => BitConverter.Int64BitsToDouble(unchecked((long)value.Payload0)),
            NativeValueKind.Utf8String => Marshal.PtrToStringUTF8(
                (nint)value.Payload0,
                checked((int)value.Payload1)) ?? string.Empty,
            NativeValueKind.ObjectHandle => new NativeObject(
                value.Payload0,
                value.Payload1,
                ObjectKindFromFlags(value.Flags) is { } kind
                    ? kind
                    : NativeKindFor(type)),
            NativeValueKind.ByteSpan => Marshal.PtrToStructure((nint)value.Payload0, type)
                ?? throw new PlasmaException($"Could not decode native value as '{type}'."),
            _ => throw new PlasmaException($"Native value kind '{value.Kind}' is unsupported."),
        };

        if (type.IsEnum)
        {
            object underlying = Convert.ChangeType(
                primitive,
                Enum.GetUnderlyingType(type),
                CultureInfo.InvariantCulture);
            return Enum.ToObject(type, underlying);
        }

        if (type == typeof(TempHashedString))
        {
            return new TempHashedString(Convert.ToUInt64(primitive, CultureInfo.InvariantCulture));
        }

        if (primitive is NativeObject nativeObject && type != typeof(NativeObject))
        {
            return Activator.CreateInstance(type, nativeObject)
                   ?? throw new PlasmaException($"Could not construct native wrapper '{type}'.");
        }

        return type.IsInstanceOfType(primitive)
            ? primitive
            : Convert.ChangeType(primitive, type, CultureInfo.InvariantCulture);
    }

    private static NativeObject? TryGetWrappedNativeObject(object value)
    {
        object? property = value.GetType().GetProperty(nameof(GameObject.Native))?.GetValue(value);
        return property is NativeObject native ? native : null;
    }

    private static NativeObjectKind NativeKindFor(Type type)
    {
        if (type == typeof(World))
        {
            return NativeObjectKind.World;
        }

        if (type == typeof(GameObject))
        {
            return NativeObjectKind.GameObject;
        }

        return type == typeof(Component) || type.Name.EndsWith("Component", StringComparison.Ordinal)
            ? NativeObjectKind.Component
            : NativeObjectKind.ReflectedObject;
    }

    private static NativeValueFlags ObjectKindFlags(NativeObjectKind kind) =>
        (NativeValueFlags)((uint)kind << 8);

    private static NativeObjectKind? ObjectKindFromFlags(NativeValueFlags flags)
    {
        uint kind = ((uint)flags >> 8) & 0xFFu;
        return kind is >= (uint)NativeObjectKind.World and <= (uint)NativeObjectKind.ReflectedObject
            ? (NativeObjectKind)kind
            : null;
    }

    private sealed class PinnedBytes : IDisposable
    {
        private GCHandle _handle;

        public PinnedBytes(byte[] bytes)
        {
            Bytes = bytes;
            _handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        }

        public byte[] Bytes { get; }
        public nint Pointer => _handle.AddrOfPinnedObject();

        public NativeValue AsValue(NativeValueKind kind) => new()
        {
            Kind = kind,
            Payload0 = unchecked((ulong)Pointer),
            Payload1 = checked((ulong)Bytes.Length),
        };

        public void Dispose()
        {
            if (_handle.IsAllocated)
            {
                _handle.Free();
            }
        }
    }
}
