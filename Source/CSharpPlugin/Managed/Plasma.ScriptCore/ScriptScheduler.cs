using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace Plasma;

/// <summary>A timer started by <see cref="ComponentScript.After"/> or <see cref="ComponentScript.Every"/>.</summary>
public sealed class ScriptTimer
{
    internal ScriptTimer(Time due, Time interval, Action action)
    {
        Due = due;
        Interval = interval;
        Action = action;
    }

    internal Time Due { get; set; }
    internal Time Interval { get; }
    internal Action Action { get; }

    /// <summary>False once a one-shot timer has fired, after <see cref="Stop"/>, or when its script went away.</summary>
    public bool IsActive { get; internal set; } = true;

    public void Stop() => IsActive = false;
}

/// <summary>Awaitable game-time wait, returned by <see cref="ComponentScript.Delay"/>, <see cref="ComponentScript.NextFrame"/>
/// and <see cref="ComponentScript.Until"/>. The awaiting method resumes inside its script's Update.</summary>
public readonly struct ScriptWait : ICriticalNotifyCompletion
{
    private readonly ScriptScheduler _scheduler;
    private readonly Time _due;
    private readonly Func<bool>? _condition;
    private readonly bool _nextFrame;

    internal ScriptWait(ScriptScheduler scheduler, Time due, Func<bool>? condition, bool nextFrame)
    {
        _scheduler = scheduler;
        _due = due;
        _condition = condition;
        _nextFrame = nextFrame;
    }

    public ScriptWait GetAwaiter() => this;

    /// <summary>A wait that is already over (a zero delay, a condition that already holds) does not suspend.</summary>
    public bool IsCompleted => !_nextFrame && (_condition is null ? _due <= _scheduler.Now : _condition());

    public void GetResult()
    {
    }

    public void OnCompleted(Action continuation) => _scheduler.AddWait(_due, _condition, _nextFrame, continuation);
    public void UnsafeOnCompleted(Action continuation) => OnCompleted(continuation);
}

/// <summary>Timers, waits and async continuations of one script, advanced by its Update.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class ScriptScheduler
{
    private readonly struct Wait(Time due, Func<bool>? condition, ulong frame, Action continuation)
    {
        public Time Due { get; } = due;
        public Func<bool>? Condition { get; } = condition;
        public ulong Frame { get; } = frame;
        public Action Continuation { get; } = continuation;
    }

    private readonly List<ScriptTimer> _timers = [];
    private readonly List<Wait> _waits = [];
    private readonly Queue<(SendOrPostCallback Callback, object? State)> _posted = new();
    private readonly object _postedLock = new();
    private ulong _frame;
    private bool _closed;

    /// <summary>Game time seen by this script: the sum of its Update deltas. It stops while the game is paused.</summary>
    public Time Now { get; private set; }

    internal SynchronizationContext Context { get; }

    internal ScriptScheduler(Action<string, Exception> reportFailure)
    {
        ReportFailure = reportFailure;
        Context = new ScriptSynchronizationContext(this);
    }

    internal Action<string, Exception> ReportFailure { get; }

    internal ScriptTimer AddTimer(Time delay, Time interval, Action action)
    {
        var timer = new ScriptTimer(Now + delay, interval, action);
        if (!_closed)
            _timers.Add(timer);
        else
            timer.IsActive = false;
        return timer;
    }

    internal void AddWait(Time due, Func<bool>? condition, bool nextFrame, Action continuation)
    {
        if (!_closed)
            _waits.Add(new Wait(due, condition, nextFrame ? _frame : ulong.MaxValue, continuation));
    }

    internal void Post(SendOrPostCallback callback, object? state)
    {
        lock (_postedLock)
        {
            if (!_closed)
                _posted.Enqueue((callback, state));
        }
    }

    /// <summary>Runs everything that is due. Called at the start of the script's Update, inside its world scope.</summary>
    internal void Advance(Time deltaTime)
    {
        ++_frame;
        Now += deltaTime;

        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(Context);
        try
        {
            RunPosted();
            RunWaits();
            RunTimers();
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }

    /// <summary>Drops everything pending: the script is going away and nothing may run for it afterwards.</summary>
    internal void Close()
    {
        lock (_postedLock)
        {
            _closed = true;
            _posted.Clear();
        }

        foreach (ScriptTimer timer in _timers)
            timer.IsActive = false;

        _timers.Clear();
        _waits.Clear();
    }

    private void RunPosted()
    {
        // Work posted while running waits until the next frame.
        int count;
        lock (_postedLock)
            count = _posted.Count;

        for (int i = 0; i < count && !_closed; ++i)
        {
            (SendOrPostCallback Callback, object? State) item;
            lock (_postedLock)
            {
                if (_posted.Count == 0)
                    return;
                item = _posted.Dequeue();
            }

            Run("An async continuation", () => item.Callback(item.State));
        }
    }

    private void RunWaits()
    {
        // Iterate a snapshot: a continuation that awaits again adds a new wait for a later frame.
        int count = _waits.Count;
        for (int i = 0; i < count && !_closed;)
        {
            Wait wait = _waits[i];
            bool due = wait.Frame != ulong.MaxValue
                ? wait.Frame < _frame
                : wait.Condition is null ? wait.Due <= Now : Evaluate(wait.Condition);

            if (!due)
            {
                ++i;
                continue;
            }

            _waits.RemoveAt(i);
            --count;
            Run("An awaited script wait", wait.Continuation);
        }
    }

    private void RunTimers()
    {
        int count = _timers.Count;
        for (int i = 0; i < count && !_closed;)
        {
            ScriptTimer timer = _timers[i];
            if (!timer.IsActive)
            {
                _timers.RemoveAt(i);
                --count;
                continue;
            }

            if (timer.Due > Now)
            {
                ++i;
                continue;
            }

            if (timer.Interval > Time.Zero)
            {
                // A long frame fires a repeating timer once, not once per missed interval.
                timer.Due = timer.Due + timer.Interval > Now ? timer.Due + timer.Interval : Now + timer.Interval;
                ++i;
            }
            else
            {
                timer.IsActive = false;
                _timers.RemoveAt(i);
                --count;
            }

            Run("A script timer", timer.Action);
        }
    }

    private bool Evaluate(Func<bool> condition)
    {
        try
        {
            return condition();
        }
        catch (Exception exception)
        {
            ReportFailure("A script wait condition failed", exception);
            return true;
        }
    }

    private void Run(string what, Action action)
    {
        try
        {
            action();
        }
        catch (Exception exception)
        {
            ReportFailure(what + " failed", exception);
        }
    }

    private sealed class ScriptSynchronizationContext(ScriptScheduler scheduler) : SynchronizationContext
    {
        public override void Post(SendOrPostCallback d, object? state) => scheduler.Post(d, state);

        // Synchronous sends run in place: the caller already waits for them.
        public override void Send(SendOrPostCallback d, object? state) => d(state);

        public override SynchronizationContext CreateCopy() => this;
    }
}
