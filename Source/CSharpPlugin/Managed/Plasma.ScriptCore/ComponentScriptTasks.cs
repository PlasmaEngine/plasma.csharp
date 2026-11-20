using System.ComponentModel;

namespace Plasma;

// Timers, game-time waits and async methods. All of them advance in the script's Update, so a script that
// uses them overrides Update (an empty override is enough). They stop with the game and are dropped when
// the script goes away: nothing scheduled here ever runs for a deleted object.
public abstract partial class ComponentScript
{
    private ScriptScheduler? _scheduler;
    private bool _updateDispatch;
    private List<IOwnedService>? _services;

    [ThreadStatic]
    private static ComponentScript? s_current;

    /// <summary>The script whose callback is running on this thread, if any.</summary>
    internal static ComponentScript? Current => s_current;

    /// <summary>Takes ownership of a save-game, settings or platform client: it is updated at the start of
    /// this script's Update and disposed when the script goes away.</summary>
    internal void Own(IOwnedService service) => (_services ??= []).Add(service);

    internal void Release(IOwnedService service) => _services?.Remove(service);

    private void PumpServices()
    {
        if (_services is null)
            return;

        foreach (IOwnedService service in _services.ToArray())
        {
            try
            {
                service.Pump();
            }
            catch (Exception exception)
            {
                ReportFailure($"Updating a {service.GetType().Name} client of '{GetType().FullName}' failed", exception);
            }
        }
    }

    private void DisposeServices()
    {
        if (_services is null)
            return;

        // Each client removes itself from the list as it is disposed.
        foreach (IOwnedService service in _services.ToArray())
        {
            try
            {
                service.Dispose();
            }
            catch (Exception exception)
            {
                ReportFailure($"Releasing a {service.GetType().Name} client of '{GetType().FullName}' failed", exception);
            }
        }

        _services.Clear();
    }

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
        ComponentScript? previous = s_current;
        s_current = this;
        try
        {
            __DrainPostedActions();
            PumpServices();
            _scheduler?.Advance(deltaTime);
        }
        finally
        {
            s_current = previous;
        }
    }

    /// <summary>Marks this script as the one running for a host callback: clients it creates become its own,
    /// and async code started there resumes in its Update (for scripts with Update).</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public ScriptContextScope __EnterContext() =>
        new(this, _updateDispatch ? (_scheduler ??= new ScriptScheduler(ReportFailure)).Context : null);

    internal static void SetCurrent(ComponentScript? script) => s_current = script;

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

/// <summary>Restores the previous running script and synchronization context when a host callback ends.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public readonly ref struct ScriptContextScope
{
    private readonly ComponentScript? _previousScript;
    private readonly SynchronizationContext? _previous;
    private readonly bool _active;

    internal ScriptContextScope(ComponentScript script, SynchronizationContext? context)
    {
        _previousScript = ComponentScript.Current;
        ComponentScript.SetCurrent(script);

        _active = context is not null;
        _previous = SynchronizationContext.Current;
        if (_active)
            SynchronizationContext.SetSynchronizationContext(context);
    }

    public void Dispose()
    {
        ComponentScript.SetCurrent(_previousScript);
        if (_active)
            SynchronizationContext.SetSynchronizationContext(_previous);
    }
}

/// <summary>The instance generated descriptors read exposed-field defaults from: created once per script type,
/// never attached to an object and never given lifecycle calls.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static class ScriptDefaults<T> where T : class
{
    private static T? s_instance;

    public static T Get(Func<T> create) => s_instance ??= create();
}

/// <summary>A client a script owns: updated at the start of its Update, disposed when it goes away.</summary>
internal interface IOwnedService : IDisposable
{
    void Pump();
}
