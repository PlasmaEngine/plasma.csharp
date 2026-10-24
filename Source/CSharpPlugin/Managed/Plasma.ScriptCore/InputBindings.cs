using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Plasma;

/// <summary>Scheme names used by the input bindings bridge. Separate from the generated InputScheme binding enum.</summary>
public enum InputBindingScheme { Any, KeyboardMouse, Gamepad, XR, Touch }

public enum InputRebindStatus { Idle, Arming, Listening, Completed, Cancelled, TimedOut, Conflict, Failed }

public enum InputRebindConflict { Ask, Swap, Replace, Reject, Allow }

/// <summary>The state of the player's interactive rebind session.</summary>
public sealed class InputRebindState
{
    public InputRebindStatus Status { get; init; }
    public bool Active { get; init; }
    public string Slot { get; init; } = "";
    public string SlotLabel { get; init; } = "";
    public string ConflictAction { get; init; } = "";
    public string ConflictActionGuid { get; init; } = "";
    public double SecondsRemaining { get; init; }
}

/// <summary>One binding of an action, filtered by the queried scheme.</summary>
public sealed class InputBindingInfo
{
    public string Guid { get; init; } = "";
    public string Scheme { get; init; } = "";
    public int Kind { get; init; }
    public bool Rebindable { get; init; }
    public string[] Slots { get; init; } = [];
    public string[] Labels { get; init; } = [];
    public string Prompt { get; init; } = "";
}

/// <summary>One action of the map. Primary and Secondary are the prompts of the first two bindings of the queried scheme.</summary>
public sealed class InputActionInfo
{
    public string Action { get; init; } = "";
    public string Guid { get; init; } = "";
    public string Display { get; init; } = "";
    public string Context { get; init; } = "";
    public int ValueType { get; init; }
    public bool Rebindable { get; init; }
    public string Primary { get; init; } = "";
    public string Secondary { get; init; } = "";
    public string PrimaryBinding { get; init; } = "";
    public string SecondaryBinding { get; init; } = "";
    public InputBindingInfo[] Bindings { get; init; } = [];
}

/// <summary>Reply of an input bindings command. Actions and Values are filled by Query, Prompt by GetPrompt.</summary>
public sealed class InputBindingsReply
{
    public InputRebindState Rebind { get; init; } = new();
    public string ActiveScheme { get; init; } = "";
    public bool HasMap { get; init; }
    public string Error { get; init; } = "";
    public InputActionInfo[] Actions { get; init; } = [];
    public Dictionary<string, float> Values { get; init; } = [];
    public string Prompt { get; init; } = "";
}

/// <summary>Player 0's input action map and saved rebinds, owned by the game application.
/// Call from script update. Completed rebinds, resets and value changes are saved by the engine on its next update.
/// Tuning keys: '&lt;Action&gt;.Sensitivity[.&lt;Scheme&gt;]', '&lt;Action&gt;.InvertX|InvertY[.&lt;Scheme&gt;]', 'DeadZone.&lt;Scheme&gt;'.</summary>
public static class InputBindings
{
    private static readonly JsonSerializerOptions Options = new() { Converters = { new JsonStringEnumConverter() } };

    public static InputBindingsReply Query(InputBindingScheme scheme = InputBindingScheme.Any) => Execute("Query", SchemeArgs(scheme));

    /// <summary>Starts listening for a new slot for the Nth binding of the scheme; an index past the end adds a binding.</summary>
    public static InputRebindState BeginRebind(string action, InputBindingScheme scheme, int bindingIndex = 0,
        InputRebindConflict conflict = InputRebindConflict.Ask, double timeoutSeconds = 10, int slotIndex = 0,
        bool acceptAxes = true, IEnumerable<string>? excludedSlots = null)
    {
        var args = SchemeArgs(scheme);
        args["Action"] = action;
        args["BindingIndex"] = bindingIndex;
        AddRebindArgs(args, conflict, timeoutSeconds, slotIndex, acceptAxes, excludedSlots);
        return Execute("BeginRebind", args).Rebind;
    }

