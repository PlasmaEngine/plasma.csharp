namespace Plasma;

/// <summary>Generated, allocation-free construction and native type mapping for component wrappers.</summary>
public interface INativeComponent<TSelf> where TSelf : struct, INativeComponent<TSelf>
{
    NativeObject Native { get; }
    static abstract string NativeTypeName { get; }
    static abstract TSelf FromNative(NativeObject native);
}
