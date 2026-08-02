using System.Collections.ObjectModel;

namespace Plasma;

public enum ScriptLifecycleMethod : ulong
{
    Initialize = 0,
    Deinitialize = 1,
    OnActivated = 2,
    OnDeactivated = 3,
    OnSimulationStarted = 4,
    Update = 5,
}

public delegate ComponentScript ScriptFactory();
public delegate void ScriptLifecycleInvoker(ComponentScript instance, Time deltaTime);
public delegate object? ScriptFieldGetter(ComponentScript instance);
public delegate void ScriptFieldSetter(ComponentScript instance, object? value);
public delegate void ScriptMessageInvoker(ComponentScript instance, ReadOnlySpan<object?> values);

public sealed record ScriptFieldDescriptor(
    ulong Id,
    string Name,
    Type ValueType,
    object? DefaultValue,
    ScriptFieldGetter Get,
    ScriptFieldSetter Set,
    IReadOnlyDictionary<string, string> EditorMetadata);

public sealed record ScriptMessageDescriptor(
    ulong Id,
    string NativeTypeName,
    Type ValueType,
    ScriptMessageInvoker Invoke);

public sealed class ScriptTypeDescriptor
{
    public required ulong Id { get; init; }
    public required Guid PersistentId { get; init; }
    public required string ManagedName { get; init; }
    public required string SourceFile { get; init; }
    public required Type ManagedType { get; init; }
    public required ScriptFactory Create { get; init; }
    public required IReadOnlyDictionary<ScriptLifecycleMethod, ScriptLifecycleInvoker> Lifecycle { get; init; }
    public required IReadOnlyDictionary<ulong, ScriptFieldDescriptor> Fields { get; init; }
    public required IReadOnlyDictionary<ulong, ScriptMessageDescriptor> Messages { get; init; }

    public static IReadOnlyDictionary<TKey, TValue> Freeze<TKey, TValue>(
        IDictionary<TKey, TValue> values) where TKey : notnull =>
        new ReadOnlyDictionary<TKey, TValue>(values);
}

/// <summary>Implemented by generated code in the collectible project assembly.</summary>
public interface IScriptDescriptorProvider
{
    IReadOnlyList<ScriptTypeDescriptor> GetDescriptors();
}

public static class StableId
{
    private static readonly Guid ScriptPersistentIdNamespace =
        new("6ba7b811-9dad-11d1-80b4-00c04fd430c8");

    /// <summary>FNV-1a is deliberately fixed here so generated and bootstrap code agree.</summary>
    public static ulong Hash(string canonicalIdentity)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;

        ulong hash = offset;
        foreach (byte value in System.Text.Encoding.UTF8.GetBytes(canonicalIdentity))
        {
            hash ^= value;
            hash *= prime;
        }

        return hash;
    }

    public static ulong ScriptType(string fullyQualifiedName) =>
        Hash($"pl-csharp/type/v1:{fullyQualifiedName}");

    public static ulong ExposedField(string fullyQualifiedTypeName, string declaredName) =>
        Hash($"pl-csharp/field/v1:{fullyQualifiedTypeName}|{declaredName}");

    public static ulong MessageHandler(string fullyQualifiedTypeName, string methodName, string nativeMessageType) =>
        Hash($"pl-csharp/message/v1:{fullyQualifiedTypeName}|{methodName}|{nativeMessageType}");

    public static ulong CustomMessage(Type messageType)
    {
        ArgumentNullException.ThrowIfNull(messageType);
        return CustomMessage(
            messageType.Assembly.GetName().Name ?? string.Empty,
            messageType.FullName ?? messageType.Name);
    }

    public static ulong CustomMessage(string assemblyName, string fullyQualifiedMetadataTypeName) =>
        Hash($"pl-csharp/custom-message/v1:{assemblyName}|{fullyQualifiedMetadataTypeName}");

    /// <summary>
    /// Returns the automatic persistent ID used by <see cref="PlasmaScriptAttribute"/> when its
    /// <see cref="PlasmaScriptAttribute.Id"/> override is omitted.
    /// </summary>
    public static Guid ScriptPersistentId(
        string assemblyName,
        string fullyQualifiedMetadataTypeName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assemblyName);
        ArgumentException.ThrowIfNullOrWhiteSpace(fullyQualifiedMetadataTypeName);

        string canonicalIdentity =
            $"pl-csharp/script-persistent-id/v1\0{assemblyName}\0{fullyQualifiedMetadataTypeName}";
        byte[] namespaceBytes = ScriptPersistentIdNamespace.ToByteArray();
        SwapGuidByteOrder(namespaceBytes);

        byte[] nameBytes = System.Text.Encoding.UTF8.GetBytes(canonicalIdentity);
        byte[] input = new byte[namespaceBytes.Length + nameBytes.Length];
        Buffer.BlockCopy(namespaceBytes, 0, input, 0, namespaceBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, input, namespaceBytes.Length, nameBytes.Length);

        byte[] hash = System.Security.Cryptography.SHA1.HashData(input);
        byte[] guidBytes = new byte[16];
        Buffer.BlockCopy(hash, 0, guidBytes, 0, guidBytes.Length);
        guidBytes[6] = (byte)((guidBytes[6] & 0x0F) | 0x50);
        guidBytes[8] = (byte)((guidBytes[8] & 0x3F) | 0x80);
        SwapGuidByteOrder(guidBytes);
        return new Guid(guidBytes);
    }

    private static void SwapGuidByteOrder(byte[] bytes)
    {
        (bytes[0], bytes[3]) = (bytes[3], bytes[0]);
        (bytes[1], bytes[2]) = (bytes[2], bytes[1]);
        (bytes[4], bytes[5]) = (bytes[5], bytes[4]);
        (bytes[6], bytes[7]) = (bytes[7], bytes[6]);
    }
}
