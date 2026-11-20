using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Plasma.ManagedHost;
using Xunit.Abstractions;

namespace Plasma.ManagedTests;

/// <summary>The managed half of an engine call, against a native stub: value conversion and its cost.</summary>
public sealed unsafe class EngineCallTests(ITestOutputHelper output)
{
    private const ulong FloatCall = 1;
    private const ulong VectorResultCall = 2;
    private const ulong RecordArgumentCall = 3;
    private const ulong InlineVectorResultCall = 4;
    private const ulong EchoArrayCall = 5;

    private static CSharpValue s_lastArgument;
    private static readonly Vec3* s_vector = AllocateVector();

    private static Vec3* AllocateVector()
    {
        var vector = (Vec3*)NativeMemory.Alloc((nuint)sizeof(Vec3));
        *vector = new Vec3(1, 2, 3);
        return vector;
    }

    [Fact]
    public void AnUntypedSlotCarriesVectorsBothWays()
    {
        WithStub(() =>
        {
            object? result = ReflectedCall.Invoke<object?>(VectorResultCall, "Test.GetVariant", default, [], []);
            Assert.Equal(new Vec3(1, 2, 3), result);

            object?[] arguments = [new Vec3(4, 5, 6)];
            ReflectedCall.Invoke(RecordArgumentCall, "Test.SetVariant", default, arguments, [typeof(object)]);
            Assert.Equal(CSharpValueKind.ByteSpan, s_lastArgument.Kind);
            Assert.Equal((uint)NativeByteSpanType.Vec3, ((uint)s_lastArgument.Flags >> 16) & 0xFFu);
            Assert.Equal(new Vec3(4, 5, 6), arguments[0]);
        });
    }

    [Fact]
    public void VariantConvertsTheWayExistingScriptsUseIt()
    {
        // Patterns from GameSDK and BrickBreaker: implicit arguments, Convert.ToX and casts on results.
        Variant fromInt = 5;
        Variant fromUInt = 7u;
        Variant fromBool = true;
        Variant fromString = "Combat";
        Variant fromVector = new Vec3(1, 2, 3);

        Assert.Equal(5, (int)fromInt);
        Assert.Equal(5f, Convert.ToSingle(fromInt));
        Assert.Equal(7u, Convert.ToUInt32((object)fromUInt));
        Assert.True(Convert.ToBoolean(fromBool));
        Assert.Equal("Combat", fromString.ToString());
        Assert.Equal(new Vec3(1, 2, 3), (Vec3)fromVector);
        Assert.Throws<InvalidCastException>(() => (Vec3)fromInt);
    }

    private enum Phase { Idle, Combat = 3 }

    [Fact]
    public void VariantConvertsGenericallyForTypedKeys()
    {
        Assert.Equal(3.5f, Variant.From(3.5f).As<float>());
        Assert.Equal(7u, Variant.From(7u).As<uint>());
        Assert.True(Variant.From(true).As<bool>());
        Assert.Equal(new Vec3(1, 2, 3), Variant.From(new Vec3(1, 2, 3)).As<Vec3>());
        Assert.Equal("text", Variant.From("text").As<string>());
        Assert.Equal(Phase.Combat, Variant.From(3).As<Phase>());
        Assert.Equal(42, ((Variant)42L).As<int>());

        var key = new BlackboardKey<float>("TargetDistance");
        Assert.Equal("TargetDistance", key.ToString());
    }

    [Fact]
    public void AVariantCrossesTheBoundaryWithoutBoxing()
    {
        WithStub(() =>
        {
            Span<NativeValue> __plArgs = stackalloc NativeValue[1];
            using (EngineCall __plCall = new(RecordArgumentCall, "Test.SetVariant", default, __plArgs))
            {
                __plCall.Any(0, new Vec3(4, 5, 6), stackalloc byte[EngineCall.StringScratchBytes]);
                __plCall.Invoke();
            }

            Assert.Equal(CSharpValueKind.ByteSpan, s_lastArgument.Kind);
            Assert.Equal((uint)NativeByteSpanType.Vec3, ((uint)s_lastArgument.Flags >> 16) & 0xFFu);
            Assert.NotEqual(0u, (uint)s_lastArgument.Flags & (uint)NativeValueFlags.InlineBytes);

            using (EngineCall __plCall = new(VectorResultCall, "Test.GetVariant", default, default))
            {
                __plCall.Invoke();
                Assert.Equal(new Vec3(1, 2, 3), (Vec3)__plCall.ResultAny(-1));
            }

            using (EngineCall __plCall = new(FloatCall, "Test.GetNumber", default, default))
            {
                __plCall.Invoke();
                Assert.Equal(0.5f, (float)__plCall.ResultAny(-1));
            }
        });
    }

    [Fact]
    public void ListsCrossTheBoundary()
    {
        WithStub(() =>
        {
            Span<NativeValue> __plArgs = stackalloc NativeValue[1];
            using EngineCall __plCall = new(EchoArrayCall, "Test.Echo", default, __plArgs);
            __plCall.AnyArray(0, [1, "two", new Vec3(3, 3, 3), Variant.Null, new string('x', 600)]);
            __plCall.Invoke();

            Variant[] list = __plCall.ResultAnyArray(-1);
            Assert.Equal(5, list.Length);
            Assert.Equal(1, (int)list[0]);
            Assert.Equal("two", list[1].ToString());
            Assert.Equal(new Vec3(3, 3, 3), (Vec3)list[2]);
            Assert.True(list[3].IsNull);
            Assert.Equal(600, list[4].ToString().Length);
        });
    }

