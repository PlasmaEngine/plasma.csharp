using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Plasma;

/// <summary>One engine call with statically typed arguments, as emitted by the binding generator.</summary>
/// <remarks>
/// Argument values live in stack memory owned by the generated method: the constructor takes a
/// <c>stackalloc</c> span, strings are encoded into a stack buffer and value types are passed by the
/// address of the method's own parameter. None of it may outlive that method, which is why this type
/// is a ref struct and is only meant for generated code. A call that passes numbers, vectors and
/// short strings allocates nothing on the managed heap.
/// </remarks>
[EditorBrowsable(EditorBrowsableState.Never)]
public unsafe ref struct EngineCall
{
    /// <summary>Stack bytes generated code reserves per string argument; longer strings use a pinned array.</summary>
    public const int StringScratchBytes = 256;

    private const int ByteSpanTypeShift = 16;

    private readonly ulong _functionId;
    private readonly string _apiName;
    private readonly NativeObject _target;
    private readonly NativeValue* _arguments;
    private readonly int _argumentCount;
    private NativeValue _result;
    private List<IDisposable>? _variantLeases;
    private List<byte[]>? _keepAlive;
    private List<NativeValue[]>? _pinnedValues;

    public EngineCall(ulong functionId, string apiName, NativeObject target, scoped Span<NativeValue> arguments)
    {
        _functionId = functionId;
        _apiName = apiName;
        _target = target;
        arguments.Clear();
        _arguments = (NativeValue*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(arguments));
        _argumentCount = arguments.Length;
        _result = default;
        _variantLeases = null;
        _keepAlive = null;
        _pinnedValues = null;
    }

    // ---------------------------------------------------------------- arguments

    public void Bool(int index, bool value) =>
        _arguments[index] = new NativeValue { Kind = NativeValueKind.Boolean, Payload0 = value ? 1UL : 0UL };

    public void Int64(int index, long value) =>
        _arguments[index] = new NativeValue { Kind = NativeValueKind.Int64, Payload0 = unchecked((ulong)value) };

    public void UInt64(int index, ulong value) =>
        _arguments[index] = new NativeValue { Kind = NativeValueKind.UInt64, Payload0 = value };

    public void Double(int index, double value) =>
        _arguments[index] = new NativeValue { Kind = NativeValueKind.Double, Payload0 = unchecked((ulong)BitConverter.DoubleToInt64Bits(value)) };

    /// <summary>A string argument, encoded into <c>scratch</c>: stack memory of the generated method,
    /// <see cref="StringScratchBytes"/> long.</summary>
    public void String(int index, string? value, scoped Span<byte> scratch)
    {
        if (value is null)
        {
            _arguments[index] = default;
            return;
        }

        Span<byte> bytes = scratch;
        if (Encoding.UTF8.GetMaxByteCount(value.Length) > scratch.Length)
        {
            int length = Encoding.UTF8.GetByteCount(value);
            if (length > scratch.Length)
            {
                // Pinned for its whole life; kept referenced until the call is disposed.
                byte[] pinned = GC.AllocateUninitializedArray<byte>(length, pinned: true);
                (_keepAlive ??= []).Add(pinned);
                bytes = pinned;
            }
        }

        int written = Encoding.UTF8.GetBytes(value, bytes);
        _arguments[index] = new NativeValue
        {
            Kind = NativeValueKind.Utf8String,
            Payload0 = (ulong)Unsafe.AsPointer(ref MemoryMarshal.GetReference(bytes)),
            Payload1 = (ulong)written,
        };
    }

    public void Object(int index, NativeObject value) =>
        _arguments[index] = value.IsValid
            ? new NativeValue
            {
                Kind = NativeValueKind.ObjectHandle,
                Flags = (NativeValueFlags)((uint)value.Kind << 8),
                Payload0 = value.Id,
                Payload1 = value.Generation,
            }
            : default;

    /// <summary>A value-type argument. <c>value</c> must be a parameter or local of the generated method:
    /// its address is passed to the engine.</summary>
    public void Bytes<T>(int index, in T value, NativeByteSpanType type) where T : unmanaged =>
        _arguments[index] = new NativeValue
        {
            Kind = NativeValueKind.ByteSpan,
            Flags = (NativeValueFlags)((uint)type << ByteSpanTypeShift),
            Payload0 = (ulong)Unsafe.AsPointer(ref Unsafe.AsRef(in value)),
            Payload1 = (ulong)sizeof(T),
        };

    /// <summary>An "any value" argument. A string is encoded into <c>scratch</c>, stack memory of the
    /// generated method; numbers, vectors and object references need none.</summary>
    public void Any(int index, global::Plasma.Variant value, scoped Span<byte> scratch) =>
        _arguments[index] = value.ToNative(scratch, ref _keepAlive);

    /// <summary>A list argument (an engine variant array). The element values live in a pinned array kept
    /// until the call is disposed; this allocates, unlike single values.</summary>
    public void AnyArray(int index, global::Plasma.Variant[]? values)
    {
        if (values is null)
        {
            _arguments[index] = default;
            return;
        }

        NativeValue[] elements = GC.AllocateArray<NativeValue>(values.Length, pinned: true);
        for (int i = 0; i < values.Length; ++i)
            elements[i] = values[i].ToNative(default, ref _keepAlive);

        (_pinnedValues ??= []).Add(elements);
        _arguments[index] = new NativeValue
        {
            Kind = NativeValueKind.ValueArray,
            Payload0 = values.Length == 0 ? 0 : (ulong)Unsafe.AsPointer(ref elements[0]),
            Payload1 = (ulong)values.Length,
        };
    }

    /// <summary>An untyped argument of a type known only at run time.</summary>
    public void Variant(int index, object? value)
    {
        _variantLeases ??= [];
        _arguments[index] = ReflectedCall.ToNativeValue(value, typeof(object), _variantLeases);
    }

    /// <summary>An out argument: nothing is passed in, the engine writes the value.</summary>
    public void Out(int index) => _arguments[index] = default;

    // ---------------------------------------------------------------- call

    public void Invoke()
    {
        NativeCallStatus status = NativeBridge.InvokeReflected(
            _functionId, _target, new ReadOnlySpan<NativeValue>(_arguments, _argumentCount), out _result);

        if (status != NativeCallStatus.Success)
            throw new PlasmaException(ReflectedCall.DescribeFailure(_apiName, _functionId, status), (int)status);
    }

    // ---------------------------------------------------------------- results (index -1) and out arguments

    public readonly bool ResultBool(int index = -1) => ToUInt64(Value(index)) != 0;
    public readonly long ResultInt64(int index = -1) => unchecked((long)ToUInt64(Value(index)));
    public readonly ulong ResultUInt64(int index = -1) => ToUInt64(Value(index));

    public readonly double ResultDouble(int index = -1)
    {
        NativeValue value = Value(index);
        return value.Kind switch
        {
            NativeValueKind.Double => BitConverter.Int64BitsToDouble(unchecked((long)value.Payload0)),
            NativeValueKind.Int64 => unchecked((long)value.Payload0),
            NativeValueKind.UInt64 => value.Payload0,
            NativeValueKind.Boolean => value.Payload0,
            _ => 0.0,
        };
    }

    public readonly string ResultString(int index = -1)
    {
        NativeValue value = Value(index);
        return value.Kind == NativeValueKind.Utf8String && value.Payload0 != 0
            ? Marshal.PtrToStringUTF8((nint)value.Payload0, checked((int)value.Payload1)) ?? string.Empty
            : string.Empty;
    }

    /// <summary>An object result; <c>kind</c> is used when the engine did not report one.</summary>
    public readonly NativeObject ResultObject(NativeObjectKind kind, int index = -1)
    {
        NativeValue value = Value(index);
        if (value.Kind != NativeValueKind.ObjectHandle)
            return default;

        uint reported = ((uint)value.Flags >> 8) & 0xFFu;
        if (reported is >= (uint)NativeObjectKind.World and <= (uint)NativeObjectKind.ReflectedObject)
            kind = (NativeObjectKind)reported;

        return new NativeObject(value.Payload0, value.Payload1, kind);
    }

    public readonly T ResultBytes<T>(int index = -1) where T : unmanaged
    {
        NativeValue value = Value(index);
        if (value.Kind == NativeValueKind.Null)
            return default;

        if (value.Kind == NativeValueKind.ByteSpan && (value.Flags & NativeValueFlags.InlineBytes) != 0 && sizeof(T) <= 16)
            return Unsafe.ReadUnaligned<T>(&value.Payload0);

        if (value.Kind != NativeValueKind.ByteSpan || value.Payload0 == 0 || value.Payload1 != (ulong)sizeof(T))
            throw new PlasmaException($"Engine API '{_apiName}' returned a value that is not a '{typeof(T).Name}'.");

        return Unsafe.ReadUnaligned<T>((void*)value.Payload0);
    }

    public readonly object? ResultVariant(int index = -1) => ReflectedCall.FromNativeValue(Value(index), typeof(object));

    public readonly global::Plasma.Variant ResultAny(int index = -1) => global::Plasma.Variant.FromNative(Value(index));

    /// <summary>A list result, copied out of native memory; the native copy is freed when the call is disposed.</summary>
    public readonly global::Plasma.Variant[] ResultAnyArray(int index = -1)
    {
        NativeValue value = Value(index);
        if (value.Kind != NativeValueKind.ValueArray || value.Payload1 == 0 || value.Payload0 == 0)
            return [];

        var elements = new ReadOnlySpan<NativeValue>((void*)value.Payload0, checked((int)value.Payload1));
        var result = new global::Plasma.Variant[elements.Length];
        for (int i = 0; i < elements.Length; ++i)
            result[i] = global::Plasma.Variant.FromNative(elements[i]);

        return result;
    }

    // ---------------------------------------------------------------- cleanup

    public void Dispose()
    {
        if ((_result.Flags & NativeValueFlags.ManagedOwned) != 0)
            _ = NativeBridge.ReleaseValue(ref _result);

        for (int i = 0; i < _argumentCount; ++i)
        {
            if ((_arguments[i].Flags & NativeValueFlags.ManagedOwned) != 0)
                _ = NativeBridge.ReleaseValue(ref _arguments[i]);
        }

        if (_variantLeases is not null)
        {
            foreach (IDisposable lease in _variantLeases)
                lease.Dispose();
        }

        GC.KeepAlive(_keepAlive);
        GC.KeepAlive(_pinnedValues);
    }

    private readonly NativeValue Value(int index) => index < 0 ? _result : _arguments[index];

    private static ulong ToUInt64(in NativeValue value) => value.Kind switch
    {
        NativeValueKind.Double => unchecked((ulong)(long)BitConverter.Int64BitsToDouble(unchecked((long)value.Payload0))),
        NativeValueKind.Null => 0,
        _ => value.Payload0,
    };
}
