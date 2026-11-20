namespace Plasma.ManagedTests;

public sealed class MathTests
{
    private static void Near(Vec3 expected, Vec3 actual, float tolerance = 1e-5f)
    {
        Assert.True(Vec3.Distance(expected, actual) < tolerance, $"Expected {expected}, got {actual}.");
    }

    [Fact]
    public void VectorsHaveTheUsualOperations()
    {
        Assert.Equal(0f, Vec3.Dot(Vec3.AxisX, Vec3.AxisY));
        Assert.Equal(Vec3.AxisZ, Vec3.Cross(Vec3.AxisX, Vec3.AxisY));
        Assert.Equal(25f, new Vec3(3, 4, 0).LengthSquared);
        Assert.Equal(new Vec3(0.5f, 1, 1.5f), Vec3.Lerp(Vec3.Zero, new Vec3(1, 2, 3), 0.5f));
        Assert.Equal(new Vec3(2, 6, 12), new Vec3(1, 2, 3) * new Vec3(2, 3, 4));
        Assert.Equal(90f, Vec3.AngleBetween(Vec3.AxisX, Vec3.AxisY).Degrees, 3);
    }

    [Fact]
    public void ANearlyZeroVectorNormalizesToZero()
    {
        Assert.Equal(Vec3.Zero, new Vec3(1e-30f, 0, 0).GetNormalized());
        Assert.Equal(Vec3.AxisX, new Vec3(5, 0, 0).GetNormalized());
    }

    [Fact]
    public void QuaternionsRotateAndComposeLikeTheEngine()
    {
        Quat yaw = Quat.FromAxisAngle(Vec3.AxisZ, Angle.FromDegrees(90));
        Quat pitch = Quat.FromAxisAngle(Vec3.AxisY, Angle.FromDegrees(90));

        Near(Vec3.AxisY, yaw * Vec3.AxisX);

        // a * b applies b first: X pitched about Y gives -Z, then yawed about Z stays -Z.
        Near(-Vec3.AxisZ, (yaw * pitch) * Vec3.AxisX);
        Near(-Vec3.AxisZ, yaw * (pitch * Vec3.AxisX));

        Near(Vec3.AxisX, yaw.GetInverse() * (yaw * Vec3.AxisX));
        Near(Vec3.AxisY, Quat.FromTo(Vec3.AxisX, Vec3.AxisY) * Vec3.AxisX);
        Near(-Vec3.AxisX, Quat.FromTo(Vec3.AxisX, -Vec3.AxisX) * Vec3.AxisX);
        Near(new Vec3(MathF.Sqrt(0.5f), MathF.Sqrt(0.5f), 0), Quat.Slerp(Quat.Identity, yaw, 0.5f) * Vec3.AxisX);
    }

    [Fact]
    public void TransformsMapLocalPointsIntoTheParent()
    {
        var parent = new Transform(new Vec3(10, 0, 0), Quat.FromAxisAngle(Vec3.AxisZ, Angle.FromDegrees(90)), new Vec3(2, 2, 2));
        Near(new Vec3(10, 2, 0), parent.TransformPosition(Vec3.AxisX));
        Near(new Vec3(0, 2, 0), parent.TransformDirection(Vec3.AxisX));

        Transform child = parent * Transform.FromPosition(Vec3.AxisX);
        Near(new Vec3(10, 2, 0), child.Position);
    }

    [Fact]
    public void TimeAndAngleSupportArithmeticAndComparison()
    {
        Assert.Equal(Time.FromSeconds(1.5), Time.FromSeconds(1) + Time.FromMilliseconds(500));
        Assert.True(Time.FromSeconds(1) < Time.FromSeconds(2));
        Assert.Equal(2.0, Time.FromSeconds(4) / Time.FromSeconds(2));
        Assert.Equal(180f, (Angle.FromDegrees(90) * 2).Degrees, 3);
    }

    [Fact]
    public void ValueTypesPrint()
    {
        // Records print their properties; none may return its own type.
        Assert.Contains("W = 1", Quat.Identity.ToString());
        Assert.Contains("Rotation", Transform.Identity.ToString());
        Assert.Contains("Seconds = 2", Time.FromSeconds(2).ToString());
    }

    [Fact]
    public void ValueTypesConvertToSystemNumerics()
    {
        System.Numerics.Vector3 numerics = new Vec3(1, 2, 3);
        Vec3 back = numerics + System.Numerics.Vector3.One;
        Assert.Equal(new Vec3(2, 3, 4), back);
    }
}
