using Plasma;

namespace Plasma.ScriptTestGame;

/// <summary>Console commands exercised by the host ABI round-trip test.</summary>
/// <remarks>
/// The commands report through <see cref="Log"/> rather than a static field. A generation is loaded
/// into its own collectible context, so this type is a different type there than the one a test in
/// the default context can see; the log callback is native and therefore genuinely shared.
/// </remarks>
public static class BeaconCommands
{
    [ConsoleCommand("test.noargs", "() - takes nothing")]
    public static void NoArgs() => Log.Info("noargs");

    [ConsoleCommand("test.echo", "(string text, int count, bool flag, float scale, double bias, uint steps)")]
    public static void Echo(string text, int count, bool flag, float scale, double bias, uint steps) =>
        Log.Info($"{text}|{count}|{flag}|{scale}|{bias}|{steps}");

    [ConsoleCommand("test.throws")]
    public static void Throws() => throw new InvalidOperationException("command failed on purpose");
}
