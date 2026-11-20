using System.Globalization;
using System.Runtime.CompilerServices;

namespace Plasma;

/// <summary>What a <see cref="Variant"/> holds.</summary>
public enum VariantKind : byte
{
    Null,
    Bool,
    Int,
    UInt,
    Double,
    String,
    Object,
    Time,
    Angle,
    Vec2,
    Vec2U32,
    Vec3,
    Vec4,
    Quat,
    Color,
    Transform,
}

/// <summary>A value of any engine type, as taken and returned by engine "any value" slots such as
/// blackboard entries and data-table cells.</summary>
/// <remarks>
/// Numbers, vectors, colours and object references are stored without boxing. Values convert
/// implicitly from the types they hold, explicitly back to them, and through <see cref="Convert"/>:
/// <c>int score = (int)board.GetEntryValue("Score");</c> and <c>Convert.ToSingle(value)</c> both work,
/// whichever integer or floating-point width the engine stored.
/// </remarks>
[System.Text.Json.Serialization.JsonConverter(typeof(VariantJsonConverter))]
public readonly struct Variant : IEquatable<Variant>, IConvertible
{
    private readonly ulong _a;
    private readonly ulong _b;
    private readonly object? _reference;
    private readonly NativeObjectKind _objectKind;

    public VariantKind Kind { get; }

    private Variant(VariantKind kind, ulong a, ulong b = 0, object? reference = null, NativeObjectKind objectKind = NativeObjectKind.Invalid)
    {
        Kind = kind;
        _a = a;
        _b = b;
        _reference = reference;
        _objectKind = objectKind;
    }

    public static Variant Null => default;
    public bool IsNull => Kind == VariantKind.Null;
    public bool IsNumber => Kind is VariantKind.Bool or VariantKind.Int or VariantKind.UInt or VariantKind.Double;

    // ---------------------------------------------------------------- in

    public static implicit operator Variant(bool value) => new(VariantKind.Bool, value ? 1UL : 0UL);
    public static implicit operator Variant(int value) => new(VariantKind.Int, unchecked((ulong)(long)value));
    public static implicit operator Variant(long value) => new(VariantKind.Int, unchecked((ulong)value));
    public static implicit operator Variant(uint value) => new(VariantKind.UInt, value);
    public static implicit operator Variant(ulong value) => new(VariantKind.UInt, value);
    public static implicit operator Variant(float value) => new(VariantKind.Double, unchecked((ulong)BitConverter.DoubleToInt64Bits(value)));
    public static implicit operator Variant(double value) => new(VariantKind.Double, unchecked((ulong)BitConverter.DoubleToInt64Bits(value)));
    public static implicit operator Variant(string? value) => value is null ? default : new(VariantKind.String, 0, 0, value);
    public static implicit operator Variant(NativeObject value) => value.IsValid ? new(VariantKind.Object, value.Id, value.Generation, null, value.Kind) : default;
    public static implicit operator Variant(GameObject value) => value.Native;
    public static implicit operator Variant(Component value) => value.Native;
    public static implicit operator Variant(World value) => value.Native;
    public static implicit operator Variant(Time value) => FromBytes(VariantKind.Time, value);
    public static implicit operator Variant(Angle value) => FromBytes(VariantKind.Angle, value);
    public static implicit operator Variant(Vec2 value) => FromBytes(VariantKind.Vec2, value);
    public static implicit operator Variant(Vec2U32 value) => FromBytes(VariantKind.Vec2U32, value);
    public static implicit operator Variant(Vec3 value) => FromBytes(VariantKind.Vec3, value);
    public static implicit operator Variant(Vec4 value) => FromBytes(VariantKind.Vec4, value);
    public static implicit operator Variant(Quat value) => FromBytes(VariantKind.Quat, value);
    public static implicit operator Variant(Color value) => FromBytes(VariantKind.Color, value);
    public static implicit operator Variant(Transform value) => new(VariantKind.Transform, 0, 0, value);

    /// <summary>Wraps a value whose type is only known at run time; null and unsupported types give <see cref="Null"/>.</summary>
    public static Variant FromObject(object? value) => value switch
    {
        null => default,
        Variant variant => variant,
        bool v => v,
        sbyte v => (long)v,
        byte v => (ulong)v,
        short v => (long)v,
        ushort v => (ulong)v,
        int v => v,
        uint v => v,
        long v => v,
        ulong v => v,
        float v => v,
        double v => v,
        string v => v,
        NativeObject v => v,
        GameObject v => v,
        Component v => v,
        World v => v,
        Time v => v,
        Angle v => v,
        Vec2 v => v,
        Vec2U32 v => v,
        Vec3 v => v,
        Vec4 v => v,
        Quat v => v,
        Color v => v,
        Transform v => v,
        Enum v => Convert.ToInt64(v, CultureInfo.InvariantCulture),
        _ => default,
    };

    // ---------------------------------------------------------------- out

    public bool AsBool() => Kind switch
    {
        VariantKind.Bool or VariantKind.Int or VariantKind.UInt => _a != 0,
        VariantKind.Double => AsDouble() != 0,
        VariantKind.String => bool.Parse((string)_reference!),
        _ => throw WrongKind(typeof(bool)),
    };

    public long AsInt64() => Kind switch
    {
        VariantKind.Bool or VariantKind.Int => unchecked((long)_a),
        VariantKind.UInt => checked((long)_a),
        VariantKind.Double => checked((long)AsDouble()),
        VariantKind.String => long.Parse((string)_reference!, CultureInfo.InvariantCulture),
        _ => throw WrongKind(typeof(long)),
    };

    public ulong AsUInt64() => Kind switch
    {
        VariantKind.Bool or VariantKind.UInt => _a,
        VariantKind.Int => checked((ulong)unchecked((long)_a)),
        VariantKind.Double => checked((ulong)AsDouble()),
        VariantKind.String => ulong.Parse((string)_reference!, CultureInfo.InvariantCulture),
        _ => throw WrongKind(typeof(ulong)),
    };

    public double AsDouble() => Kind switch
    {
        VariantKind.Double => BitConverter.Int64BitsToDouble(unchecked((long)_a)),
        VariantKind.Bool or VariantKind.UInt => _a,
        VariantKind.Int => unchecked((long)_a),
        VariantKind.String => double.Parse((string)_reference!, CultureInfo.InvariantCulture),
        _ => throw WrongKind(typeof(double)),
    };

    public NativeObject AsObject() => Kind switch
    {
        VariantKind.Object => new NativeObject(_a, _b, _objectKind),
        VariantKind.Null => default,
        _ => throw WrongKind(typeof(NativeObject)),
    };

    /// <summary>The value as a string: the string itself, or any other value formatted invariantly.</summary>
    public override string ToString() => Kind switch
    {
        VariantKind.Null => string.Empty,
        VariantKind.String => (string)_reference!,
        VariantKind.Bool => _a != 0 ? "true" : "false",
        VariantKind.Int => unchecked((long)_a).ToString(CultureInfo.InvariantCulture),
        VariantKind.UInt => _a.ToString(CultureInfo.InvariantCulture),
        VariantKind.Double => AsDouble().ToString(CultureInfo.InvariantCulture),
        _ => ToObject()?.ToString() ?? string.Empty,
    };

    /// <summary>The value boxed as the type it holds, or null.</summary>
    public object? ToObject() => Kind switch
    {
        VariantKind.Null => null,
        VariantKind.Bool => _a != 0,
        VariantKind.Int => unchecked((long)_a),
        VariantKind.UInt => _a,
        VariantKind.Double => AsDouble(),
        VariantKind.String or VariantKind.Transform => _reference,
        VariantKind.Object => AsObject(),
        VariantKind.Time => ToBytes<Time>(),
        VariantKind.Angle => ToBytes<Angle>(),
        VariantKind.Vec2 => ToBytes<Vec2>(),
        VariantKind.Vec2U32 => ToBytes<Vec2U32>(),
        VariantKind.Vec3 => ToBytes<Vec3>(),
        VariantKind.Vec4 => ToBytes<Vec4>(),
        VariantKind.Quat => ToBytes<Quat>(),
        VariantKind.Color => ToBytes<Color>(),
        _ => null,
    };

    public static explicit operator bool(Variant value) => value.AsBool();
    public static explicit operator int(Variant value) => checked((int)value.AsInt64());
    public static explicit operator long(Variant value) => value.AsInt64();
    public static explicit operator uint(Variant value) => checked((uint)value.AsUInt64());
    public static explicit operator ulong(Variant value) => value.AsUInt64();
    public static explicit operator float(Variant value) => (float)value.AsDouble();
    public static explicit operator double(Variant value) => value.AsDouble();
    public static explicit operator string(Variant value) => value.ToString();
    public static explicit operator NativeObject(Variant value) => value.AsObject();
    public static explicit operator GameObject(Variant value) => new(value.AsObject());
    public static explicit operator Component(Variant value) => new(value.AsObject());
    public static explicit operator Time(Variant value) => value.Bytes<Time>(VariantKind.Time);
    public static explicit operator Angle(Variant value) => value.Bytes<Angle>(VariantKind.Angle);
    public static explicit operator Vec2(Variant value) => value.Bytes<Vec2>(VariantKind.Vec2);
    public static explicit operator Vec2U32(Variant value) => value.Bytes<Vec2U32>(VariantKind.Vec2U32);
    public static explicit operator Vec3(Variant value) => value.Bytes<Vec3>(VariantKind.Vec3);
    public static explicit operator Vec4(Variant value) => value.Bytes<Vec4>(VariantKind.Vec4);
    public static explicit operator Quat(Variant value) => value.Bytes<Quat>(VariantKind.Quat);
    public static explicit operator Color(Variant value) => value.Bytes<Color>(VariantKind.Color);
    public static explicit operator Transform(Variant value) =>
        value.Kind == VariantKind.Transform ? (Transform)value._reference! : throw value.WrongKind(typeof(Transform));

    // ---------------------------------------------------------------- equality

    public bool Equals(Variant other) =>
        Kind == other.Kind && _a == other._a && _b == other._b && _objectKind == other._objectKind && Equals(_reference, other._reference);

    public override bool Equals(object? obj) => obj is Variant other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Kind, _a, _b, _reference);
    public static bool operator ==(Variant left, Variant right) => left.Equals(right);
    public static bool operator !=(Variant left, Variant right) => !left.Equals(right);

    // ---------------------------------------------------------------- IConvertible, so Convert.ToX(value) works

    TypeCode IConvertible.GetTypeCode() => Kind switch
    {
        VariantKind.Bool => TypeCode.Boolean,
        VariantKind.Int => TypeCode.Int64,
        VariantKind.UInt => TypeCode.UInt64,
        VariantKind.Double => TypeCode.Double,
        VariantKind.String => TypeCode.String,
        VariantKind.Null => TypeCode.Empty,
        _ => TypeCode.Object,
    };

    bool IConvertible.ToBoolean(IFormatProvider? provider) => AsBool();
    byte IConvertible.ToByte(IFormatProvider? provider) => checked((byte)AsUInt64());
    char IConvertible.ToChar(IFormatProvider? provider) => checked((char)AsUInt64());
    DateTime IConvertible.ToDateTime(IFormatProvider? provider) => throw new InvalidCastException("A Variant cannot be converted to DateTime.");
    decimal IConvertible.ToDecimal(IFormatProvider? provider) => Kind == VariantKind.Double ? (decimal)AsDouble() : AsInt64();
    double IConvertible.ToDouble(IFormatProvider? provider) => AsDouble();
    short IConvertible.ToInt16(IFormatProvider? provider) => checked((short)AsInt64());
    int IConvertible.ToInt32(IFormatProvider? provider) => checked((int)AsInt64());
    long IConvertible.ToInt64(IFormatProvider? provider) => AsInt64();
    sbyte IConvertible.ToSByte(IFormatProvider? provider) => checked((sbyte)AsInt64());
    float IConvertible.ToSingle(IFormatProvider? provider) => (float)AsDouble();
    string IConvertible.ToString(IFormatProvider? provider) => ToString();
    ushort IConvertible.ToUInt16(IFormatProvider? provider) => checked((ushort)AsUInt64());
    uint IConvertible.ToUInt32(IFormatProvider? provider) => checked((uint)AsUInt64());
    ulong IConvertible.ToUInt64(IFormatProvider? provider) => AsUInt64();

    object IConvertible.ToType(Type conversionType, IFormatProvider? provider)
    {
        object? value = ToObject();
        if (value is not null && conversionType.IsInstanceOfType(value))
            return value;

        throw WrongKind(conversionType);
    }

    // ---------------------------------------------------------------- bridge

    internal static unsafe Variant FromNative(in NativeValue value)
    {
        switch (value.Kind)
        {
            case NativeValueKind.Null:
                return default;
            case NativeValueKind.Boolean:
                return new(VariantKind.Bool, value.Payload0 != 0 ? 1UL : 0UL);
            case NativeValueKind.Int64:
                return new(VariantKind.Int, value.Payload0);
            case NativeValueKind.UInt64:
                return new(VariantKind.UInt, value.Payload0);
            case NativeValueKind.Double:
                return new(VariantKind.Double, value.Payload0);
            case NativeValueKind.Utf8String:
                return System.Runtime.InteropServices.Marshal.PtrToStringUTF8((nint)value.Payload0, checked((int)value.Payload1)) ?? string.Empty;
            case NativeValueKind.ObjectHandle:
            {
                uint kind = ((uint)value.Flags >> 8) & 0xFFu;
                return new(VariantKind.Object, value.Payload0, value.Payload1, null,
                    kind is >= (uint)NativeObjectKind.World and <= (uint)NativeObjectKind.ReflectedObject ? (NativeObjectKind)kind : NativeObjectKind.ReflectedObject);
            }
            case NativeValueKind.ByteSpan:
            {
                var type = (NativeByteSpanType)(((uint)value.Flags >> 16) & 0xFFu);
                NativeValue copy = value;
                void* data = (copy.Flags & NativeValueFlags.InlineBytes) != 0 ? &copy.Payload0 : (void*)copy.Payload0;
                return type switch
                {
                    NativeByteSpanType.Time => Unsafe.ReadUnaligned<Time>(data),
                    NativeByteSpanType.Angle => Unsafe.ReadUnaligned<Angle>(data),
                    NativeByteSpanType.Vec2 => Unsafe.ReadUnaligned<Vec2>(data),
                    NativeByteSpanType.Vec2U32 => Unsafe.ReadUnaligned<Vec2U32>(data),
                    NativeByteSpanType.Vec3 => Unsafe.ReadUnaligned<Vec3>(data),
                    NativeByteSpanType.Vec4 => Unsafe.ReadUnaligned<Vec4>(data),
                    NativeByteSpanType.Quat => Unsafe.ReadUnaligned<Quat>(data),
                    NativeByteSpanType.Color => Unsafe.ReadUnaligned<Color>(data),
                    NativeByteSpanType.Transform => Unsafe.ReadUnaligned<Transform>(data),
                    _ => throw new PlasmaException("The engine returned a value of a type a Variant cannot hold."),
                };
            }
            default:
                throw new PlasmaException($"Native value kind '{value.Kind}' is unsupported.");
        }
    }

    /// <summary>Writes this value as a call argument. Strings use <paramref name="scratch"/> (stack memory of the
    /// calling method) or a pinned array kept in <paramref name="keepAlive"/>; vectors are copied inline.</summary>
    internal unsafe NativeValue ToNative(scoped Span<byte> scratch, ref List<byte[]>? keepAlive)
    {
        switch (Kind)
        {
            case VariantKind.Null:
                return default;
            case VariantKind.Bool:
                return new NativeValue { Kind = NativeValueKind.Boolean, Payload0 = _a };
            case VariantKind.Int:
                return new NativeValue { Kind = NativeValueKind.Int64, Payload0 = _a };
            case VariantKind.UInt:
                return new NativeValue { Kind = NativeValueKind.UInt64, Payload0 = _a };
            case VariantKind.Double:
                return new NativeValue { Kind = NativeValueKind.Double, Payload0 = _a };
            case VariantKind.Object:
                return new NativeValue { Kind = NativeValueKind.ObjectHandle, Flags = (NativeValueFlags)((uint)_objectKind << 8), Payload0 = _a, Payload1 = _b };
            case VariantKind.String:
            {
                string text = (string)_reference!;
                int length = System.Text.Encoding.UTF8.GetByteCount(text);
                Span<byte> bytes = scratch;
                if (length > scratch.Length)
                {
                    byte[] pinned = GC.AllocateUninitializedArray<byte>(length, pinned: true);
                    (keepAlive ??= []).Add(pinned);
                    bytes = pinned;
                }

                int written = System.Text.Encoding.UTF8.GetBytes(text, bytes);
                return new NativeValue
                {
                    Kind = NativeValueKind.Utf8String,
                    Payload0 = (ulong)Unsafe.AsPointer(ref System.Runtime.InteropServices.MemoryMarshal.GetReference(bytes)),
                    Payload1 = (ulong)written,
                };
            }
            case VariantKind.Transform:
            {
                byte[] pinned = GC.AllocateUninitializedArray<byte>(sizeof(Transform), pinned: true);
                Unsafe.WriteUnaligned(ref pinned[0], (Transform)_reference!);
                (keepAlive ??= []).Add(pinned);
                return new NativeValue
                {
                    Kind = NativeValueKind.ByteSpan,
                    Flags = (NativeValueFlags)((uint)NativeByteSpanType.Transform << 16),
                    Payload0 = (ulong)Unsafe.AsPointer(ref pinned[0]),
                    Payload1 = (ulong)sizeof(Transform),
                };
            }
            default:
            {
                // Vectors, colours, times and angles: at most 16 bytes, passed inline.
                NativeByteSpanType tag = ByteSpanTypeOf(Kind);
                return new NativeValue
                {
                    Kind = NativeValueKind.ByteSpan,
                    Flags = (NativeValueFlags)(((uint)tag << 16) | (uint)NativeValueFlags.InlineBytes),
                    Payload0 = _a,
                    Payload1 = _b,
                };
            }
        }
    }

    private static NativeByteSpanType ByteSpanTypeOf(VariantKind kind) => kind switch
    {
        VariantKind.Time => NativeByteSpanType.Time,
        VariantKind.Angle => NativeByteSpanType.Angle,
        VariantKind.Vec2 => NativeByteSpanType.Vec2,
        VariantKind.Vec2U32 => NativeByteSpanType.Vec2U32,
        VariantKind.Vec3 => NativeByteSpanType.Vec3,
        VariantKind.Vec4 => NativeByteSpanType.Vec4,
        VariantKind.Quat => NativeByteSpanType.Quat,
        VariantKind.Color => NativeByteSpanType.Color,
        _ => NativeByteSpanType.None,
    };

    private static unsafe Variant FromBytes<T>(VariantKind kind, T value) where T : unmanaged
    {
        // Every inline kind is at most 16 bytes, the size of the two payload words.
        ulong* words = stackalloc ulong[2];
        words[0] = 0;
        words[1] = 0;
        Unsafe.WriteUnaligned(words, value);
        return new Variant(kind, words[0], words[1]);
    }

    private unsafe T ToBytes<T>() where T : unmanaged
    {
        ulong* words = stackalloc ulong[2];
        words[0] = _a;
        words[1] = _b;
        return Unsafe.ReadUnaligned<T>(words);
    }

    private T Bytes<T>(VariantKind expected) where T : unmanaged =>
        Kind == expected ? ToBytes<T>() : throw WrongKind(typeof(T));

    private InvalidCastException WrongKind(Type requested) =>
        new($"This Variant holds {Kind}, which cannot be read as {requested.Name}.");
}

