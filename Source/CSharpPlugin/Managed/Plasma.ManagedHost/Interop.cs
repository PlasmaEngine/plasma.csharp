using System.Runtime.InteropServices;

namespace Plasma.ManagedHost;

public enum CSharpStatus : int
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

public enum CSharpLogLevel : uint
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
    Dev = 4,
    Success = 5,
    SeriousWarning = 6,
}

public enum CSharpValueKind : uint
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
public enum CSharpValueFlags : uint
{
    None = 0,
    ManagedOwned = 1,
    ObjectKindWorld = 1u << 8,
    ObjectKindGameObject = 2u << 8,
    ObjectKindComponent = 3u << 8,
    ObjectKindResource = 4u << 8,
    ObjectKindReflectedObject = 5u << 8,
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct CSharpUtf8Span
{
    public byte* Data;
    public uint Length;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct CSharpBuffer
{
    public void* Data;
    public uint Size;
    public uint Capacity;
    public ulong OwnerToken;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct CSharpObjectHandle
{
    public ulong Value;
    public ulong Generation;
    public uint Kind;
    public uint Reserved;

    public readonly NativeObject ToNativeObject() =>
        new(Value, Generation, (NativeObjectKind)Kind);
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct CSharpValue
{
    public CSharpValueKind Kind;
    public CSharpValueFlags Flags;
    public ulong Payload0;
    public ulong Payload1;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct CSharpGenerationLoadDesc
{
    public uint Size;
    public uint Version;
    public uint Flags;
    public uint Reserved;
    public CSharpUtf8Span AssemblyPath;
    public CSharpUtf8Span ShadowCopyRoot;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct CSharpInstanceCreateDesc
{
    public uint Size;
    public uint Version;
    public ulong Generation;
    public ulong TypeId;
    public CSharpObjectHandle Owner;
    public CSharpObjectHandle World;
    public CSharpObjectHandle OwnerComponent;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct CSharpUnloadReport
{
    public uint Size;
    public uint Version;
    public uint LoadContextAlive;
    public uint GcCycles;
    public uint LiveInstances;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct NativeApiV1
{
    public uint Size;
    public uint Version;
    public uint PointerSize;
    public uint ValueLayoutVersion;

    public delegate* unmanaged[Cdecl]<CSharpLogLevel, CSharpUtf8Span, CSharpStatus> Log;
    public delegate* unmanaged[Cdecl]<CSharpObjectHandle, uint, CSharpStatus> ValidateObject;
    public delegate* unmanaged[Cdecl]<ulong, CSharpObjectHandle, CSharpValue*, uint, CSharpValue*, CSharpStatus> InvokeReflected;
    public delegate* unmanaged[Cdecl]<CSharpValue*, CSharpStatus> ReleaseNativeValue;
    public delegate* unmanaged[Cdecl]<CSharpUtf8Span, uint, void**, CSharpStatus> QueryExtension;
    public delegate* unmanaged[Cdecl]<long, long*, CSharpStatus> M0Probe;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct ManagedApiV1
{
    public uint Size;
    public uint Version;
    public uint PointerSize;
    public uint ValueLayoutVersion;

    public delegate* unmanaged[Cdecl]<CSharpStatus> Shutdown;
    public delegate* unmanaged[Cdecl]<CSharpGenerationLoadDesc*, ulong*, CSharpStatus> LoadGeneration;
    public delegate* unmanaged[Cdecl]<ulong, CSharpBuffer*, CSharpStatus> GetGenerationDescriptor;
    public delegate* unmanaged[Cdecl]<CSharpInstanceCreateDesc*, ulong*, CSharpStatus> CreateInstance;
    public delegate* unmanaged[Cdecl]<ulong, CSharpStatus> DestroyInstance;
    public delegate* unmanaged[Cdecl]<ulong, ulong, CSharpValue*, uint, CSharpValue*, CSharpStatus> InvokeMethod;
    public delegate* unmanaged[Cdecl]<ulong, ulong, CSharpValue*, CSharpStatus> GetField;
    public delegate* unmanaged[Cdecl]<ulong, ulong, CSharpValue*, CSharpStatus> SetField;
    public delegate* unmanaged[Cdecl]<ulong, ulong, CSharpValue*, CSharpStatus> DispatchMessage;
    public delegate* unmanaged[Cdecl]<ulong, CSharpUnloadReport*, CSharpStatus> UnloadGeneration;
    public delegate* unmanaged[Cdecl]<CSharpBuffer*, CSharpStatus> GetLastError;
    public delegate* unmanaged[Cdecl]<CSharpBuffer*, CSharpStatus> FreeBuffer;
    public delegate* unmanaged[Cdecl]<CSharpValue*, CSharpStatus> ReleaseValue;
    public delegate* unmanaged[Cdecl]<CSharpUtf8Span, uint, void**, CSharpStatus> QueryExtension;
    public delegate* unmanaged[Cdecl]<long, long*, CSharpStatus> RunM0Probe;
}
