using Plasma.Testing;

namespace Plasma.ScriptTestGame;

/// <summary>Sample script tests: plain logic, run by the editor's project checks without a world.</summary>
public static class BeaconTests
{
    [PlasmaTest]
    public static void SpeedMessageDefaults()
    {
        var message = new SetBeaconSpeed();
        Assert.Near(0.0, message.DegreesPerSecond);
        Assert.False(message.Consumed);
    }

    [PlasmaTest]
    public static void DirectionsAreOpposite()
    {
        Assert.Equal(-(short)BeaconDirection.Forward, (short)BeaconDirection.Reverse);
    }
}