    /// <summary>Starts listening for a new slot for the binding with the given guid.</summary>
    public static InputRebindState BeginRebind(string action, string bindingGuid, InputBindingScheme scheme = InputBindingScheme.Any,
        InputRebindConflict conflict = InputRebindConflict.Ask, double timeoutSeconds = 10, int slotIndex = 0,
        bool acceptAxes = true, IEnumerable<string>? excludedSlots = null)
    {
        var args = SchemeArgs(scheme);
        args["Action"] = action;
        args["Binding"] = bindingGuid;
        AddRebindArgs(args, conflict, timeoutSeconds, slotIndex, acceptAxes, excludedSlots);
        return Execute("BeginRebind", args).Rebind;
    }

    public static InputRebindState Poll() => Execute("Poll").Rebind;
    public static InputRebindState Cancel() => Execute("Cancel").Rebind;
    public static InputRebindState Resolve(InputRebindConflict resolution) =>
        Execute("Resolve", new Dictionary<string, object?> { ["Resolution"] = resolution.ToString() }).Rebind;

    /// <summary>Removes the overrides of one action, or of all actions when action is null.</summary>
    public static InputBindingsReply Reset(string? action = null) =>
        Execute("Reset", action is null ? null : new Dictionary<string, object?> { ["Action"] = action });

    public static InputBindingsReply SetValue(string key, float value) =>
        Execute("SetValue", new Dictionary<string, object?> { ["Key"] = key, ["Value"] = value });

    public static InputBindingsReply Save() => Execute("Save");

    /// <summary>The display prompt of an action; Any uses the player's active scheme.</summary>
    public static string GetPrompt(string action, InputBindingScheme scheme = InputBindingScheme.Any)
    {
        var args = SchemeArgs(scheme);
        args["Action"] = action;
        return Execute("Prompt", args).Prompt;
    }

    public static void PushContext(string context) => Execute("PushContext", new Dictionary<string, object?> { ["Context"] = context });
    public static void PopContext(string context) => Execute("PopContext", new Dictionary<string, object?> { ["Context"] = context });

    /// <summary>Runs a raw plInputBindings command; args is serialized to a JSON object.</summary>
    public static InputBindingsReply Execute(string command, object? args = null)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(args ?? new { });
        uint error = Native(command, json, (uint)json.Length, null, 0, out uint size);
        if (error != 0) throw new InvalidOperationException($"Input bindings bridge failed: {error}");
        if (size > 4 * 1024 * 1024) throw new InvalidOperationException("Input bindings response exceeded limit.");
        byte[] output = new byte[size];
        error = Native(null, null, 0, output, size, out _);
        if (error != 0) throw new InvalidOperationException($"Input bindings response failed: {error}");
        return JsonSerializer.Deserialize<InputBindingsReply>(output.AsSpan(0, (int)size - 1), Options)!;
    }

    private static Dictionary<string, object?> SchemeArgs(InputBindingScheme scheme)
    {
        var args = new Dictionary<string, object?>();
        if (scheme != InputBindingScheme.Any) args["Scheme"] = scheme.ToString();
        return args;
    }

    private static void AddRebindArgs(Dictionary<string, object?> args, InputRebindConflict conflict, double timeoutSeconds,
        int slotIndex, bool acceptAxes, IEnumerable<string>? excludedSlots)
    {
        args["Conflict"] = conflict.ToString();
        args["Timeout"] = timeoutSeconds;
        args["SlotIndex"] = slotIndex;
        args["AcceptAxes"] = acceptAxes;
        if (excludedSlots is not null) args["Excluded"] = excludedSlots.ToArray();
    }

    [DllImport("plCSharpPlugin", EntryPoint = "plInputBindingsManagedCommand", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint Native([MarshalAs(UnmanagedType.LPUTF8Str)] string? command,
        byte[]? json, uint size, [Out] byte[]? output, uint capacity, out uint required);
}
