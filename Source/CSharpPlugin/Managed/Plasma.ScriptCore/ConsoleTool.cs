using System.ComponentModel;

namespace Plasma;

/// <summary>Registers a <see cref="ConsoleTool"/> so the engine console gives it a window.</summary>
/// <example>
/// <code>
/// [ConsoleTool("AI", Category = "Gameplay")]
/// public sealed class AiTools : ConsoleTool
/// {
///     private float _radius = 5.0f;
///
///     public override void OnDraw()
///     {
///         ImGui.Checkbox("Draw paths"u8, ref AiDebug.DrawPaths);
///         ImGui.SliderFloat("Query radius"u8, ref _radius, 0.0f, 50.0f);
///         if (ImGui.Button("Rebuild navmesh"u8))
///             Nav.Rebuild();
///     }
/// }
/// </code>
/// </example>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ConsoleToolAttribute : Attribute
{
    public ConsoleToolAttribute(string name)
    {
        Name = name;
    }

    /// <summary>Window title, and the key its open/closed state is saved under.</summary>
    public string Name { get; }

    /// <summary>Groups the entry in the console's Windows menu. Defaults to "Tools".</summary>
    public string Category { get; set; } = "Tools";

    /// <summary>When true the window stays visible after the console is closed.</summary>
    public bool Pinned { get; set; }
}

/// <summary>
/// A debug panel drawn by the engine console. Derive from this, mark it with
/// <see cref="ConsoleToolAttribute"/>, and draw with <see cref="ImGui"/> in <see cref="OnDraw"/>.
/// </summary>
/// <remarks>
/// <para>
/// One instance is created per script generation and lives until that generation is unloaded, so a
/// tool may hold state across frames - but not across a script rebuild.
/// </para>
/// <para>
/// <see cref="OnDraw"/> runs on the engine's main thread inside the console's ImGui frame, between a
/// Begin/End pair the console owns. Calling ImGui from anywhere else is outside a frame and will
/// fail. If the method throws, or leaves ImGui's stacks unbalanced, the console disables the tool
/// rather than letting it corrupt the rest of the UI.
/// </para>
/// </remarks>
public abstract class ConsoleTool
{
    /// <summary>Draws the tool's contents. Called once per frame while the tool is visible.</summary>
    public abstract void OnDraw();

    /// <summary>Called once after the tool is registered, before its first draw.</summary>
    public virtual void OnRegistered() { }

    /// <summary>Called before the tool is removed, when its generation is unloaded.</summary>
    public virtual void OnUnregistered() { }
}

/// <summary>One console tool, emitted by the source generator.</summary>
public sealed record ScriptToolDescriptor(
    ulong Id,
    string Name,
    string Category,
    bool Pinned,
    Func<ConsoleTool> Create);

/// <summary>Implemented by generated code in the collectible project assembly.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public interface IScriptToolProvider
{
    IReadOnlyList<ScriptToolDescriptor> GetTools();
}
