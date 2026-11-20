using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Plasma;

public enum GameSettingsStatus
{
    Ready, Queued, AwaitingConfirmation, Applied, Reverted,
    InvalidValue, Unsupported, Busy, BackendFailure, SaveFailure
}

/// <summary>A complete settings draft. Keys and supported values are returned by Describe().</summary>
public sealed class GameSettingsDraft
{
    public Dictionary<string, object?> Values { get; } = [];
    public void SetQualityPreset(string id)
    {
        Values["Quality.Preset"] = id;
        Values["Quality.Overrides"] = new Dictionary<string, object>();
    }
    public void SetQualityOverride(string cvar, object value)
    {
        if (Values.GetValueOrDefault("Quality.Overrides") is not Dictionary<string, object?> overrides)
        {
            overrides = [];
            if (Values.GetValueOrDefault("Quality.Overrides") is JsonElement json)
                foreach (var field in json.EnumerateObject()) overrides[field.Name] = field.Value.Clone();
            Values["Quality.Overrides"] = overrides;
        }
        overrides[cvar] = value;
    }
    public bool VSync { set => Values["Display.VSync"] = value; }
    public int FpsCap { set => Values["Display.FpsCap"] = value; }
    public float MasterVolume { set => Values["Audio.Master"] = value; }
}

public sealed class GameSettingsState
{
    public GameSettingsStatus Status { get; init; }
    public ulong Transaction { get; init; }
    public double SecondsRemaining { get; init; }
    public bool RestartRequired { get; init; }
    public string Message { get; init; } = "";
    public Dictionary<string, JsonElement> Values { get; init; } = [];
    public Dictionary<string, JsonElement> Confirmed { get; init; } = [];
    public Dictionary<string, JsonElement> Defaults { get; init; } = [];
    public JsonElement[] Presets { get; init; } = [];
    public JsonElement[] Descriptors { get; init; } = [];
    public JsonElement[] QualityDescriptors { get; init; } = [];
    public JsonElement[] DisplayModes { get; init; } = [];
}

/// <summary>Client of the application-owned service. Apply queues work at a safe native frame
/// boundary. Call Update from your script update to receive state events. Dispose on menu close
/// to cancel an owned display preview. The native timeout continues even without Update.</summary>
public sealed class GameSettings : IDisposable, IOwnedService
{
    private static readonly HashSet<GameSettings> Clients = [];
    private readonly ComponentScript? _owner;
    private readonly int _thread = Environment.CurrentManagedThreadId;
    private bool _disposed;
    private ulong _transaction;
    private GameSettingsStatus? _lastStatus;
    public event Action<GameSettingsState>? StateChanged;

    /// <summary>Created in a script callback, the client belongs to that script unless another <paramref name="owner"/>
    /// is given: the script calls <see cref="Update"/> each frame and disposes it when it goes away.</summary>
    public GameSettings(ComponentScript? owner = null)
    {
        _owner = owner ?? ComponentScript.Current;
        _owner?.Own(this);
        if (_owner is null)
            Clients.Add(this);
    }

    void IOwnedService.Pump() => Update();

    public GameSettingsState Describe() => Command("Query");
    public GameSettingsDraft CreateDraft() => Draft(Describe().Confirmed);
    public GameSettingsDraft ResetToDefaults(string category = "") => Draft(Command("Query", new { Category = category }).Defaults);
    public GameSettingsState Validate(GameSettingsDraft draft) => Command("Validate", new { Values = draft.Values });
    public GameSettingsState Apply(GameSettingsDraft draft)
    {
        var state = Command("Apply", new { Values = draft.Values });
        if (state.Status == GameSettingsStatus.Queued) _transaction = state.Transaction;
        return state;
    }
    public GameSettingsState Confirm() => Command("Confirm", new { Transaction = _transaction });
    public GameSettingsState Revert() => Command("Revert", new { Transaction = _transaction });
    public void Update()
    {
        var state = Describe();
        if (_lastStatus != state.Status || state.Status == GameSettingsStatus.AwaitingConfirmation)
        {
            _lastStatus = state.Status;
            StateChanged?.Invoke(state);
        }
    }
    public void Dispose()
    {
        if (_disposed) return;
        try { if (_transaction != 0) Revert(); }
        finally { _disposed = true; StateChanged = null; Clients.Remove(this); _owner?.Release(this); }
    }
    internal static void ResetAll()
    {
        foreach (var c in Clients.ToArray()) c.Dispose();
    }
    private static GameSettingsDraft Draft(Dictionary<string, JsonElement> values)
    {
        var draft = new GameSettingsDraft();
        foreach (var entry in values) draft.Values[entry.Key] = entry.Value.Clone();
        return draft;
    }
    private GameSettingsState Command(string command, object? args = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_thread != Environment.CurrentManagedThreadId) throw new InvalidOperationException("Use the settings client on its creating script thread.");
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(args ?? new { });
        uint error = Native(command, json, (uint)json.Length, null, 0, out uint size);
        if (error != 0) throw new InvalidOperationException($"Settings bridge failed: {error}");
        if (size > 4 * 1024 * 1024) throw new InvalidOperationException("Settings response exceeded limit.");
        byte[] output = new byte[size];
        error = Native(null, null, 0, output, size, out _);
        if (error != 0) throw new InvalidOperationException($"Settings response failed: {error}");
        return JsonSerializer.Deserialize<GameSettingsState>(output.AsSpan(0, (int)size - 1))!;
    }
    [DllImport("plCSharpPlugin", EntryPoint = "plGameSettingsManagedCommand", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint Native([MarshalAs(UnmanagedType.LPUTF8Str)] string? command,
        byte[]? json, uint size, [Out] byte[]? output, uint capacity, out uint required);
}
