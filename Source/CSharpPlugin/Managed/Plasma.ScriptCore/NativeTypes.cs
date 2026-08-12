using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Plasma;

public enum NativeObjectKind : uint
{
    Invalid,
    World,
    GameObject,
    Component,
    Resource,
    ReflectedObject,
}

/// <summary>
/// A generation-checked reference to a native object. It intentionally contains no native pointer.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
[DebuggerDisplay("{Kind} {Id} (generation {Generation})")]
public readonly struct NativeObject : IEquatable<NativeObject>
{
    public NativeObject(ulong id, ulong generation, NativeObjectKind kind)
    {
        Id = id;
        Generation = generation;
        Kind = kind;
        Reserved = 0;
    }

    public ulong Id { get; }
    public ulong Generation { get; }
    public NativeObjectKind Kind { get; }
    private uint Reserved { get; }

    public bool IsValid => Id != 0 && Kind != NativeObjectKind.Invalid;
    public bool Equals(NativeObject other) =>
        Id == other.Id && Generation == other.Generation && Kind == other.Kind;
    public override bool Equals(object? obj) => obj is NativeObject other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Id, Generation, Kind);
    public static bool operator ==(NativeObject left, NativeObject right) => left.Equals(right);
    public static bool operator !=(NativeObject left, NativeObject right) => !left.Equals(right);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Time(double Seconds)
{
    public static Time Zero => default;
    public static Time FromSeconds(double seconds) => new(seconds);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Angle(float Radians)
{
    public static Angle FromRadians(float radians) => new(radians);
    public static Angle FromDegrees(float degrees) => new(degrees * (MathF.PI / 180.0f));
    public float Degrees => Radians * (180.0f / MathF.PI);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct TempHashedString(ulong Hash);

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec2(float X, float Y)
{
    public float Length => MathF.Sqrt(X * X + Y * Y);
    public Vec2 GetNormalized() => Length > float.Epsilon ? this / Length : default;

    public static Vec2 operator +(Vec2 left, Vec2 right) =>
        new(left.X + right.X, left.Y + right.Y);
    public static Vec2 operator -(Vec2 left, Vec2 right) =>
        new(left.X - right.X, left.Y - right.Y);
    public static Vec2 operator *(Vec2 value, float scale) =>
        new(value.X * scale, value.Y * scale);
    public static Vec2 operator *(float scale, Vec2 value) => value * scale;
    public static Vec2 operator /(Vec2 value, float divisor) =>
        new(value.X / divisor, value.Y / divisor);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec2U32(uint X, uint Y);

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec3(float X, float Y, float Z)
{
    public static Vec3 AxisX => new(1, 0, 0);
    public static Vec3 AxisY => new(0, 1, 0);
    public static Vec3 AxisZ => new(0, 0, 1);

    public float Length => MathF.Sqrt(X * X + Y * Y + Z * Z);
    public Vec3 GetNormalized() => Length > float.Epsilon ? this / Length : default;

    public static Vec3 operator +(Vec3 left, Vec3 right) =>
        new(left.X + right.X, left.Y + right.Y, left.Z + right.Z);
    public static Vec3 operator -(Vec3 left, Vec3 right) =>
        new(left.X - right.X, left.Y - right.Y, left.Z - right.Z);
    public static Vec3 operator -(Vec3 value) => new(-value.X, -value.Y, -value.Z);
    public static Vec3 operator *(Vec3 value, float scale) =>
        new(value.X * scale, value.Y * scale, value.Z * scale);
    public static Vec3 operator *(float scale, Vec3 value) => value * scale;
    public static Vec3 operator /(Vec3 value, float divisor) =>
        new(value.X / divisor, value.Y / divisor, value.Z / divisor);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec4(float X, float Y, float Z, float W)
{
    public static Vec4 operator +(Vec4 left, Vec4 right) =>
        new(left.X + right.X, left.Y + right.Y, left.Z + right.Z, left.W + right.W);
    public static Vec4 operator -(Vec4 left, Vec4 right) =>
        new(left.X - right.X, left.Y - right.Y, left.Z - right.Z, left.W - right.W);
    public static Vec4 operator *(Vec4 value, float scale) =>
        new(value.X * scale, value.Y * scale, value.Z * scale, value.W * scale);
    public static Vec4 operator *(float scale, Vec4 value) => value * scale;
    public static Vec4 operator /(Vec4 value, float divisor) =>
        new(value.X / divisor, value.Y / divisor, value.Z / divisor, value.W / divisor);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Quat(float X, float Y, float Z, float W)
{
    public static Quat Identity => new(0, 0, 0, 1);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Color(float R, float G, float B, float A = 1.0f)
{
    public static Color White => new(1, 1, 1);
    public static Color Black => new(0, 0, 0);
    public static Color DarkRed => new(0.5f, 0, 0);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Transform(Vec3 Position, Quat Rotation, Vec3 Scale)
{
    public static Transform Identity => new(default, Quat.Identity, new Vec3(1, 1, 1));

    public static Transform FromPosition(Vec3 position) =>
        new(position, Quat.Identity, new Vec3(1, 1, 1));

    public static Transform FromPositionRotation(Vec3 position, Quat rotation) =>
        new(position, rotation, new Vec3(1, 1, 1));
}

/// <summary>A borrowed, non-owning view of UTF-8 bytes. Matches <c>plCSharpUtf8Span</c>.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal unsafe struct Utf8Span
{
    public byte* Data;
    public uint Length;
    public uint Reserved;
}

public readonly partial record struct World(NativeObject Native)
{
    public bool IsValid => Native.Kind == NativeObjectKind.World && Native.IsValid;
}

public readonly partial record struct GameObject(NativeObject Native)
{
    public bool IsValid => Native.Kind == NativeObjectKind.GameObject && Native.IsValid;
}

public readonly partial record struct Component(NativeObject Native)
{
    public bool IsValid => Native.Kind == NativeObjectKind.Component && Native.IsValid;
}

public readonly record struct ResourceHandle<T>(ulong Id, ulong Generation)
{
    public bool IsValid => Id != 0;
}
