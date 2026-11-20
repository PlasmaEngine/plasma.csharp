using System.ComponentModel;

namespace Plasma;

/// <summary>Base class for scripts hosted by an existing native <c>plScriptComponent</c>.</summary>
public abstract partial class ComponentScript
{
    public GameObject Owner { get; private set; }
    public World World { get; private set; }
    public Component OwnerComponent { get; private set; }
    private readonly CancellationTokenSource _lifetime = new();
    private readonly Queue<Action> _posted = new();
    private bool _acceptPostedActions;

    /// <summary>Cancellation owned by this instance. Destruction and full reload cancel it;
    /// an in-place method update retains it. Background work must not call native APIs.</summary>
    public CancellationToken LifetimeCancellation => _lifetime.Token;

    /// <summary>Queues synchronous work for the next Update callback in this instance's world.
    /// Override Update to enable dispatch. Returns false after destruction or if 256 actions
    /// are already pending. Async delegates are unsupported: perform I/O in the background
    /// and post only the synchronous application of its result.</summary>
    public bool PostToUpdate(Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        if (action.Method.IsDefined(typeof(System.Runtime.CompilerServices.AsyncStateMachineAttribute), false))
            throw new ArgumentException("Post a synchronous action; async delegates can escape the native world scope.", nameof(action));
        lock (_posted)
        {
            if (!_acceptPostedActions || _posted.Count >= 256) return false;
            _posted.Enqueue(action);
            return true;
        }
    }

    [EditorBrowsable(EditorBrowsableState.Never)]
    public void __EnableUpdateDispatch(bool enabled)
    {
        _updateDispatch = enabled;
        lock (_posted) _acceptPostedActions = enabled && !_lifetime.IsCancellationRequested;
    }

    [EditorBrowsable(EditorBrowsableState.Never)]
    public void __DrainPostedActions()
    {
        // Bounded work per update; actions posted by a draining action wait until a later frame.
        int count;
        lock (_posted) count = Math.Min(_posted.Count, 64);
        for (int i = 0; i < count; ++i)
        {
            Action action;
            lock (_posted)
            {
                if (!_acceptPostedActions || _posted.Count == 0) return;
                action = _posted.Dequeue();
            }

            // A failing action is reported on its own and does not pause this script's Update.
            try
            {
                action();
            }
            catch (Exception exception)
            {
                ReportFailure($"A posted action of '{GetType().FullName}' failed", exception);
            }
        }
    }

    /// <summary>Called once after the managed object has received its native owner context.</summary>
    public virtual void Initialize() { }

    /// <summary>Called before the managed instance releases its native context.</summary>
    public virtual void Deinitialize() { }

    public virtual void OnActivated() { }
    public virtual void OnDeactivated() { }
    public virtual void OnSimulationStarted() { }
    public virtual void Update(Time deltaTime) { }

    [EditorBrowsable(EditorBrowsableState.Never)]
    public void __Attach(in ScriptOwnerContext context)
    {
        Owner = new GameObject(context.Owner);
        World = new World(context.World);
        OwnerComponent = new Component(context.OwnerComponent);
    }

    [EditorBrowsable(EditorBrowsableState.Never)]
    public void __Detach()
    {
        lock (_posted)
        {
            _acceptPostedActions = false;
            _posted.Clear();
        }

        // Each step runs even when an earlier one throws, so the native context is always released.
        DetachStep("timers and waits", () => _scheduler?.Close());
        DetachStep("lifetime cancellation", _lifetime.Cancel);
        DetachStep("save game, settings and platform clients", DisposeServices);

        Owner = default;
        World = default;
        OwnerComponent = default;
    }

    private void DetachStep(string name, Action step)
    {
        try
        {
            step();
        }
        catch (Exception exception)
        {
            ReportFailure($"Releasing {name} of '{GetType().FullName}' failed", exception);
        }
    }

    // Outside the engine (script tests) there is no native log to write to; the failure is still contained.
    private static void ReportFailure(string message, Exception exception)
    {
        try
        {
            Log.Error(message, exception);
        }
        catch (InvalidOperationException)
        {
        }
    }
}

public readonly record struct ScriptOwnerContext(
    NativeObject Owner,
    NativeObject World,
    NativeObject OwnerComponent);

/// <summary>The world scoped to the current native-to-managed script call.</summary>
public static class ScriptExecutionContext
{
    [ThreadStatic]
    private static World s_currentWorld;

    public static World CurrentWorld =>
        s_currentWorld.IsValid
            ? s_currentWorld
            : throw new InvalidOperationException("No Plasma script is executing on this thread.");

    [EditorBrowsable(EditorBrowsableState.Never)]
    public static Scope Enter(World world) => new(world);

    public ref struct Scope
    {
        private readonly World _previous;

        internal Scope(World world)
        {
            _previous = s_currentWorld;
            s_currentWorld = world;
        }

        public void Dispose() => s_currentWorld = _previous;
    }
}
