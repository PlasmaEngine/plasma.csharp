using System.Text.Json;
using Plasma.ManagedHost;

namespace Plasma.ManagedTests;

public sealed class KeepOnReloadTests
{
    private sealed class Inventory
    {
        public int Coins { get; set; }
        public List<string> Items { get; set; } = [];
    }

    private sealed class VersionOne : ComponentScript
    {
        [KeepOnReload] public int Score;
        [KeepOnReload] private double _timer;
        [KeepOnReload] public Vec3 Checkpoint { get; set; }
        [KeepOnReload] public Inventory Bag { get; set; } = new();
        [KeepOnReload] public string Renamed = "";
        public int NotKept;

        public double Timer { get => _timer; set => _timer = value; }
    }

    // The same script after an edit: Renamed changed type, NotKept is gone, a new member arrived.
    private sealed class VersionTwo : ComponentScript
    {
        [KeepOnReload] public int Score = 0;
        [KeepOnReload] private double _timer = 0;
        [KeepOnReload] public Vec3 Checkpoint { get; set; }
        [KeepOnReload] public Inventory Bag { get; set; } = new();
        [KeepOnReload] public int Renamed = 7;
        [KeepOnReload] public bool Added = true;

        public double Timer => _timer;
    }

    [Fact]
    public void KeptMembersSurviveAFullReloadAndTypeChangesKeepTheirDefault()
    {
        var before = new VersionOne { Score = 42, Timer = 1.5, Checkpoint = new Vec3(1, 2, 3), Renamed = "text", NotKept = 9 };
        before.Bag.Coins = 5;
        before.Bag.Items.Add("key");

        string? state = KeptMembers.Capture(before);
        Assert.NotNull(state);
        Assert.DoesNotContain("NotKept", state);

        var after = new VersionTwo();
        using (JsonDocument document = JsonDocument.Parse(state))
            KeptMembers.Restore(after, document.RootElement);

        Assert.Equal(42, after.Score);
        Assert.Equal(1.5, after.Timer);
        Assert.Equal(new Vec3(1, 2, 3), after.Checkpoint);
        Assert.Equal(5, after.Bag.Coins);
        Assert.Equal(["key"], after.Bag.Items);
        Assert.Equal(7, after.Renamed);
        Assert.True(after.Added);
    }

    [Fact]
    public void AScriptWithoutKeptMembersCapturesNothing()
    {
        Assert.Null(KeptMembers.Capture(new ScriptSchedulerTestsScript()));
        Assert.False(KeptMembers.Has(typeof(ScriptSchedulerTestsScript)));
    }

    private sealed class ScriptSchedulerTestsScript : ComponentScript
    {
        public int Value = 1;
    }
}