/// <summary>Messages cross the engine boundary as JSON: a Variant reads and writes as the plain value it holds.</summary>
internal sealed class VariantJsonConverter : System.Text.Json.Serialization.JsonConverter<Variant>
{
    public override Variant Read(ref System.Text.Json.Utf8JsonReader reader, Type typeToConvert, System.Text.Json.JsonSerializerOptions options)
    {
        switch (reader.TokenType)
        {
            case System.Text.Json.JsonTokenType.True:
                return true;
            case System.Text.Json.JsonTokenType.False:
                return false;
            case System.Text.Json.JsonTokenType.String:
                return reader.GetString();
            case System.Text.Json.JsonTokenType.Number:
                if (reader.TryGetInt64(out long integer))
                    return integer;
                if (reader.TryGetUInt64(out ulong unsigned))
                    return unsigned;
                return reader.GetDouble();
            case System.Text.Json.JsonTokenType.StartObject:
            {
                // Vectors and colours arrive as objects of named components.
                using var document = System.Text.Json.JsonDocument.ParseValue(ref reader);
                System.Text.Json.JsonElement element = document.RootElement;
                float Component(string name) => element.TryGetProperty(name, out var c) ? c.GetSingle() : 0f;
                if (element.TryGetProperty("R", out _))
                    return new Color(Component("R"), Component("G"), Component("B"), element.TryGetProperty("A", out _) ? Component("A") : 1f);
                if (element.TryGetProperty("W", out _))
                    return new Vec4(Component("X"), Component("Y"), Component("Z"), Component("W"));
                if (element.TryGetProperty("Z", out _))
                    return new Vec3(Component("X"), Component("Y"), Component("Z"));
                if (element.TryGetProperty("Y", out _))
                    return new Vec2(Component("X"), Component("Y"));
                if (element.TryGetProperty("Seconds", out var seconds))
                    return Time.FromSeconds(seconds.GetDouble());
                return default;
            }
            default:
                reader.Skip();
                return default;
        }
    }

    public override void Write(System.Text.Json.Utf8JsonWriter writer, Variant value, System.Text.Json.JsonSerializerOptions options)
    {
        switch (value.Kind)
        {
            case VariantKind.Null:
            case VariantKind.Object:
                writer.WriteNullValue();
                break;
            case VariantKind.Bool:
                writer.WriteBooleanValue(value.AsBool());
                break;
            case VariantKind.Int:
                writer.WriteNumberValue(value.AsInt64());
                break;
            case VariantKind.UInt:
                writer.WriteNumberValue(value.AsUInt64());
                break;
            case VariantKind.Double:
                writer.WriteNumberValue(value.AsDouble());
                break;
            case VariantKind.String:
                writer.WriteStringValue(value.ToString());
                break;
            default:
                System.Text.Json.JsonSerializer.Serialize(writer, value.ToObject(), value.ToObject()!.GetType(), options);
                break;
        }
    }
}