    [Fact]
    public void SmallValuesComeBackInlineOnBothPaths()
    {
        WithStub(() =>
        {
            Assert.Equal(new Vec3(7, 8, 9), ReflectedCall.Invoke<Vec3>(InlineVectorResultCall, "Test.GetPosition", default, [], []));
            Assert.Equal(new Vec3(7, 8, 9), ReflectedCall.Invoke<object?>(InlineVectorResultCall, "Test.GetVariant", default, [], []));

            Span<NativeValue> __plArgs = default;
            using EngineCall __plCall = new(InlineVectorResultCall, "Test.GetPosition", default, __plArgs);
            __plCall.Invoke();
            Assert.Equal(new Vec3(7, 8, 9), __plCall.ResultBytes<Vec3>(-1));
        });
    }

    // The generated typed call allocates nothing for numbers, vectors and short strings.
    // The reflected path it replaced is measured alongside for comparison.
    [Fact]
    public void TypedEngineCallsDoNotAllocate()
    {
        WithStub(() =>
        {
            (long typedBytes, double typedMicroseconds) = Measure(TypedCall);
            (long reflectedBytes, double reflectedMicroseconds) = Measure(ReflectedCallOfTheSameShape);
            (long vectorBytes, _) = Measure(TypedVectorCall);

            output.WriteLine($"Engine call (string, bool) -> float, managed side: typed {typedBytes} bytes, {typedMicroseconds:F3} us; reflected {reflectedBytes} bytes, {reflectedMicroseconds:F3} us.");
            Assert.Equal(0, typedBytes);
            Assert.Equal(0, vectorBytes);
            Assert.True(reflectedBytes > 0);
        });
    }

    private static (long BytesPerCall, double MicrosecondsPerCall) Measure(Func<float> call)
    {
        const int Calls = 10_000;
        for (int i = 0; i < 100; ++i)
            call();

        long before = GC.GetAllocatedBytesForCurrentThread();
        var watch = System.Diagnostics.Stopwatch.StartNew();
        for (int i = 0; i < Calls; ++i)
            call();
        watch.Stop();
        return ((GC.GetAllocatedBytesForCurrentThread() - before) / Calls, watch.Elapsed.TotalMilliseconds * 1000.0 / Calls);
    }

    // The shape the binding generator emits for InputComponent.GetCurrentInputState.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static float TypedCall()
    {
        Span<NativeValue> __plArgs = stackalloc NativeValue[2];
        using EngineCall __plCall = new(FloatCall, "InputComponent.GetCurrentInputState", default, __plArgs);
        __plCall.String(0, "Left", stackalloc byte[EngineCall.StringScratchBytes]);
        __plCall.Bool(1, false);
        __plCall.Invoke();
        return (float)__plCall.ResultDouble(-1);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static float TypedVectorCall()
    {
        var position = new Vec3(1, 2, 3);
        Span<NativeValue> __plArgs = stackalloc NativeValue[1];
        using EngineCall __plCall = new(FloatCall, "GameObject.SetLocalPosition", default, __plArgs);
        Vec3 __plValue0 = position;
        __plCall.Bytes(0, in __plValue0, NativeByteSpanType.Vec3);
        __plCall.Invoke();
        return 0;
    }

    // The shape the generator emitted before typed calls.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static float ReflectedCallOfTheSameShape()
    {
        object?[] arguments = new object?[] { "Left", false };
        Type[] argumentTypes = new Type[] { typeof(string), typeof(bool) };
        return ReflectedCall.Invoke<float>(FloatCall, "InputComponent.GetCurrentInputState", default, arguments, argumentTypes);
    }

    private static void WithStub(Action test)
    {
        var nativeApi = new NativeApiV1
        {
            Size = (uint)sizeof(NativeApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
            InvokeReflected = &StubInvokeReflected,
            ReleaseNativeValue = &StubReleaseValue,
        };

        NativeBridge.Configure((nint)(&nativeApi));
        try
        {
            test();
        }
        finally
        {
            NativeBridge.Reset();
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus StubInvokeReflected(
        ulong functionId, CSharpObjectHandle target, CSharpValue* arguments, uint argumentCount, CSharpValue* result)
    {
        _ = target;
        *result = default;

        switch (functionId)
        {
            case FloatCall:
                result->Kind = CSharpValueKind.Double;
                result->Payload0 = unchecked((ulong)BitConverter.DoubleToInt64Bits(0.5));
                return CSharpStatus.Success;

            case VectorResultCall:
                result->Kind = CSharpValueKind.ByteSpan;
                result->Flags = (CSharpValueFlags)((uint)NativeByteSpanType.Vec3 << 16);
                result->Payload0 = (ulong)s_vector;
                result->Payload1 = (ulong)sizeof(Vec3);
                return CSharpStatus.Success;

            case InlineVectorResultCall:
                result->Kind = CSharpValueKind.ByteSpan;
                result->Flags = (CSharpValueFlags)(((uint)NativeByteSpanType.Vec3 << 16) | (uint)NativeValueFlags.InlineBytes);
                *(Vec3*)&result->Payload0 = new Vec3(7, 8, 9);
                return CSharpStatus.Success;

            case EchoArrayCall:
                // Returns the list it was given, as the engine returns a variant array (ManagedOwned only on
                // native allocations, so this borrowed echo is not released).
                *result = arguments[0];
                return CSharpStatus.Success;

            case RecordArgumentCall:
                s_lastArgument = argumentCount > 0 ? arguments[0] : default;
                return CSharpStatus.Success;

            default:
                return CSharpStatus.MemberNotFound;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus StubReleaseValue(CSharpValue* value)
    {
        *value = default;
        return CSharpStatus.Success;
    }
}
