using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Plasma;

public enum NativeCallStatus : int
{
    Success = 0,
    InvalidArgument = 1,
    AbiMismatch = 2,
    NotInitialized = 3,
    RuntimeLoadFailed = 4,
    AssemblyLoadFailed = 5,
    GenerationNotFound = 6,
    GenerationInUse = 7,
    TypeNotFound = 8,
    InstanceNotFound = 9,
    MemberNotFound = 10,
    InvalidValue = 11,
    ManagedException = 12,
    UnloadIncomplete = 13,
    BufferTooSmall = 14,
    Unsupported = 15,
}

public enum NativeValueKind : uint
{
    Null,
    Boolean,
    Int64,
    UInt64,
    Double,
    Utf8String,
    ObjectHandle,
    ByteSpan,
}

[Flags]
public enum NativeValueFlags : uint
{
    None = 0,
    ManagedOwned = 1,
    ObjectKindWorld = 1u << 8,
    ObjectKindGameObject = 2u << 8,
    ObjectKindComponent = 3u << 8,
    ObjectKindResource = 4u << 8,
    ObjectKindReflectedObject = 5u << 8,
}

/// <summary>Fixed-layout value used by generated wrappers to call reflected native functions.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct NativeValue
{
    public NativeValueKind Kind;
    public NativeValueFlags Flags;
    public ulong Payload0;
    public ulong Payload1;
}

