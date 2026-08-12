using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Plasma;

/// <summary>
/// Exposes a static method as a console command, callable from the engine's console.
/// </summary>
/// <remarks>
/// The method must be <c>static</c>. Parameters are limited to <c>bool</c>, <c>int</c>,
/// <c>uint</c>, <c>float</c>, <c>double</c> and <c>string</c>, because the console's command
/// interpreter is Lua and marshals through <c>plVariant</c>; anything else is a compile error rather
/// than a surprise at the prompt. A return value is allowed but ignored, matching native console
/// functions.
/// </remarks>
/// <example>
/// <code>
/// [ConsoleCommand("ai.reveal", "(bool enabled) - draw every agent's current path")]
/// public static void Reveal(bool enabled) => AiDebug.DrawPaths = enabled;
/// </code>
/// </example>
[AttributeUsage(AttributeTargets.Method, Inherited = false)]
public sealed class ConsoleCommandAttribute : Attribute
{
    public ConsoleCommandAttribute(string name, string help = "")
    {
        Name = name;
        Help = help;
    }

    /// <summary>The name typed at the console. Also what auto-completion matches on.</summary>
    public string Name { get; }

    /// <summary>One-line description shown by the console next to the name.</summary>
    public string Help { get; }
}

/// <summary>Argument types a console command can take.</summary>
public enum ScriptCommandParameterKind : uint
{
    Bool = 0,
    Int = 1,
    UInt = 2,
    Float = 3,
    Double = 4,
    String = 5,
}

public delegate void ScriptCommandInvoker(ReadOnlySpan<object?> arguments);

/// <summary>One console command, emitted by the source generator.</summary>
public sealed record ScriptCommandDescriptor(
    ulong Id,
    string Name,
    string Help,
    IReadOnlyList<ScriptCommandParameterKind> Parameters,
    ScriptCommandInvoker Invoke);

/// <summary>Implemented by generated code in the collectible project assembly.</summary>
public interface IScriptCommandProvider
{
    IReadOnlyList<ScriptCommandDescriptor> GetCommands();
}

/// <summary>Writes to the engine console from a console command.</summary>
/// <remarks>
/// <para>
/// Output goes to the main console, so it lands in whichever front-end invoked the command: the
/// in-game ImGui console's command window, or the editor's console panel over
/// <c>plConsoleCmdMsgToEngine</c>.
/// </para>
/// <para>
/// Named <c>DebugConsole</c> rather than <c>Console</c> on purpose: generated script projects enable
/// implicit usings, so a <c>Plasma.Console</c> would be ambiguous with <c>System.Console</c> in every
/// file that writes a line.
/// </para>
/// </remarks>
public static unsafe class DebugConsole
{
    /// <summary>Mirrors plConsoleString::Type.</summary>
    private enum LineType : uint
    {
        Default = 0,
        Error = 1,
        SeriousWarning = 2,
        Warning = 3,
        Note = 4,
        Success = 5,
    }

    public static void Print(string text) => Write(LineType.Default, text);

    public static void Success(string text) => Write(LineType.Success, text);

    public static void Note(string text) => Write(LineType.Note, text);

    public static void Warning(string text) => Write(LineType.Warning, text);

    public static void Error(string text) => Write(LineType.Error, text);

    private static void Write(LineType type, string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return;
        }

        ConsoleApi api = ConsoleApi.Require();
        byte[] bytes = Encoding.UTF8.GetBytes(text);
        fixed (byte* data = bytes)
        {
            api.Print((uint)type, new Utf8Span
            {
                Data = data,
                Length = checked((uint)bytes.Length),
            });
        }
    }
}

/// <summary>The native "Plasma.Console" extension table. Infrastructure, not an authoring API.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal unsafe struct ConsoleApi
{
    private const uint RequiredVersion = 1;

    public uint Size;
    public uint Version;
    public delegate* unmanaged[Cdecl]<uint, Utf8Span, NativeCallStatus> Print;

    private static ConsoleApi s_api;
    private static bool s_resolved;
    private static readonly object s_lock = new();

    internal static ConsoleApi Require()
    {
        lock (s_lock)
        {
            if (!s_resolved)
            {
                s_resolved = true;
                s_api = NativeBridge.QueryExtension<ConsoleApi>("Plasma.Console", RequiredVersion) is { } api &&
                        api.Size >= (uint)sizeof(ConsoleApi) && api.Version >= RequiredVersion
                    ? api
                    : default;
            }

            if (s_api.Print is null)
            {
                throw new PlasmaException("The engine console is not available in this host.");
            }

            return s_api;
        }
    }

    [EditorBrowsable(EditorBrowsableState.Never)]
    internal static void Reset()
    {
        lock (s_lock)
        {
            s_resolved = false;
            s_api = default;
        }
    }
}
