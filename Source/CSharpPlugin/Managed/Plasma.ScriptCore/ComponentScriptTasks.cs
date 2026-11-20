using System.ComponentModel;

namespace Plasma;

// Timers, game-time waits and async methods. All of them advance in the script's Update, so a script that
// uses them overrides Update (an empty override is enough). They stop with the game and are dropped when
// the script goes away: nothing scheduled here ever runs for a deleted object.
public abstract partial class ComponentScript
{
    private ScriptScheduler? _scheduler;
    private bool _updateDispatch;

    /// <summary>Game time seen by this script: the sum of its Update deltas. It does not advance while the game is paused.</summary>
    public Time GameTime => _scheduler?.Now ?? Time.Zero;

    /// <summary>Runs <paramref name="action"/> once, <paramref name="delay"/> of game time from now.</summary>
    public ScriptTimer After(Time delay, Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        return Scheduler(nameof(After)).AddTimer(delay, Time.Zero, action);
    }

    /// <summary>Runs <paramref name="action"/> every <paramref name="interval"/> of game time until stopped.
    /// After a long frame it runs once, not once per missed interval.</summary>
    public ScriptTimer Every(Time interval, Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        if (interval <= Time.Zero)
            throw new ArgumentOutOfRangeException(nameof(interval), "The interval must be positive.");

        return Scheduler(nameof(Every)).AddTimer(interval, interval, action);
    }

    /// <summary><c>await Delay(Time.FromSeconds(2));</c> resumes in this script's Update after that much game time.</summary>
    public ScriptWait Delay(Time duration)
    {
        ScriptScheduler scheduler = Scheduler(nameof(Delay));
        return new ScriptWait(scheduler, scheduler.Now + duration, null, nextFrame: false);
    }

    /// <summary><c>await NextFrame();</c> resumes in this script's next Update.</summary>
    public ScriptWait NextFrame() => new(Scheduler(nameof(NextFrame)), Time.Zero, null, nextFrame: true);

    /// <summary><c>await Until(() => door.IsOpen);</c> resumes in the first Update in which the condition holds.</summary>
    public ScriptWait Until(Func<bool> condition)
    {
        ArgumentNullException.ThrowIfNull(condition);
        return new ScriptWait(Scheduler(nameof(Until)), Time.Zero, condition, nextFrame: false);
    }

    /// <summary>Starts an async method that belongs to this script. Its awaits resume inside this script's
    /// Update, even <c>await Task.Delay(...)</c>; an exception is logged instead of being lost.</summary>
    public void Run(Func<Task> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        ScriptScheduler scheduler = Scheduler(nameof(Run));

        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(scheduler.Context);
        try
        {
            Task task = work();
            if (!task.IsCompleted || task.IsFaulted)
            {
                task.ContinueWith(
                    static (finished, state) => ((ComponentScript)state!).ReportTaskFailure(finished),
                    this,
                    CancellationToken.None,
                    TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
            }
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }

    /// <summary>Advances posted actions, timers and waits; called by the host at the start of Update.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void __BeginUpdate(Time deltaTime)
    {
        __DrainPostedActions();
        _scheduler?.Advance(deltaTime);
    }

    /// <summary>Makes this script's synchronization context current for a host callback, so async code
    /// started there resumes in this script's Update. Does nothing for scripts without Update.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public ScriptContextScope __EnterContext() =>
        new(_updateDispatch ? (_scheduler ??= new ScriptScheduler(ReportFailure)).Context : null);

    private ScriptScheduler Scheduler(string api)
    {
        if (!_updateDispatch)
        {
            throw new InvalidOperationException(
                $"{GetType().Name}.{api}: timers, waits and async methods advance in Update. Override Update(Time) in {GetType().Name}; an empty override is enough.");
        }

        return _scheduler ??= new ScriptScheduler(ReportFailure);
    }

    private void ReportTaskFailure(Task task) =>
        ReportFailure($"An async method of '{GetType().FullName}' failed", task.Exception?.InnerExceptions.Count == 1 ? task.Exception.InnerException! : task.Exception!);
}

/// <summary>Restores the previous synchronization context when a host callback ends.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public readonly ref struct ScriptContextScope
{
    private readonly SynchronizationContext? _previous;
    private readonly bool _active;

    internal ScriptContextScope(SynchronizationContext? context)
    {
        _active = context is not null;
        _previous = SynchronizationContext.Current;
        if (_active)
            SynchronizationContext.SetSynchronizationContext(context);
    }

    public void Dispose()
    {
        if (_active)
            SynchronizationContext.SetSynchronizationContext(_previous);
    }
}
