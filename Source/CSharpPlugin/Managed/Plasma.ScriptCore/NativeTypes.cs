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
    /// <summary>Checks native liveness in the current engine execution scope.</summary>
    public bool IsAlive => IsValid && NativeBridge.ValidateObject(this, Kind) == NativeCallStatus.Success;
    public bool Equals(NativeObject other) =>
        Id == other.Id && Generation == other.Generation && Kind == other.Kind;
    public override bool Equals(object? obj) => obj is NativeObject other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Id, Generation, Kind);
    public static bool operator ==(NativeObject left, NativeObject right) => left.Equals(right);
    public static bool operator !=(NativeObject left, NativeObject right) => !left.Equals(right);
}

// The value types below cross the native boundary as raw bytes: keep their fields, order and sizes.

/// <summary>A duration or point in time, in seconds.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Time(double Seconds) : IComparable<Time>
{
    public static Time Zero => default;
    public static Time FromSeconds(double seconds) => new(seconds);
    public static Time FromMilliseconds(double milliseconds) => new(milliseconds / 1000.0);
    public static Time FromMinutes(double minutes) => new(minutes * 60.0);

    public double Milliseconds => Seconds * 1000.0;
    public float SecondsF => (float)Seconds;

    public static Time operator +(Time left, Time right) => new(left.Seconds + right.Seconds);
    public static Time operator -(Time left, Time right) => new(left.Seconds - right.Seconds);
    public static Time operator -(Time value) => new(-value.Seconds);
    public static Time operator *(Time value, double scale) => new(value.Seconds * scale);
    public static Time operator *(double scale, Time value) => new(value.Seconds * scale);
    public static Time operator /(Time value, double divisor) => new(value.Seconds / divisor);
    public static double operator /(Time value, Time divisor) => value.Seconds / divisor.Seconds;
    public static bool operator <(Time left, Time right) => left.Seconds < right.Seconds;
    public static bool operator >(Time left, Time right) => left.Seconds > right.Seconds;
    public static bool operator <=(Time left, Time right) => left.Seconds <= right.Seconds;
    public static bool operator >=(Time left, Time right) => left.Seconds >= right.Seconds;
    public int CompareTo(Time other) => Seconds.CompareTo(other.Seconds);
}

