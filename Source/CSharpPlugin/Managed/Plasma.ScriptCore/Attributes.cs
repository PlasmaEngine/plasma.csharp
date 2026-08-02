namespace Plasma;

/// <summary>Marks a concrete <see cref="ComponentScript"/> as a Plasma script asset.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class PlasmaScriptAttribute : Attribute
{
    /// <summary>
    /// Gets or sets an optional persistent asset identity override. When omitted, the source
    /// generator derives a deterministic ID from the assembly name and fully qualified script type
    /// name. Set this only when preserving identity across an assembly, namespace, or class rename.
    /// </summary>
    public string? Id { get; set; }
}

/// <summary>Publishes a field or property through <c>plScriptComponent.Parameters</c>.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, Inherited = true)]
public sealed class ExposeAttribute : Attribute;

/// <summary>Marks a method taking one generated message value as a native message handler.</summary>
[AttributeUsage(AttributeTargets.Method, Inherited = true)]
public sealed class MessageHandlerAttribute : Attribute;

/// <summary>
/// Marks a user-defined C# class or value type as a synchronous Plasma message.
/// The message receives an automatic stable identity; no GUID or registration is required.
/// </summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct, Inherited = false)]
public sealed class PlasmaMessageAttribute : Attribute;

/// <summary>Adds editor range metadata to an exposed numeric value.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, Inherited = true)]
public sealed class RangeAttribute(double minimum, double maximum) : Attribute
{
    public double Minimum { get; } = minimum;
    public double Maximum { get; } = maximum;
}

/// <summary>Adds editor category metadata to an exposed value.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, Inherited = true)]
public sealed class CategoryAttribute(string name) : Attribute
{
    public string Name { get; } = name;
}

/// <summary>Overrides the editor-facing name of an exposed value.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, Inherited = true)]
public sealed class DisplayNameAttribute(string name) : Attribute
{
    public string Name { get; } = name;
}

/// <summary>Marks a value type emitted from a reflected native message descriptor.</summary>
[AttributeUsage(AttributeTargets.Struct, Inherited = false)]
[System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
public sealed class GeneratedMessageAttribute : Attribute
{
    public GeneratedMessageAttribute(string nativeTypeName)
    {
        NativeTypeName = nativeTypeName;
    }

    public string NativeTypeName { get; }
}