/// <summary>
/// Process-wide native function table shared by the permanent bootstrap and every collectible script generation.
/// This is infrastructure for generated code and is not a gameplay authoring API.
/// </summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class NativeBridge
{
    private const uint AbiVersion = 1;
    private const uint ValueLayoutVersion = 2;

    private static NativeApiV1 s_api;
    private static bool s_configured;
    private static readonly object s_extensionLock = new();
    private static WorldApiV1 s_worldApi;
    private static bool s_worldApiResolved;

    [EditorBrowsable(EditorBrowsableState.Never)]
    public static void Configure(nint apiPointer)
    {
        if (apiPointer == 0)
        {
            throw new ArgumentNullException(nameof(apiPointer));
        }

        uint apiSize = *(uint*)apiPointer;
        if (apiSize < (uint)sizeof(NativeApiV1))
        {
            throw new InvalidOperationException(
                $"Native bridge ABI table is too small: {apiSize}/{sizeof(NativeApiV1)}.");
        }

        var api = *(NativeApiV1*)apiPointer;
        if (api.Version != AbiVersion ||
            api.PointerSize != (uint)sizeof(void*) ||
            api.ValueLayoutVersion != ValueLayoutVersion)
        {
            throw new InvalidOperationException(
                $"Native bridge ABI mismatch: {api.Size}/{api.Version}/{api.PointerSize}/{api.ValueLayoutVersion}.");
        }

        s_api = api;
        s_configured = true;
        s_worldApi = default;
        s_worldApiResolved = false;
        DebugApi.Reset();
        ConsoleApi.Reset();
    }

    [EditorBrowsable(EditorBrowsableState.Never)]
    public static void Reset()
    {
        s_configured = false;
        s_api = default;
        s_worldApi = default;
        s_worldApiResolved = false;
        DebugApi.Reset();
        ConsoleApi.Reset();
    }

    public static NativeCallStatus ValidateObject(NativeObject value, NativeObjectKind expectedKind)
    {
        EnsureConfigured();
        if (s_api.ValidateObject == null)
        {
            return NativeCallStatus.Unsupported;
        }

        return s_api.ValidateObject(ToHandle(value), (uint)expectedKind);
    }

    public static NativeCallStatus InvokeReflected(
        ulong functionId,
        NativeObject target,
        ReadOnlySpan<NativeValue> arguments,
        out NativeValue result)
    {
        EnsureConfigured();
        result = default;
        if (s_api.InvokeReflected == null)
        {
            return NativeCallStatus.Unsupported;
        }

        fixed (NativeValue* argumentData = arguments)
        fixed (NativeValue* resultData = &result)
        {
            return s_api.InvokeReflected(
                functionId,
                ToHandle(target),
                argumentData,
                checked((uint)arguments.Length),
                resultData);
        }
    }

    public static NativeCallStatus ReleaseValue(ref NativeValue value)
    {
        EnsureConfigured();
        if (s_api.ReleaseNativeValue == null)
        {
            return NativeCallStatus.Unsupported;
        }

        fixed (NativeValue* valueData = &value)
        {
            return s_api.ReleaseNativeValue(valueData);
        }
    }

    public static void LogInfo(string message)
    {
        WriteLog(1, message);
    }

    internal static void WriteLog(uint level, string message)
    {
        EnsureConfigured();
        if (s_api.Log == null)
        {
            return;
        }

        byte[] bytes = Encoding.UTF8.GetBytes(message);
        fixed (byte* data = bytes)
        {
            _ = s_api.Log(level, new Utf8Span
            {
                Data = data,
                Length = checked((uint)bytes.Length),
            });
        }
    }

    internal static NativeCallStatus SendMessage(
        NativeObject target,
        NativeObject senderComponent,
        string nativeTypeName,
        string payloadJson,
        uint routing)
    {
        EnsureConfigured();
        if (s_api.QueryExtension == null)
        {
            return NativeCallStatus.Unsupported;
        }

        if (!TryGetWorldApi(out WorldApiV1 worldApi))
        {
            return NativeCallStatus.Unsupported;
        }

        byte[] typeNameBytes = Encoding.UTF8.GetBytes(nativeTypeName);
        byte[] payloadBytes = Encoding.UTF8.GetBytes(payloadJson);
        fixed (byte* typeNameData = typeNameBytes)
        fixed (byte* payloadData = payloadBytes)
        {
            return worldApi.SendMessage(
                ToHandle(target),
                ToHandle(senderComponent),
                new Utf8Span
                {
                    Data = typeNameData,
                    Length = checked((uint)typeNameBytes.Length),
                },
                new Utf8Span
                {
                    Data = payloadData,
                    Length = checked((uint)payloadBytes.Length),
                },
                routing);
        }
    }

    internal static NativeCallStatus SendManagedMessage(
        NativeObject target,
        NativeObject senderComponent,
        ulong messageId,
        ulong payloadToken,
        uint routing)
    {
        EnsureConfigured();
        if (!TryGetWorldApi(out WorldApiV1 worldApi) ||
            worldApi.SendManagedMessage == null)
        {
            return NativeCallStatus.Unsupported;
        }

        return worldApi.SendManagedMessage(
            ToHandle(target),
            ToHandle(senderComponent),
            messageId,
            payloadToken,
            routing);
    }

    /// <summary>
    /// Resolves an optional native extension table by name. Returns null when the host does not
    /// provide it, which is a normal answer rather than an error: extension tables exist so the
    /// frozen host ABI does not have to grow every time a capability is added.
    /// </summary>
    internal static T? QueryExtension<T>(string name, uint minimumVersion)
        where T : unmanaged
    {
        EnsureConfigured();
        if (s_api.QueryExtension == null)
        {
            return null;
        }

        byte[] extensionName = Encoding.UTF8.GetBytes(name);
        fixed (byte* extensionData = extensionName)
        {
            void* extensionPointer = null;
            NativeCallStatus status = s_api.QueryExtension(
                new Utf8Span
                {
                    Data = extensionData,
                    Length = checked((uint)extensionName.Length),
                },
                minimumVersion,
                &extensionPointer);

            if (status != NativeCallStatus.Success || extensionPointer is null)
            {
                return null;
            }

            return *(T*)extensionPointer;
        }
    }

    private static bool TryGetWorldApi(out WorldApiV1 worldApi)
    {
        lock (s_extensionLock)
        {
            if (!s_worldApiResolved)
            {
                s_worldApiResolved = true;

                if (QueryExtension<WorldApiV1>("Plasma.World", 2) is { } candidate &&
                    candidate.Size >= (uint)sizeof(WorldApiV1) &&
                    candidate.Version == 2 &&
                    candidate.SendMessage != null)
                {
                    s_worldApi = candidate;
                }
            }

            worldApi = s_worldApi;
            return worldApi.SendMessage != null;
        }
    }

    private static void EnsureConfigured()
    {
        if (!s_configured)
        {
            throw new InvalidOperationException("The Plasma native bridge has not been configured.");
        }
    }

    private static ObjectHandle ToHandle(NativeObject value) => new()
    {
        Value = value.Id,
        Generation = value.Generation,
        Kind = (uint)value.Kind,
    };

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct ObjectHandle
    {
        public ulong Value;
        public ulong Generation;
        public uint Kind;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct NativeApiV1
    {
        public uint Size;
        public uint Version;
        public uint PointerSize;
        public uint ValueLayoutVersion;

        public delegate* unmanaged[Cdecl]<uint, Utf8Span, NativeCallStatus> Log;
        public delegate* unmanaged[Cdecl]<ObjectHandle, uint, NativeCallStatus> ValidateObject;
        public delegate* unmanaged[Cdecl]<ulong, ObjectHandle, NativeValue*, uint, NativeValue*, NativeCallStatus> InvokeReflected;
        public delegate* unmanaged[Cdecl]<NativeValue*, NativeCallStatus> ReleaseNativeValue;
        public delegate* unmanaged[Cdecl]<Utf8Span, uint, void**, NativeCallStatus> QueryExtension;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct WorldApiV1
    {
        public uint Size;
        public uint Version;
        public delegate* unmanaged[Cdecl]<
            ObjectHandle,
            ObjectHandle,
            Utf8Span,
            Utf8Span,
            uint,
            NativeCallStatus> SendMessage;
        public delegate* unmanaged[Cdecl]<
            ObjectHandle,
            ObjectHandle,
            ulong,
            ulong,
            uint,
            NativeCallStatus> SendManagedMessage;
    }
}