/// <summary>An angle, stored in radians.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Angle(float Radians) : IComparable<Angle>
{
    public static Angle Zero => default;
    public static Angle FromRadians(float radians) => new(radians);
    public static Angle FromDegrees(float degrees) => new(degrees * (MathF.PI / 180.0f));
    public float Degrees => Radians * (180.0f / MathF.PI);

    public static Angle operator +(Angle left, Angle right) => new(left.Radians + right.Radians);
    public static Angle operator -(Angle left, Angle right) => new(left.Radians - right.Radians);
    public static Angle operator -(Angle value) => new(-value.Radians);
    public static Angle operator *(Angle value, float scale) => new(value.Radians * scale);
    public static Angle operator *(float scale, Angle value) => new(value.Radians * scale);
    public static Angle operator /(Angle value, float divisor) => new(value.Radians / divisor);
    public static bool operator <(Angle left, Angle right) => left.Radians < right.Radians;
    public static bool operator >(Angle left, Angle right) => left.Radians > right.Radians;
    public static bool operator <=(Angle left, Angle right) => left.Radians <= right.Radians;
    public static bool operator >=(Angle left, Angle right) => left.Radians >= right.Radians;
    public int CompareTo(Angle other) => Radians.CompareTo(other.Radians);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct TempHashedString(ulong Hash);

/// <summary>Below this squared length a vector is treated as zero and does not normalize.</summary>
internal static class VectorLimits
{
    public const float ZeroLengthSquared = 1e-12f;
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec2(float X, float Y)
{
    public static Vec2 Zero => default;
    public static Vec2 One => new(1, 1);

    public float Length => MathF.Sqrt(LengthSquared);
    public float LengthSquared => X * X + Y * Y;

    /// <summary>This vector scaled to length 1, or zero when it is (nearly) zero.</summary>
    public Vec2 GetNormalized() => LengthSquared > VectorLimits.ZeroLengthSquared ? this / Length : default;

    public static float Dot(Vec2 a, Vec2 b) => a.X * b.X + a.Y * b.Y;
    public static float Distance(Vec2 a, Vec2 b) => (a - b).Length;
    public static Vec2 Lerp(Vec2 from, Vec2 to, float t) => from + (to - from) * t;
    public static Vec2 Min(Vec2 a, Vec2 b) => new(MathF.Min(a.X, b.X), MathF.Min(a.Y, b.Y));
    public static Vec2 Max(Vec2 a, Vec2 b) => new(MathF.Max(a.X, b.X), MathF.Max(a.Y, b.Y));

    public static Vec2 operator +(Vec2 left, Vec2 right) =>
        new(left.X + right.X, left.Y + right.Y);
    public static Vec2 operator -(Vec2 left, Vec2 right) =>
        new(left.X - right.X, left.Y - right.Y);
    public static Vec2 operator -(Vec2 value) => new(-value.X, -value.Y);
    public static Vec2 operator *(Vec2 value, float scale) =>
        new(value.X * scale, value.Y * scale);
    public static Vec2 operator *(float scale, Vec2 value) => value * scale;
    /// <summary>Component-wise product.</summary>
    public static Vec2 operator *(Vec2 left, Vec2 right) => new(left.X * right.X, left.Y * right.Y);
    public static Vec2 operator /(Vec2 value, float divisor) =>
        new(value.X / divisor, value.Y / divisor);

    public static implicit operator System.Numerics.Vector2(Vec2 value) => new(value.X, value.Y);
    public static implicit operator Vec2(System.Numerics.Vector2 value) => new(value.X, value.Y);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec2U32(uint X, uint Y);

/// <summary>A 3D vector. The engine is Z-up: X forward, Y right, Z up.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec3(float X, float Y, float Z)
{
    public static Vec3 Zero => default;
    public static Vec3 One => new(1, 1, 1);
    public static Vec3 AxisX => new(1, 0, 0);
    public static Vec3 AxisY => new(0, 1, 0);
    public static Vec3 AxisZ => new(0, 0, 1);

    public float Length => MathF.Sqrt(LengthSquared);
    public float LengthSquared => X * X + Y * Y + Z * Z;

    /// <summary>This vector scaled to length 1, or zero when it is (nearly) zero.</summary>
    public Vec3 GetNormalized() => LengthSquared > VectorLimits.ZeroLengthSquared ? this / Length : default;

    public static float Dot(Vec3 a, Vec3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    public static Vec3 Cross(Vec3 a, Vec3 b) =>
        new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    public static float Distance(Vec3 a, Vec3 b) => (a - b).Length;
    public static float DistanceSquared(Vec3 a, Vec3 b) => (a - b).LengthSquared;
    public static Vec3 Lerp(Vec3 from, Vec3 to, float t) => from + (to - from) * t;
    public static Vec3 Min(Vec3 a, Vec3 b) => new(MathF.Min(a.X, b.X), MathF.Min(a.Y, b.Y), MathF.Min(a.Z, b.Z));
    public static Vec3 Max(Vec3 a, Vec3 b) => new(MathF.Max(a.X, b.X), MathF.Max(a.Y, b.Y), MathF.Max(a.Z, b.Z));

    /// <summary>The angle between two vectors; zero when either is (nearly) zero.</summary>
    public static Angle AngleBetween(Vec3 a, Vec3 b)
    {
        float lengths = MathF.Sqrt(a.LengthSquared * b.LengthSquared);
        return lengths > VectorLimits.ZeroLengthSquared
            ? Angle.FromRadians(MathF.Acos(Math.Clamp(Dot(a, b) / lengths, -1f, 1f)))
            : Angle.Zero;
    }

    public static Vec3 operator +(Vec3 left, Vec3 right) =>
        new(left.X + right.X, left.Y + right.Y, left.Z + right.Z);
    public static Vec3 operator -(Vec3 left, Vec3 right) =>
        new(left.X - right.X, left.Y - right.Y, left.Z - right.Z);
    public static Vec3 operator -(Vec3 value) => new(-value.X, -value.Y, -value.Z);
    public static Vec3 operator *(Vec3 value, float scale) =>
        new(value.X * scale, value.Y * scale, value.Z * scale);
    public static Vec3 operator *(float scale, Vec3 value) => value * scale;
    /// <summary>Component-wise product.</summary>
    public static Vec3 operator *(Vec3 left, Vec3 right) => new(left.X * right.X, left.Y * right.Y, left.Z * right.Z);
    public static Vec3 operator /(Vec3 value, float divisor) =>
        new(value.X / divisor, value.Y / divisor, value.Z / divisor);

    public static implicit operator System.Numerics.Vector3(Vec3 value) => new(value.X, value.Y, value.Z);
    public static implicit operator Vec3(System.Numerics.Vector3 value) => new(value.X, value.Y, value.Z);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vec4(float X, float Y, float Z, float W)
{
    public static Vec4 Zero => default;
    public static Vec4 One => new(1, 1, 1, 1);

    public float Length => MathF.Sqrt(LengthSquared);
    public float LengthSquared => X * X + Y * Y + Z * Z + W * W;

    public static float Dot(Vec4 a, Vec4 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    public static Vec4 Lerp(Vec4 from, Vec4 to, float t) => from + (to - from) * t;

    public static Vec4 operator +(Vec4 left, Vec4 right) =>
        new(left.X + right.X, left.Y + right.Y, left.Z + right.Z, left.W + right.W);
    public static Vec4 operator -(Vec4 left, Vec4 right) =>
        new(left.X - right.X, left.Y - right.Y, left.Z - right.Z, left.W - right.W);
    public static Vec4 operator -(Vec4 value) => new(-value.X, -value.Y, -value.Z, -value.W);
    public static Vec4 operator *(Vec4 value, float scale) =>
        new(value.X * scale, value.Y * scale, value.Z * scale, value.W * scale);
    public static Vec4 operator *(float scale, Vec4 value) => value * scale;
    public static Vec4 operator /(Vec4 value, float divisor) =>
        new(value.X / divisor, value.Y / divisor, value.Z / divisor, value.W / divisor);

    public static implicit operator System.Numerics.Vector4(Vec4 value) => new(value.X, value.Y, value.Z, value.W);
    public static implicit operator Vec4(System.Numerics.Vector4 value) => new(value.X, value.Y, value.Z, value.W);
}

/// <summary>A rotation. <c>a * b</c> rotates by <c>b</c> first, then by <c>a</c>, as in the engine.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Quat(float X, float Y, float Z, float W)
{
    public static Quat Identity => new(0, 0, 0, 1);

    /// <summary>A rotation by <paramref name="angle"/> around <paramref name="axis"/> (normalized here).</summary>
    public static Quat FromAxisAngle(Vec3 axis, Angle angle)
    {
        Vec3 unit = axis.GetNormalized();
        float half = angle.Radians * 0.5f;
        float sin = MathF.Sin(half);
        return new Quat(unit.X * sin, unit.Y * sin, unit.Z * sin, MathF.Cos(half));
    }

    /// <summary>The shortest rotation that turns direction <paramref name="from"/> into <paramref name="to"/>.</summary>
    public static Quat FromTo(Vec3 from, Vec3 to)
    {
        Vec3 a = from.GetNormalized();
        Vec3 b = to.GetNormalized();
        float dot = Vec3.Dot(a, b);
        if (dot < -0.999999f)
        {
            // Opposite directions: any perpendicular axis works.
            Vec3 axis = Vec3.Cross(Vec3.AxisX, a);
            if (axis.LengthSquared < 1e-6f)
                axis = Vec3.Cross(Vec3.AxisY, a);
            return FromAxisAngle(axis, Angle.FromRadians(MathF.PI));
        }

        Vec3 cross = Vec3.Cross(a, b);
        return new Quat(cross.X, cross.Y, cross.Z, 1 + dot).GetNormalized();
    }

    // Methods, not properties: a record prints its properties, and a Quat property returning a Quat would recurse.
    public Quat GetConjugate() => new(-X, -Y, -Z, W);

    /// <summary>The opposite rotation. For a unit quaternion this is the conjugate.</summary>
    public Quat GetInverse()
    {
        float lengthSquared = X * X + Y * Y + Z * Z + W * W;
        return lengthSquared > VectorLimits.ZeroLengthSquared
            ? new Quat(-X / lengthSquared, -Y / lengthSquared, -Z / lengthSquared, W / lengthSquared)
            : Identity;
    }

    public Quat GetNormalized()
    {
        float lengthSquared = X * X + Y * Y + Z * Z + W * W;
        if (lengthSquared <= VectorLimits.ZeroLengthSquared)
            return Identity;

        float inverse = 1f / MathF.Sqrt(lengthSquared);
        return new Quat(X * inverse, Y * inverse, Z * inverse, W * inverse);
    }

    public static float Dot(Quat a, Quat b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;

    /// <summary>Spherical interpolation along the shorter arc.</summary>
    public static Quat Slerp(Quat from, Quat to, float t)
    {
        float cos = Dot(from, to);
        if (cos < 0)
        {
            to = new Quat(-to.X, -to.Y, -to.Z, -to.W);
            cos = -cos;
        }

        if (cos > 0.9995f)
        {
            return new Quat(from.X + (to.X - from.X) * t, from.Y + (to.Y - from.Y) * t,
                from.Z + (to.Z - from.Z) * t, from.W + (to.W - from.W) * t).GetNormalized();
        }

        float angle = MathF.Acos(cos);
        float sin = MathF.Sin(angle);
        float a = MathF.Sin((1 - t) * angle) / sin;
        float b = MathF.Sin(t * angle) / sin;
        return new Quat(from.X * a + to.X * b, from.Y * a + to.Y * b, from.Z * a + to.Z * b, from.W * a + to.W * b);
    }

    public static Quat operator *(Quat a, Quat b) => new(
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    /// <summary>Rotates a vector.</summary>
    public static Vec3 operator *(Quat rotation, Vec3 vector)
    {
        var axis = new Vec3(rotation.X, rotation.Y, rotation.Z);
        Vec3 t = 2f * Vec3.Cross(axis, vector);
        return vector + rotation.W * t + Vec3.Cross(axis, t);
    }

    public static implicit operator System.Numerics.Quaternion(Quat value) => new(value.X, value.Y, value.Z, value.W);
    public static implicit operator Quat(System.Numerics.Quaternion value) => new(value.X, value.Y, value.Z, value.W);
}

/// <summary>A linear RGBA colour. Note that <c>default(Color)</c> is transparent black; use <see cref="Black"/> for opaque.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Color(float R, float G, float B, float A = 1.0f)
{
    public static Color White => new(1, 1, 1);
    public static Color Black => new(0, 0, 0);
    public static Color Transparent => new(0, 0, 0, 0);
    public static Color Red => new(1, 0, 0);
    public static Color Green => new(0, 1, 0);
    public static Color Blue => new(0, 0, 1);
    public static Color Yellow => new(1, 1, 0);
    public static Color DarkRed => new(0.5f, 0, 0);

    public Color WithAlpha(float alpha) => new(R, G, B, alpha);

    public static Color Lerp(Color from, Color to, float t) => new(
        from.R + (to.R - from.R) * t, from.G + (to.G - from.G) * t,
        from.B + (to.B - from.B) * t, from.A + (to.A - from.A) * t);

    public static Color operator +(Color left, Color right) => new(left.R + right.R, left.G + right.G, left.B + right.B, left.A + right.A);
    public static Color operator *(Color value, float scale) => new(value.R * scale, value.G * scale, value.B * scale, value.A * scale);
    /// <summary>Component-wise product, for tinting.</summary>
    public static Color operator *(Color left, Color right) => new(left.R * right.R, left.G * right.G, left.B * right.B, left.A * right.A);
}

[StructLayout(LayoutKind.Sequential)]
public readonly record struct Transform(Vec3 Position, Quat Rotation, Vec3 Scale)
{
    public static Transform Identity => new(default, Quat.Identity, new Vec3(1, 1, 1));

    public static Transform FromPosition(Vec3 position) =>
        new(position, Quat.Identity, new Vec3(1, 1, 1));

    public static Transform FromPositionRotation(Vec3 position, Quat rotation) =>
        new(position, rotation, new Vec3(1, 1, 1));

    /// <summary>A point from this transform's local space into its parent space.</summary>
    public Vec3 TransformPosition(Vec3 local) => Position + Rotation * (Scale * local);

    /// <summary>A direction from local space into parent space: rotated and scaled, not moved.</summary>
    public Vec3 TransformDirection(Vec3 local) => Rotation * (Scale * local);

    /// <summary>Applies <paramref name="local"/> inside <paramref name="parent"/>: <c>parent * child</c> gives the child's global transform.</summary>
    public static Transform operator *(Transform parent, Transform local) =>
        new(parent.TransformPosition(local.Position), parent.Rotation * local.Rotation, parent.Scale * local.Scale);
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
    /// <summary>Whether this refers to a world at all. It does not check that the world still exists; use <see cref="IsAlive"/>.</summary>
    public bool IsValid => Native.Kind == NativeObjectKind.World && Native.IsValid;

    /// <summary>Whether the world still exists. Asks the engine, so call it from a script callback.</summary>
    public bool IsAlive => IsValid && Native.IsAlive;
}

public readonly partial record struct GameObject(NativeObject Native)
{
    /// <summary>Whether this refers to an object at all. It does not check that the object still exists; use <see cref="IsAlive"/>.</summary>
    public bool IsValid => Native.Kind == NativeObjectKind.GameObject && Native.IsValid;

    /// <summary>Whether the object still exists. Asks the engine, so call it from a script callback.</summary>
    public bool IsAlive => IsValid && Native.IsAlive;
}

public readonly partial record struct Component(NativeObject Native) : INativeComponent<Component>
{
    public static string NativeTypeName => "plComponent";
    public static Component FromNative(NativeObject native) => new(native);

    /// <summary>Whether this refers to a component at all. It does not check that the component still exists; use <see cref="IsAlive"/>.</summary>
    public bool IsValid => Native.Kind == NativeObjectKind.Component && Native.IsValid;

    /// <summary>Whether the component still exists. Asks the engine, so call it from a script callback.</summary>
    public bool IsAlive => IsValid && Native.IsAlive;
}

public readonly record struct ResourceHandle<T>(ulong Id, ulong Generation)
{
    public bool IsValid => Id != 0;
}
