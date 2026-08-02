using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Plasma.ManagedHost;
using Plasma.ScriptTestGame;

namespace Plasma.ManagedTests;

public sealed unsafe class ManagedHostAbiTests
{
    private const string ScriptName = "Plasma.ScriptTestGame.RotatingBeacon";
    private const int AbiGuardSize = 32;
    private const byte AbiGuardValue = 0xD7;
    private static CSharpLogLevel s_capturedLogLevel;
    private static string s_capturedLogMessage = string.Empty;

    [Fact]
    public void GameplayLogFacadeForwardsEveryNativeLogLevel()
    {
        var nativeApi = new NativeApiV1
        {
            Size = (uint)sizeof(NativeApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
            Log = &CapturingNativeLog,
        };

        NativeBridge.Configure((nint)(&nativeApi));
        try
        {
            Log.Debug("debug");
            Assert.Equal(CSharpLogLevel.Debug, s_capturedLogLevel);
            Assert.Equal("debug", s_capturedLogMessage);

            Log.Dev("dev");
            Assert.Equal(CSharpLogLevel.Dev, s_capturedLogLevel);
            Assert.Equal("dev", s_capturedLogMessage);

            Log.Info("value {0}", 42);
            Assert.Equal(CSharpLogLevel.Info, s_capturedLogLevel);
            Assert.Equal("value 42", s_capturedLogMessage);

            Log.Success("success");
            Assert.Equal(CSharpLogLevel.Success, s_capturedLogLevel);
            Assert.Equal("success", s_capturedLogMessage);

            Log.Warning("warning");
            Assert.Equal(CSharpLogLevel.Warning, s_capturedLogLevel);
            Assert.Equal("warning", s_capturedLogMessage);

            Log.SeriousWarning("serious");
            Assert.Equal(CSharpLogLevel.SeriousWarning, s_capturedLogLevel);
            Assert.Equal("serious", s_capturedLogMessage);

            var exception = new InvalidOperationException("failure");
            Log.Error("error", exception);
            Assert.Equal(CSharpLogLevel.Error, s_capturedLogLevel);
            Assert.Contains("error", s_capturedLogMessage, StringComparison.Ordinal);
            Assert.Contains("failure", s_capturedLogMessage, StringComparison.Ordinal);
        }
        finally
        {
            NativeBridge.Reset();
        }
    }

    [Fact]
    public void ReflectedCallFailureNamesTheEngineApiAndSuggestsRecovery()
    {
        var nativeApi = new NativeApiV1
        {
            Size = (uint)sizeof(NativeApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
            InvokeReflected = &NativeInvokeMissingReflected,
            ReleaseNativeValue = &NativeReleaseValue,
        };

        NativeBridge.Configure((nint)(&nativeApi));
        try
        {
            PlasmaException exception = Assert.Throws<PlasmaException>(() =>
                ReflectedCall.Invoke(
                    0xEA3AD9B5F001CA3FUL,
                    "HeadBoneComponent.ChangeVerticalRotation",
                    default,
                    Array.Empty<object?>(),
                    Type.EmptyTypes));

            Assert.Contains(
                "Engine API 'HeadBoneComponent.ChangeVerticalRotation' failed",
                exception.Message,
                StringComparison.Ordinal);
            Assert.Contains("Rebuild the C# scripts", exception.Message, StringComparison.Ordinal);
            Assert.Contains("MemberNotFound", exception.Message, StringComparison.Ordinal);
            Assert.Contains("0xEA3AD9B5F001CA3F", exception.Message, StringComparison.Ordinal);
        }
        finally
        {
            NativeBridge.Reset();
        }
    }

    [Fact]
    public void BootstrapAcceptsBaseSizedTablesWithoutOptionalM0Tail()
    {
        int nativeBaseSize = checked(
            (int)Marshal.OffsetOf<NativeApiV1>(nameof(NativeApiV1.M0Probe)));
        int managedBaseSize = checked(
            (int)Marshal.OffsetOf<ManagedApiV1>(nameof(ManagedApiV1.RunM0Probe)));
        byte* nativeStorage = (byte*)NativeMemory.Alloc(
            checked((nuint)(nativeBaseSize + AbiGuardSize)));
        byte* managedStorage = (byte*)NativeMemory.Alloc(
            checked((nuint)(managedBaseSize + AbiGuardSize)));
        bool initialized = false;
        bool shutdown = false;

        try
        {
            new Span<byte>(nativeStorage, nativeBaseSize + AbiGuardSize).Clear();
            new Span<byte>(managedStorage, managedBaseSize + AbiGuardSize).Clear();
            new Span<byte>(nativeStorage + nativeBaseSize, AbiGuardSize).Fill(AbiGuardValue);
            new Span<byte>(managedStorage + managedBaseSize, AbiGuardSize).Fill(AbiGuardValue);

            var nativeApi = (NativeApiV1*)nativeStorage;
            nativeApi->Size = checked((uint)nativeBaseSize);
            nativeApi->Version = 1;
            nativeApi->PointerSize = (uint)sizeof(void*);
            nativeApi->ValueLayoutVersion = 2;
            nativeApi->Log = &NativeLog;
            nativeApi->ValidateObject = &NativeValidateObject;
            nativeApi->InvokeReflected = &NativeInvokeReflected;
            nativeApi->ReleaseNativeValue = &NativeReleaseValue;
            nativeApi->QueryExtension = &NativeQueryExtension;

            var managedApi = (ManagedApiV1*)managedStorage;
            managedApi->Size = checked((uint)managedBaseSize);

            delegate* unmanaged[Cdecl]<NativeApiV1*, ManagedApiV1*, CSharpStatus> initialize =
                &Bootstrap.Initialize;
            Assert.Equal(CSharpStatus.Success, initialize(nativeApi, managedApi));
            initialized = true;

            Assert.Equal((uint)managedBaseSize, managedApi->Size);
            Assert.Equal(1U, managedApi->Version);
            Assert.Equal((uint)sizeof(void*), managedApi->PointerSize);
            Assert.Equal(2U, managedApi->ValueLayoutVersion);
            Assert.True(managedApi->Shutdown != null);
            Assert.True(managedApi->LoadGeneration != null);
            Assert.True(managedApi->GetGenerationDescriptor != null);
            Assert.True(managedApi->CreateInstance != null);
            Assert.True(managedApi->DestroyInstance != null);
            Assert.True(managedApi->InvokeMethod != null);
            Assert.True(managedApi->GetField != null);
            Assert.True(managedApi->SetField != null);
            Assert.True(managedApi->DispatchMessage != null);
            Assert.True(managedApi->UnloadGeneration != null);
            Assert.True(managedApi->GetLastError != null);
            Assert.True(managedApi->FreeBuffer != null);
            Assert.True(managedApi->ReleaseValue != null);
            Assert.True(managedApi->QueryExtension != null);
            Assert.Equal(
                NativeCallStatus.Success,
                NativeBridge.ValidateObject(
                    new NativeObject(0x1001, 1, NativeObjectKind.GameObject),
                    NativeObjectKind.GameObject));

            AssertGuardIntact(nativeStorage + nativeBaseSize);
            AssertGuardIntact(managedStorage + managedBaseSize);

            Assert.Equal(CSharpStatus.Success, managedApi->Shutdown());
            shutdown = true;
        }
        finally
        {
            if (initialized && !shutdown)
            {
                var managedApi = (ManagedApiV1*)managedStorage;
                if (managedApi->Shutdown != null)
                {
                    _ = managedApi->Shutdown();
                }
            }

            NativeMemory.Free(nativeStorage);
            NativeMemory.Free(managedStorage);
        }
    }

    [Fact]
    public void BootstrapLoadsInvokesAndUnloadsCollectibleGeneration()
    {
        var nativeApi = new NativeApiV1
        {
            Size = (uint)sizeof(NativeApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
            Log = &NativeLog,
            ValidateObject = &NativeValidateObject,
            InvokeReflected = &NativeInvokeReflected,
            ReleaseNativeValue = &NativeReleaseValue,
            QueryExtension = &NativeQueryExtension,
            M0Probe = &NativeM0Probe,
        };
        var managedApi = new ManagedApiV1
        {
            Size = (uint)sizeof(ManagedApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
        };

        delegate* unmanaged[Cdecl]<NativeApiV1*, ManagedApiV1*, CSharpStatus> initialize =
            &Bootstrap.Initialize;
        Assert.Equal(CSharpStatus.Success, initialize(&nativeApi, &managedApi));

        try
        {
            RunGenerationRoundTrip(managedApi);
            CreateLiveInstanceForShutdown(managedApi);
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, managedApi.Shutdown());
        }
    }

    private static void CreateLiveInstanceForShutdown(ManagedApiV1 api)
    {
        string assemblyPath = typeof(RotatingBeacon).Assembly.Location;
        byte[] pathBytes = Encoding.UTF8.GetBytes(assemblyPath);
        ulong generation = 0;
        fixed (byte* path = pathBytes)
        {
            var load = new CSharpGenerationLoadDesc
            {
                Size = (uint)sizeof(CSharpGenerationLoadDesc),
                Version = 1,
                AssemblyPath = new CSharpUtf8Span
                {
                    Data = path,
                    Length = checked((uint)pathBytes.Length),
                },
            };
            Assert.Equal(CSharpStatus.Success, api.LoadGeneration(&load, &generation));
        }

        var create = new CSharpInstanceCreateDesc
        {
            Size = (uint)sizeof(CSharpInstanceCreateDesc),
            Version = 1,
            Generation = generation,
            TypeId = StableId.ScriptType(ScriptName),
        };
        ulong instance = 0;
        Assert.Equal(CSharpStatus.Success, api.CreateInstance(&create, &instance));
        Assert.NotEqual(0UL, instance);
        // Intentionally leave both alive. Shutdown must detach the instance before
        // attempting to collect its AssemblyLoadContext.
    }

    private static void RunGenerationRoundTrip(ManagedApiV1 api)
    {
        string assemblyPath = typeof(RotatingBeacon).Assembly.Location;
        AssertNestedShadowCopyIsRejected(api, assemblyPath);

        byte[] pathBytes = Encoding.UTF8.GetBytes(assemblyPath);
        ulong generation = 0;
        fixed (byte* path = pathBytes)
        {
            var load = new CSharpGenerationLoadDesc
            {
                Size = (uint)sizeof(CSharpGenerationLoadDesc),
                Version = 1,
                AssemblyPath = new CSharpUtf8Span
                {
                    Data = path,
                    Length = checked((uint)pathBytes.Length),
                },
            };
            Assert.Equal(CSharpStatus.Success, api.LoadGeneration(&load, &generation));
        }

        var create = new CSharpInstanceCreateDesc
        {
            Size = (uint)sizeof(CSharpInstanceCreateDesc),
            Version = 1,
            Generation = generation,
            TypeId = StableId.ScriptType(ScriptName),
            Owner = new CSharpObjectHandle
            {
                Value = 0x1001,
                Generation = 1,
                Kind = (uint)NativeObjectKind.GameObject,
            },
            World = new CSharpObjectHandle
            {
                Value = 0x2001,
                Generation = 1,
                Kind = (uint)NativeObjectKind.World,
            },
            OwnerComponent = new CSharpObjectHandle
            {
                Value = 0x3001,
                Generation = 1,
                Kind = (uint)NativeObjectKind.Component,
            },
        };

        ulong instance = 0;
        try
        {
            AssertGenerationManifest(api, generation);
            Assert.Equal(CSharpStatus.Success, api.CreateInstance(&create, &instance));

            CSharpValue result = default;
            Assert.Equal(
                CSharpStatus.MemberNotFound,
                api.InvokeMethod(
                    instance,
                    (ulong)ScriptLifecycleMethod.Initialize,
                    null,
                    0,
                    &result));

            var update = new CSharpValue
            {
                Kind = CSharpValueKind.Double,
                Payload0 = unchecked((ulong)BitConverter.DoubleToInt64Bits(0.25)),
            };
            Assert.Equal(
                CSharpStatus.Success,
                api.InvokeMethod(
                    instance,
                    (ulong)ScriptLifecycleMethod.Update,
                    &update,
                    1,
                    &result));

            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Duration), Time.FromSeconds(9.5));
            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Heading), Angle.FromDegrees(37.0f));
            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Offset2), new Vec2(8.0f, 9.0f));
            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Offset3), new Vec3(8.0f, 9.0f, 10.0f));
            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Offset4), new Vec4(8.0f, 9.0f, 10.0f, 11.0f));
            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Orientation), new Quat(1.0f, 2.0f, 3.0f, 4.0f));
            RoundTripBlittable(api, instance, nameof(RotatingBeacon.Tint), new Color(0.1f, 0.2f, 0.3f, 0.4f));
            RoundTripBlittable(
                api,
                instance,
                nameof(RotatingBeacon.Pose),
                new Transform(
                    new Vec3(5.0f, 6.0f, 7.0f),
                    new Quat(0.0f, 0.0f, 1.0f, 0.0f),
                    new Vec3(2.0f, 3.0f, 4.0f)));

            RoundTripObjectHandle(
                api,
                instance,
                nameof(RotatingBeacon.Target),
                new NativeObject(0x4101, 7, NativeObjectKind.ReflectedObject));
            RoundTripObjectHandle(
                api,
                instance,
                nameof(RotatingBeacon.TargetWorld),
                new NativeObject(0x4201, 8, NativeObjectKind.World));
            RoundTripObjectHandle(
                api,
                instance,
                nameof(RotatingBeacon.TargetGameObject),
                new NativeObject(0x4301, 9, NativeObjectKind.GameObject));
            RoundTripObjectHandle(
                api,
                instance,
                nameof(RotatingBeacon.TargetComponent),
                new NativeObject(0x4401, 10, NativeObjectKind.Component));

            AssertJsonMessageDispatch(api, instance);

            var throwOnUpdate = new CSharpValue
            {
                Kind = CSharpValueKind.Boolean,
                Payload0 = 1,
            };
            Assert.Equal(
                CSharpStatus.Success,
                api.SetField(
                    instance,
                    StableId.ExposedField(ScriptName, nameof(RotatingBeacon.ThrowOnUpdate)),
                    &throwOnUpdate));
            Assert.Equal(
                CSharpStatus.ManagedException,
                api.InvokeMethod(
                    instance,
                    (ulong)ScriptLifecycleMethod.Update,
                    &update,
                    1,
                    &result));
            string lifecycleError = ReadLastError(api);
            Assert.True(lifecycleError.Contains("Source:", StringComparison.Ordinal), lifecycleError);
            Assert.Contains("RotatingBeacon.cs(", lifecycleError, StringComparison.Ordinal);
            Assert.Matches(
                @"Source: [^\r\n]*RotatingBeacon\.cs\([1-9]\d*,[1-9]\d*\)\.",
                lifecycleError);
            Assert.Contains("Intentional source-location test failure", lifecycleError, StringComparison.Ordinal);

            long probeResult = 0;
            Assert.Equal(CSharpStatus.Success, api.RunM0Probe(41, &probeResult));
            Assert.Equal(42, probeResult);
        }
        finally
        {
            if (instance != 0)
            {
                Assert.Equal(CSharpStatus.Success, api.DestroyInstance(instance));
            }
        }

        var report = new CSharpUnloadReport
        {
            Size = (uint)sizeof(CSharpUnloadReport),
            Version = 1,
        };
        Assert.Equal(CSharpStatus.Success, api.UnloadGeneration(generation, &report));
        Assert.Equal(0U, report.LoadContextAlive);

        ulong staleInstance = 0;
        Assert.Equal(CSharpStatus.GenerationNotFound, api.CreateInstance(&create, &staleInstance));
    }

    private static void AssertNestedShadowCopyIsRejected(ManagedApiV1 api, string assemblyPath)
    {
        byte[] pathBytes = Encoding.UTF8.GetBytes(assemblyPath);
        byte[] shadowBytes = Encoding.UTF8.GetBytes(
            Path.Combine(Path.GetDirectoryName(assemblyPath)!, "NestedShadowCopy"));
        ulong generation = 0;
        fixed (byte* path = pathBytes)
        fixed (byte* shadow = shadowBytes)
        {
            var load = new CSharpGenerationLoadDesc
            {
                Size = (uint)sizeof(CSharpGenerationLoadDesc),
                Version = 1,
                AssemblyPath = new CSharpUtf8Span
                {
                    Data = path,
                    Length = checked((uint)pathBytes.Length),
                },
                ShadowCopyRoot = new CSharpUtf8Span
                {
                    Data = shadow,
                    Length = checked((uint)shadowBytes.Length),
                },
            };
            Assert.Equal(CSharpStatus.AssemblyLoadFailed, api.LoadGeneration(&load, &generation));
            Assert.Equal(0UL, generation);
        }
    }

    private static void AssertGenerationManifest(ManagedApiV1 api, ulong generation)
    {
        CSharpBuffer descriptor = default;
        Assert.Equal(
            CSharpStatus.Success,
            api.GetGenerationDescriptor(generation, &descriptor));
        try
        {
            byte[] json = new ReadOnlySpan<byte>(
                descriptor.Data,
                checked((int)descriptor.Size)).ToArray();
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            AssertHexId(root.GetProperty("generation"));

            foreach (JsonElement type in root.GetProperty("types").EnumerateArray())
            {
                AssertHexId(type.GetProperty("id"));
                foreach (JsonElement lifecycle in type.GetProperty("lifecycle").EnumerateArray())
                {
                    AssertHexId(lifecycle.GetProperty("id"));
                }
                foreach (JsonElement field in type.GetProperty("fields").EnumerateArray())
                {
                    AssertHexId(field.GetProperty("id"));
                }
                foreach (JsonElement message in type.GetProperty("messages").EnumerateArray())
                {
                    AssertHexId(message.GetProperty("id"));
                }
            }
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, api.FreeBuffer(&descriptor));
        }
    }

    private static void AssertHexId(JsonElement value)
    {
        Assert.Equal(JsonValueKind.String, value.ValueKind);
        Assert.Matches("^[0-9A-F]{16}$", value.GetString()!);
    }

    private static void RoundTripBlittable<T>(
        ManagedApiV1 api,
        ulong instance,
        string fieldName,
        T expected)
        where T : unmanaged, IEquatable<T>
    {
        T input = expected;
        var inputValue = new CSharpValue
        {
            Kind = CSharpValueKind.ByteSpan,
            Payload0 = (ulong)(nuint)(&input),
            Payload1 = (ulong)sizeof(T),
        };
        ulong fieldId = StableId.ExposedField(ScriptName, fieldName);
        Assert.Equal(CSharpStatus.Success, api.SetField(instance, fieldId, &inputValue));

        CSharpValue outputValue = default;
        Assert.Equal(CSharpStatus.Success, api.GetField(instance, fieldId, &outputValue));
        try
        {
            Assert.Equal(CSharpValueKind.ByteSpan, outputValue.Kind);
            Assert.Equal((ulong)sizeof(T), outputValue.Payload1);
            Assert.True(outputValue.Payload0 != 0);
            T actual = Unsafe.ReadUnaligned<T>((void*)outputValue.Payload0);
            Assert.Equal(expected, actual);
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, api.ReleaseValue(&outputValue));
        }
    }

    private static void RoundTripObjectHandle(
        ManagedApiV1 api,
        ulong instance,
        string fieldName,
        NativeObject expected)
    {
        var inputValue = new CSharpValue
        {
            Kind = CSharpValueKind.ObjectHandle,
            Payload0 = expected.Id,
            Payload1 = expected.Generation,
        };
        ulong fieldId = StableId.ExposedField(ScriptName, fieldName);
        Assert.Equal(CSharpStatus.Success, api.SetField(instance, fieldId, &inputValue));

        CSharpValue outputValue = default;
        Assert.Equal(CSharpStatus.Success, api.GetField(instance, fieldId, &outputValue));
        try
        {
            Assert.Equal(CSharpValueKind.ObjectHandle, outputValue.Kind);
            Assert.Equal(
                (CSharpValueFlags)((uint)expected.Kind << 8),
                outputValue.Flags);
            Assert.Equal(expected.Id, outputValue.Payload0);
            Assert.Equal(expected.Generation, outputValue.Payload1);
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, api.ReleaseValue(&outputValue));
        }
    }

    private static void AssertJsonMessageDispatch(ManagedApiV1 api, ulong instance)
    {
        byte[] invalidJson = Encoding.UTF8.GetBytes("{not-json");
        fixed (byte* json = invalidJson)
        {
            var payload = new CSharpValue
            {
                Kind = CSharpValueKind.Utf8String,
                Payload0 = (ulong)(nuint)json,
                Payload1 = checked((ulong)invalidJson.Length),
            };
            Assert.Equal(
                CSharpStatus.InvalidValue,
                api.DispatchMessage(
                    instance,
                    StableId.MessageHandler(ScriptName, "OnBeacon", "plMsgBeacon"),
                    &payload));
        }
        string error = ReadLastError(api);
        Assert.Contains("Generation=0x", error, StringComparison.Ordinal);
        Assert.Contains($"ManagedType='{ScriptName}'", error, StringComparison.Ordinal);
        Assert.Contains("Instance=0x", error, StringComparison.Ordinal);
        Assert.Contains("Owner=GameObject:0x0000000000001001@0x0000000000000001", error, StringComparison.Ordinal);
        Assert.Contains("World=World:0x0000000000002001@0x0000000000000001", error, StringComparison.Ordinal);
        Assert.Contains("OwnerComponent=Component:0x0000000000003001@0x0000000000000001", error, StringComparison.Ordinal);
        Assert.Contains("InvalidValueException", error, StringComparison.Ordinal);

        byte[] validJson = Encoding.UTF8.GetBytes("""{"enabled":false}""");
        fixed (byte* json = validJson)
        {
            var payload = new CSharpValue
            {
                Kind = CSharpValueKind.ByteSpan,
                Payload0 = (ulong)(nuint)json,
                Payload1 = checked((ulong)validJson.Length),
            };
            AssertSuccess(
                api,
                api.DispatchMessage(
                    instance,
                    StableId.MessageHandler(ScriptName, "OnBeacon", "plMsgBeacon"),
                    &payload));
        }

        CSharpValue enabled = default;
        Assert.Equal(
            CSharpStatus.Success,
            api.GetField(
                instance,
                StableId.ExposedField(ScriptName, nameof(RotatingBeacon.Enabled)),
                &enabled));
        try
        {
            Assert.Equal(CSharpValueKind.Boolean, enabled.Kind);
            Assert.Equal(0UL, enabled.Payload0);
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, api.ReleaseValue(&enabled));
        }
    }

    private static void AssertSuccess(ManagedApiV1 api, CSharpStatus status)
    {
        if (status == CSharpStatus.Success)
        {
            return;
        }

        Assert.Fail($"Managed API returned {status}: {ReadLastError(api)}");
    }

    private static string ReadLastError(ManagedApiV1 api)
    {
        CSharpBuffer error = default;
        if (api.GetLastError(&error) != CSharpStatus.Success)
        {
            return string.Empty;
        }

        try
        {
            return Encoding.UTF8.GetString(
                new ReadOnlySpan<byte>(error.Data, checked((int)error.Size)));
        }
        finally
        {
            _ = api.FreeBuffer(&error);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeLog(CSharpLogLevel level, CSharpUtf8Span message)
    {
        _ = level;
        _ = message;
        return CSharpStatus.Success;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus CapturingNativeLog(
        CSharpLogLevel level, CSharpUtf8Span message)
    {
        s_capturedLogLevel = level;
        s_capturedLogMessage = Encoding.UTF8.GetString(
            new ReadOnlySpan<byte>(message.Data, checked((int)message.Length)));
        return CSharpStatus.Success;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeValidateObject(CSharpObjectHandle value, uint expectedKind)
    {
        _ = value;
        _ = expectedKind;
        return CSharpStatus.Success;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeInvokeReflected(
        ulong functionId,
        CSharpObjectHandle target,
        CSharpValue* arguments,
        uint argumentCount,
        CSharpValue* result)
    {
        _ = functionId;
        _ = target;
        _ = arguments;
        _ = argumentCount;
        if (result is not null)
        {
            *result = default;
        }
        return CSharpStatus.Unsupported;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeInvokeMissingReflected(
        ulong functionId,
        CSharpObjectHandle target,
        CSharpValue* arguments,
        uint argumentCount,
        CSharpValue* result)
    {
        _ = functionId;
        _ = target;
        _ = arguments;
        _ = argumentCount;
        if (result is not null)
        {
            *result = default;
        }
        return CSharpStatus.MemberNotFound;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeReleaseValue(CSharpValue* value)
    {
        if (value is null)
        {
            return CSharpStatus.InvalidArgument;
        }

        *value = default;
        return CSharpStatus.Success;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeQueryExtension(
        CSharpUtf8Span name,
        uint minimumVersion,
        void** api)
    {
        _ = name;
        _ = minimumVersion;
        if (api is not null)
        {
            *api = null;
        }
        return CSharpStatus.Unsupported;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus NativeM0Probe(long input, long* output)
    {
        if (output is null)
        {
            return CSharpStatus.InvalidArgument;
        }

        *output = input + 1;
        return CSharpStatus.Success;
    }

    private static void AssertGuardIntact(byte* guard)
    {
        for (int index = 0; index < AbiGuardSize; ++index)
        {
            Assert.Equal(AbiGuardValue, guard[index]);
        }
    }
}
