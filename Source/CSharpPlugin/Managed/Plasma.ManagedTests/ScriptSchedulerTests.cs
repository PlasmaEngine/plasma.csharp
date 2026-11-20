namespace Plasma.ManagedTests;

public sealed class ScriptSchedulerTests
{
    private sealed class Script : ComponentScript
    {
        public override void Update(Time deltaTime) { }
    }

    private sealed class ScriptWithoutUpdate : ComponentScript
    {
    }

    private static Script CreateScript()
    {
        var script = new Script();
        script.__EnableUpdateDispatch(true);
        return script;
    }

    private static void Frame(ComponentScript script, double seconds = 0.1) => script.__BeginUpdate(Time.FromSeconds(seconds));

    [Fact]
    public void TimersFollowGameTime()
    {
        Script script = CreateScript();
        int once = 0, repeated = 0;
        script.After(Time.FromSeconds(0.25), () => ++once);
        ScriptTimer every = script.Every(Time.FromSeconds(0.1), () => ++repeated);

        Frame(script);
        Frame(script);
        Assert.Equal(0, once);
        Frame(script);
        Assert.Equal(1, once);
        Assert.Equal(3, repeated);

        every.Stop();
        Frame(script);
        Assert.Equal(3, repeated);
        Assert.False(every.IsActive);

        // One long frame fires a repeating timer once.
        int caughtUp = 0;
        script.Every(Time.FromSeconds(0.1), () => ++caughtUp);
        Frame(script, 1.0);
        Assert.Equal(1, caughtUp);
    }

    [Fact]
    public async Task AwaitsResumeInUpdateOnGameTime()
    {
        Script script = CreateScript();
        var steps = new List<string>();
        bool open = false;

        async Task Sequence()
        {
            steps.Add("start");
            await script.NextFrame();
            steps.Add("next frame");
            await script.Delay(Time.FromSeconds(0.25));
            steps.Add("delay");
            await script.Until(() => open);
            steps.Add("until");
        }

        script.Run(Sequence);
        Assert.Equal(["start"], steps);

        Frame(script);
        Assert.Equal(["start", "next frame"], steps);
        Frame(script);
        Frame(script);
        Assert.Equal(2, steps.Count);
        Frame(script);
        Assert.Equal(3, steps.Count);
        Frame(script);
        Assert.Equal(3, steps.Count);
        open = true;
        Frame(script);
        Assert.Equal(["start", "next frame", "delay", "until"], steps);
        await Task.CompletedTask;
    }

    [Fact]
    public void AnAwaitedTaskResumesOnTheScriptThreadDuringUpdate()
    {
        Script script = CreateScript();
        int threadBefore = Environment.CurrentManagedThreadId;
        int? resumedOn = null;
        bool resumedInsideContext = false;

        script.Run(async () =>
        {
            await Task.Delay(10);
            resumedOn = Environment.CurrentManagedThreadId;
            resumedInsideContext = SynchronizationContext.Current is not null;
        });

        // The continuation is queued for the script, not run on a pool thread.
        Thread.Sleep(100);
        Assert.Null(resumedOn);

        Frame(script);
        Assert.Equal(threadBefore, resumedOn);
        Assert.True(resumedInsideContext);
    }

    [Fact]
    public void NothingRunsForADetachedScript()
    {
        Script script = CreateScript();
        int ran = 0;
        script.After(Time.FromSeconds(0.1), () => ++ran);
        script.Run(async () =>
        {
            await script.NextFrame();
            ++ran;
        });

        script.__Detach();
        Frame(script);
        Frame(script);
        Assert.Equal(0, ran);
    }

    [Fact]
    public void AFailingTimerDoesNotStopTheOthers()
    {
        Script script = CreateScript();
        int ran = 0;
        script.After(Time.Zero, () => throw new InvalidOperationException("timer failure"));
        script.After(Time.Zero, () => ++ran);

        Frame(script);
        Assert.Equal(1, ran);
    }

    private sealed class FakeClient : IOwnedService
    {
        public FakeClient()
        {
            Owner = ComponentScript.Current;
            Owner?.Own(this);
        }

        public ComponentScript? Owner { get; }
        public int Pumped { get; private set; }
        public bool Disposed { get; private set; }

        public void Pump() => ++Pumped;

        public void Dispose()
        {
            Disposed = true;
            Owner?.Release(this);
        }
    }

    [Fact]
    public void AClientCreatedInACallbackBelongsToThatScript()
    {
        Script script = CreateScript();
        FakeClient client;
        using (script.__EnterContext())
            client = new FakeClient();

        Assert.Same(script, client.Owner);
        Assert.Null(ComponentScript.Current);

        Frame(script);
        Frame(script);
        Assert.Equal(2, client.Pumped);

        script.__Detach();
        Assert.True(client.Disposed);
    }

    [Fact]
    public void AScriptWithoutUpdateIsToldToAddOne()
    {
        var script = new ScriptWithoutUpdate();
        script.__EnableUpdateDispatch(false);
        InvalidOperationException error = Assert.Throws<InvalidOperationException>(() => script.After(Time.FromSeconds(1), () => { }));
        Assert.Contains("Override Update(Time) in ScriptWithoutUpdate", error.Message);
    }
}
