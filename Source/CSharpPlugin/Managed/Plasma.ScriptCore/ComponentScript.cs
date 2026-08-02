using System.ComponentModel;

namespace Plasma;

/// <summary>Base class for scripts hosted by an existing native <c>plScriptComponent</c>.</summary>
public abstract class ComponentScript
{
    public GameObject Owner { get; private set; }
    public World World { get; private set; }
    public Component OwnerComponent { get; private set; }

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
        Owner = default;
        World = default;
        OwnerComponent = default;
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
