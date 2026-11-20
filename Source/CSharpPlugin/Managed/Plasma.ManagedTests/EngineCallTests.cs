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

    // P9 baseline: bytes one generated-style call allocates. R1 drives this to zero; until then it is reported.
    [Fact]
    public void ReportsTheManagedCostOfAnEngineCall()
    {
        WithStub(() =>
        {
            const int Calls = 10_000;
            for (int i = 0; i < 100; ++i)
                CallLikeGeneratedCode();

            long before = GC.GetAllocatedBytesForCurrentThread();
            var watch = System.Diagnostics.Stopwatch.StartNew();
            for (int i = 0; i < Calls; ++i)
                CallLikeGeneratedCode();
            watch.Stop();
            long bytesPerCall = (GC.GetAllocatedBytesForCurrentThread() - before) / Calls;

            output.WriteLine($"Engine call (string, bool) -> float: {bytesPerCall} bytes allocated, {watch.Elapsed.TotalMilliseconds * 1000.0 / Calls:F3} us per call (managed side only).");
            Assert.True(bytesPerCall > 0, "Update this test into a zero-allocation assertion once typed generated calls land.");
        });
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static float CallLikeGeneratedCode()
    {
        // The exact shape the binding generator emits today for InputComponent.GetCurrentInputState.
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
