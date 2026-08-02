using Plasma;

namespace Plasma.ScriptTestGame;

public enum BeaconDirection : short
{
    Reverse = -1,
    Forward = 1,
}

[PlasmaMessage]
public sealed class SetBeaconSpeed
{
    public float DegreesPerSecond { get; set; }
    public bool Consumed { get; set; }
}

[PlasmaScript]
public sealed partial class RotatingBeacon : ComponentScript
{
    [Expose, Range(0.0, 720.0), Category("Motion")]
    public float DegreesPerSecond { get; set; } = 90.0f;

    [Expose]
    public bool Enabled { get; set; } = true;

    [Expose]
    public bool ThrowOnUpdate { get; set; }

    [Expose]
    public BeaconDirection Direction { get; set; } = BeaconDirection.Reverse;

    [Expose]
    public Time Duration { get; set; } = Time.FromSeconds(1.25);

    [Expose]
    public Angle Heading { get; set; } = Angle.FromDegrees(90.0f);

    [Expose]
    public Vec2 Offset2 { get; set; } = new(1.0f, 2.0f);

    [Expose]
    public Vec3 Offset3 { get; set; } = new(3.0f, 4.0f, 5.0f);

    [Expose]
    public Vec4 Offset4 { get; set; } = new(6.0f, 7.0f, 8.0f, 9.0f);

    [Expose]
    public Quat Orientation { get; set; } = Quat.Identity;

    [Expose]
    public Color Tint { get; set; } = new(0.25f, 0.5f, 0.75f);

    [Expose]
    public Transform Pose { get; set; } = new(
        new Vec3(1.0f, 2.0f, 3.0f),
        Quat.Identity,
        new Vec3(1.0f, 1.0f, 1.0f));

    [Expose]
    public NativeObject Target { get; set; }

    [Expose]
    public World TargetWorld { get; set; }

    [Expose]
    public GameObject TargetGameObject { get; set; }

    [Expose]
    public Component TargetComponent { get; set; }

    public int SimulationStartCount { get; private set; }
    public Time LastUpdate { get; private set; }

    public override void OnSimulationStarted()
    {
        ++SimulationStartCount;
    }

    public override void Update(Time deltaTime)
    {
        if (ThrowOnUpdate)
        {
            throw new InvalidOperationException("Intentional source-location test failure.");
        }

        if (Enabled)
        {
            LastUpdate = deltaTime;
        }
    }

    [MessageHandler]
    private void OnBeacon(in MsgBeacon message)
    {
        Enabled = message.Enabled;
    }

    [MessageHandler]
    private void OnSetBeaconSpeed(SetBeaconSpeed message)
    {
        DegreesPerSecond = message.DegreesPerSecond;
        message.Consumed = true;
    }
}
