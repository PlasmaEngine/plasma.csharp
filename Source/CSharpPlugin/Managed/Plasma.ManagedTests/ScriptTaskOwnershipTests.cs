namespace Plasma.ManagedTests;

public sealed class ScriptTaskOwnershipTests
{
    private sealed class Script : ComponentScript
    {
        public override void Update(Time deltaTime) { }
    }

    [Fact]
    public async Task BackgroundResultsRunOnlyDuringUpdateAndDetachCancelsPendingWork()
    {
        var script = new Script();
        var world = new World(new NativeObject(2, 1, NativeObjectKind.World));
        script.__Attach(new(default, world.Native, default));
        script.__EnableUpdateDispatch(true);
        int applied = 0;
        Assert.True(await Task.Run(() => script.PostToUpdate(() =>
        {
            Assert.Equal(world, ScriptExecutionContext.CurrentWorld);
            ++applied;
        })));
        Assert.Equal(0, applied);
        using (ScriptExecutionContext.Enter(world)) script.__DrainPostedActions();
        Assert.Equal(1, applied);
        CancellationToken lifetime = script.LifetimeCancellation;
        Assert.True(script.PostToUpdate(() => ++applied));
        script.__Detach();
        Assert.True(lifetime.IsCancellationRequested);
        Assert.False(script.PostToUpdate(() => ++applied));
        script.__DrainPostedActions();
        Assert.Equal(1, applied);
    }

    [Fact]
    public void AFailingPostedActionDoesNotStopTheOthers()
    {
        var script = new Script();
        script.__EnableUpdateDispatch(true);
        int applied = 0;
        Assert.True(script.PostToUpdate(() => throw new InvalidOperationException("posted action failure")));
        Assert.True(script.PostToUpdate(() => ++applied));

        script.__DrainPostedActions();

        Assert.Equal(1, applied);
        script.__Detach();
    }

    [Fact]
    public void DetachReleasesEverythingWhenACancellationCallbackThrows()
    {
        var script = new Script();
        var world = new World(new NativeObject(2, 1, NativeObjectKind.World));
        script.__Attach(new(default, world.Native, default));
        script.LifetimeCancellation.Register(() => throw new InvalidOperationException("cancellation callback failure"));

        script.__Detach();

        Assert.False(script.World.IsValid);
        Assert.True(script.LifetimeCancellation.IsCancellationRequested);
    }

    [Fact]
    public void DispatchIsBoundedAndRejectsAsyncVoidActions()
    {
        var script = new Script();
        script.__EnableUpdateDispatch(true);
        int applied = 0;
        for (int i = 0; i < 256; ++i) Assert.True(script.PostToUpdate(() => ++applied));
        Assert.False(script.PostToUpdate(() => ++applied));
        script.__DrainPostedActions();
        Assert.Equal(64, applied);
        Assert.Throws<ArgumentException>(() => script.PostToUpdate(async () => await Task.Yield()));
        script.__Detach();
    }
}
