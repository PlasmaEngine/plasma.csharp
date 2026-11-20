using System.Runtime.InteropServices;
using System.Text.Json;

namespace Plasma;

/// <summary>Mirrors plPlatformError; the bridge reports these by name.</summary>
public enum PlatformError : uint
{
    Success, Unsupported, NotSignedIn, Offline, InvalidArgument, NotFound, Busy, Cancelled,
    Timeout, RateLimited, PermissionDenied, Conflict, LobbyFull, AlreadyInLobby, BackendFailure
}

public sealed class PlatformException(PlatformError error) : Exception($"Platform request failed: {error}")
{
    public PlatformError Error { get; } = error;
}

public enum LobbyVisibility { Public, FriendsOnly, Private, Invisible }
public enum LeaderboardWindow { Global, Friends, AroundUser }
public enum OverlayPage { Default, Friends, Community, Players, Settings, Achievements, Store, Profile, InviteDialog }
public enum FilterComparison { Equal, NotEqual, Less, Greater }

public sealed record PlatformUser(string Id, string Name, string SecondaryId, string Status, bool InSameGame);
public sealed record PlatformLobbyMember(PlatformUser User, IReadOnlyDictionary<string, string> Attributes);
public sealed record PlatformLobby(string Id, string Owner, uint MaxMembers, uint MemberCount, string Visibility,
    IReadOnlyDictionary<string, string> Attributes, PlatformLobbyMember[] Members);
public sealed record PlatformLeaderboardEntry(string UserId, string Name, int Rank, long Score, int[] Details);
public sealed record PlatformAchievement(string Id, bool Unlocked, uint Progress, uint Target, long UnlockTimeUtcMicroseconds);
public sealed record PlatformMatchmakingTicket(string Id, string State, string LobbyId);
/// <summary>One plPlatformEvent. Type is the native type name ("SignedIn", "LobbyMemberJoined", ...).</summary>
public sealed record PlatformEvent(string Type, string Id, string UserId, string UserName, string LobbyId, string Text,
    JsonElement Value, uint Extra, bool Flag);

public sealed record LobbyFilter(string Key, string Value, FilterComparison Comparison = FilterComparison.Equal, bool Numeric = false);

public sealed class LobbySearch
{
    public uint MaxResults { get; set; } = 20;
    public bool AvailableSlotsOnly { get; set; } = true;
    public List<LobbyFilter> Filters { get; } = [];
    public LobbySearch Where(string key, string value) { Filters.Add(new LobbyFilter(key, value)); return this; }
}

/// <summary>
/// Client of the application-owned platform hub (identity, achievements, stats, leaderboards, friends,
/// presence, lobbies, matchmaking, entitlements, overlay). Synchronous members answer from the hub's caches;
/// asynchronous ones return a Task that Update completes on the engine thread, so call Update every frame.
/// Events arrive through EventReceived while Update runs and someone is subscribed. Dispose on script teardown.
/// </summary>
public sealed class Platform : IDisposable, IOwnedService
{
    private const string Library = "plCSharpPlugin";
    private static readonly HashSet<Platform> Clients = [];
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    private readonly ComponentScript? _owner;
    private readonly int _thread = Environment.CurrentManagedThreadId;
    private readonly Dictionary<ulong, Pending> _pending = [];
    private bool _disposed;

    /// <summary>Raised from Update for every hub event taken since the last call.</summary>
    public event Action<PlatformEvent>? EventReceived;

    /// <summary>Created in a script callback, the client belongs to that script unless another <paramref name="owner"/>
    /// is given: the script calls <see cref="Update"/> each frame and disposes it when it goes away.</summary>
    public Platform(ComponentScript? owner = null)
    {
        _owner = owner ?? ComponentScript.Current;
        _owner?.Own(this);
        if (_owner is null)
            Clients.Add(this);
    }

    void IOwnedService.Pump() => Update();

    // ---------------------------------------------------------------- synchronous

    public string BackendId => Str(Command("Query"), "BackendId", "Backend");
    public bool IsOnline => Bool(Command("Query"), "Online", "IsOnline");
    public bool IsSignedIn => Bool(Command("Query"), "SignedIn", "IsSignedIn");
    /// <summary>The signed-in user, or null.</summary>
    public PlatformUser? LocalUser
    {
        get
        {
            var query = Command("Query");
            return Bool(query, "SignedIn", "IsSignedIn") && query.TryGetProperty("User", out var user) ? ParseUser(user) : null;
        }
    }

    /// <summary>True on the first unlock; false when already unlocked, unknown or unsupported.</summary>
    public bool UnlockAchievement(string id) => Status(Command("Achievements.Unlock", new { Id = id })) == PlatformError.Success;
    public bool IsAchievementUnlocked(string id) => GetAchievement(id)?.Unlocked ?? false;
    public PlatformAchievement? GetAchievement(string id) => ListAchievements().FirstOrDefault(a => a.Id == id);
    public PlatformAchievement[] ListAchievements() => Array(Command("Achievements.List"), ParseAchievement, "Achievements");
    public PlatformError SetAchievementProgress(string id, uint progress) => Status(Command("Achievements.Progress", new { Id = id, Progress = progress }));

    public PlatformError SetStat(string id, double value) => Status(Command("Stats.Set", new { Id = id, Value = value }));
    public PlatformError SetStat(string id, long value) => Status(Command("Stats.Set", new { Id = id, Value = value }));
    public PlatformError IncrementStat(string id, double delta = 1.0) => Status(Command("Stats.Increment", new { Id = id, Delta = delta }));
    public double GetStat(string id) => Num(Command("Stats.Get", new { Id = id }), "Value");
    public long GetStatInt(string id) => (long)GetStat(id);

    public PlatformError SetPresence(string key, string value) => Status(Command("Presence.Set", new { Key = key, Value = value }));
    public bool OwnsDlc(string id) => Bool(Command("Entitlements.Owns", new { Id = id }), "Owned", "Flag");
    public PlatformError ShowOverlay(OverlayPage page = OverlayPage.Default) => Status(Command("Overlay.Show", new { Page = page.ToString() }));

    /// <summary>The lobby the local user is in, or null.</summary>
    public PlatformLobby? CurrentLobby
    {
        get
        {
            var current = Command("Lobbies.Current");
            if (current.TryGetProperty("Lobby", out var lobby) && lobby.ValueKind == JsonValueKind.Object) return ParseLobby(lobby);
            return current.ValueKind == JsonValueKind.Object && current.TryGetProperty("Id", out _) ? ParseLobby(current) : null;
        }
    }
    public PlatformError SetLobbyAttribute(string key, string value) => Status(Command("Lobbies.SetAttribute", new { Key = key, Value = value }));

    // ---------------------------------------------------------------- asynchronous

    public Task<PlatformUser> SignInAsync(CancellationToken cancellation = default)
        => Request("Identity.SignIn", null, r => ParseUser(r.GetProperty("User")), cancellation);

    public Task<PlatformLobby[]> FindLobbiesAsync(LobbySearch search, CancellationToken cancellation = default)
        => Request("Lobbies.Find", new
        {
            search.MaxResults,
            search.AvailableSlotsOnly,
            Filters = search.Filters.Select(f => new { f.Key, f.Value, Comparison = f.Comparison.ToString(), f.Numeric }).ToArray()
        }, r => Array(r, ParseLobby, "Lobbies"), cancellation);

    public Task<PlatformLobby> CreateLobbyAsync(uint maxMembers, LobbyVisibility visibility = LobbyVisibility.Public,
        IReadOnlyDictionary<string, string>? attributes = null, CancellationToken cancellation = default)
        => Request("Lobbies.Create", new { MaxMembers = maxMembers, Visibility = visibility.ToString(), Attributes = attributes ?? new Dictionary<string, string>() },
            r => ParseLobby(r.GetProperty("Lobby")), cancellation);

    public Task<PlatformLobby> JoinLobbyAsync(string lobbyId, CancellationToken cancellation = default)
        => Request("Lobbies.Join", new { Lobby = lobbyId }, r => ParseLobby(r.GetProperty("Lobby")), cancellation);

    public Task LeaveLobbyAsync(CancellationToken cancellation = default)
        => Request("Lobbies.Leave", null, _ => true, cancellation);

    public Task<PlatformLeaderboardEntry[]> ReadLeaderboardAsync(string id, LeaderboardWindow window = LeaderboardWindow.Global,
        int rangeStart = 1, int rangeEnd = 10, CancellationToken cancellation = default)
        => Request("Leaderboards.Read", new { Id = id, Window = window.ToString(), RangeStart = rangeStart, RangeEnd = rangeEnd },
            r => Array(r, ParseLeaderboardEntry, "LeaderboardEntries", "Entries"), cancellation);

    public Task UploadScoreAsync(string id, long score, int[]? details = null, CancellationToken cancellation = default)
        => Request("Leaderboards.Upload", new { Id = id, Score = score, Details = details ?? [] }, _ => true, cancellation);

    /// <summary>Completes with the ticket once a lobby is found; the local user is in it. Cancellation asks the
    /// matcher to stop.</summary>
    public Task<PlatformMatchmakingTicket> MatchmakeAsync(uint players, IEnumerable<LobbyFilter>? filters = null, CancellationToken cancellation = default)
        => Request("Matchmaking.Start", new
        {
            Players = players,
            Filters = (filters ?? []).Select(f => new { f.Key, f.Value, Comparison = f.Comparison.ToString(), f.Numeric }).ToArray()
        }, r => ParseTicket(r.TryGetProperty("Ticket", out var t) ? t : r), cancellation, cancelCommand: "Matchmaking.Cancel");

    public Task<PlatformAchievement[]> RefreshAchievementsAsync(CancellationToken cancellation = default)
        => Request("Achievements.List", null, r => Array(r, ParseAchievement, "Achievements"), cancellation);

    public Task<PlatformUser[]> RefreshFriendsAsync(CancellationToken cancellation = default)
        => Request("Friends.List", null, r => Array(r, ParseUser, "Users", "Friends"), cancellation);

    public Task InviteAsync(string userId, CancellationToken cancellation = default)
        => Request("Sessions.Invite", new { User = userId }, _ => true, cancellation);

    // ---------------------------------------------------------------- frame pump

    /// <summary>Polls outstanding operations and takes queued events. Call once per frame on the script thread.</summary>
    public void Update()
    {
        VerifyThread();
        foreach (var (operation, pending) in _pending.ToArray())
        {
            if (_disposed) break; // A completed continuation may dispose this client.
            if (pending.CancelRequested)
            {
                pending.CancelRequested = false;
                if (pending.CancelCommand is { } cancel) Command(cancel);
            }
            JsonElement poll;
            try { poll = Command("Operation.Poll", new { Operation = operation }); }
            catch (Exception error) { Finish(operation, pending, () => pending.Fail(error)); continue; }
            if (!Bool(poll, "Finished")) continue;
            var status = Status(poll);
            Finish(operation, pending, () =>
            {
                if (status == PlatformError.Cancelled) pending.Cancel();
                else if (status != PlatformError.Success) pending.Fail(new PlatformException(status));
                else pending.Complete(poll.TryGetProperty("Result", out var result) ? result : poll);
            });
        }
        if (_disposed || EventReceived is null) return;
        var take = Command("Events.Take");
        if (!take.TryGetProperty("Events", out var events) || events.ValueKind != JsonValueKind.Array) return;
        foreach (var e in events.EnumerateArray()) EventReceived?.Invoke(ParseEvent(e));
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        EventReceived = null;
        Clients.Remove(this);
        _owner?.Release(this);
        var pending = _pending.Values.ToArray(); _pending.Clear();
        foreach (var p in pending) { p.Registration.Dispose(); p.Cancel(); }
    }
    internal static void ResetAll()
    {
        foreach (var c in Clients.ToArray()) c.Dispose();
    }

    // ---------------------------------------------------------------- operations

    private abstract class Pending
    {
        internal CancellationTokenRegistration Registration;
        internal volatile bool CancelRequested;
        internal string? CancelCommand;
        internal abstract void Complete(JsonElement result);
        internal abstract void Fail(Exception error);
        internal abstract void Cancel();
    }
    private sealed class Pending<T>(Func<JsonElement, T> parse) : Pending
    {
        internal readonly TaskCompletionSource<T> Completion = new();
        internal override void Complete(JsonElement result)
        {
            try { Completion.TrySetResult(parse(result)); }
            catch (Exception error) { Completion.TrySetException(error); }
        }
        internal override void Fail(Exception error) => Completion.TrySetException(error);
        internal override void Cancel() => Completion.TrySetCanceled();
    }

    /// <summary>Issues a bridge command. A response carrying an Operation id is polled from Update; any other
    /// response is treated as an immediate result.</summary>
    private Task<T> Request<T>(string command, object? args, Func<JsonElement, T> parse, CancellationToken cancellation, string? cancelCommand = null)
    {
        VerifyThread();
        if (cancellation.IsCancellationRequested) return Task.FromCanceled<T>(cancellation);
        var pending = new Pending<T>(parse) { CancelCommand = cancelCommand };
        JsonElement response;
        try { response = Command(command, args); }
        catch (Exception error) { return Task.FromException<T>(error); }
        if (response.TryGetProperty("Operation", out var op) && op.TryGetUInt64(out ulong operation) && operation != 0)
        {
            pending.Registration = cancellation.Register(() => pending.CancelRequested = true);
            _pending.Add(operation, pending);
            return pending.Completion.Task;
        }
        var status = Status(response);
        if (status != PlatformError.Success) pending.Fail(new PlatformException(status));
        else pending.Complete(response);
        return pending.Completion.Task;
    }

    private void Finish(ulong operation, Pending pending, Action settle)
    {
        _pending.Remove(operation);
        pending.Registration.Dispose();
        settle();
    }

    // ---------------------------------------------------------------- bridge

    private JsonElement Command(string command, object? args = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        VerifyThread();
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(args ?? new { });
        uint error = Native(command, json, (uint)json.Length, null, 0, out uint size);
        if (error == 2) throw new PlatformException(PlatformError.Unsupported);
        if (error != 0) throw new InvalidOperationException($"Platform bridge failed: {error}");
        if (size > 8 * 1024 * 1024) throw new InvalidOperationException("Platform response exceeded limit.");
        byte[] output = new byte[size];
        error = Native(null, null, 0, output, size, out _);
        if (error != 0) throw new InvalidOperationException($"Platform response failed: {error}");
        return JsonSerializer.Deserialize<JsonElement>(output.AsSpan(0, (int)size - 1), JsonOptions);
    }

    private void VerifyThread()
    {
        if (_thread != Environment.CurrentManagedThreadId) throw new InvalidOperationException("Use the platform client on its creating script thread.");
    }

    [DllImport(Library, EntryPoint = "plPlatformManagedCommand", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint Native([MarshalAs(UnmanagedType.LPUTF8Str)] string? command,
        byte[]? json, uint size, [Out] byte[]? output, uint capacity, out uint required);

    // ---------------------------------------------------------------- parsing

    private static PlatformError Status(JsonElement e)
    {
        if (!TryGet(e, out var status, "Status", "Error")) return PlatformError.Success;
        if (status.ValueKind == JsonValueKind.Number && status.TryGetUInt32(out uint code)) return (PlatformError)code;
        return status.ValueKind == JsonValueKind.String && Enum.TryParse<PlatformError>(status.GetString(), true, out var parsed) ? parsed : PlatformError.BackendFailure;
    }
    private static bool TryGet(JsonElement e, out JsonElement value, params string[] names)
    {
        value = default;
        if (e.ValueKind != JsonValueKind.Object) return false;
        foreach (var name in names) if (e.TryGetProperty(name, out value)) return true;
        return false;
    }
    private static string Str(JsonElement e, params string[] names)
        => TryGet(e, out var v, names) ? v.ValueKind switch { JsonValueKind.String => v.GetString() ?? "", JsonValueKind.Null => "", _ => v.ToString() } : "";
    private static bool Bool(JsonElement e, params string[] names)
        => TryGet(e, out var v, names) && v.ValueKind switch { JsonValueKind.True => true, JsonValueKind.Number => v.GetDouble() != 0, _ => false };
    private static double Num(JsonElement e, params string[] names)
        => TryGet(e, out var v, names) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;
    private static T[] Array<T>(JsonElement e, Func<JsonElement, T> parse, params string[] names)
        => TryGet(e, out var v, names) && v.ValueKind == JsonValueKind.Array ? v.EnumerateArray().Select(parse).ToArray() : [];
    private static Dictionary<string, string> Attributes(JsonElement e)
    {
        Dictionary<string, string> result = [];
        if (!TryGet(e, out var attributes, "Attributes")) return result;
        if (attributes.ValueKind == JsonValueKind.Object)
            foreach (var field in attributes.EnumerateObject()) result[field.Name] = Str(attributes, field.Name);
        else if (attributes.ValueKind == JsonValueKind.Array)
            foreach (var entry in attributes.EnumerateArray()) result[Str(entry, "Key")] = Str(entry, "Value");
        return result;
    }
    /// <summary>Accepts a user dictionary or a bare canonical id.</summary>
    private static PlatformUser ParseUser(JsonElement e)
        => e.ValueKind == JsonValueKind.String
            ? new PlatformUser(e.GetString() ?? "", "", "", "", false)
            : new PlatformUser(Str(e, "Id", "UserId"), Str(e, "DisplayName", "Name"), Str(e, "SecondaryId"), Str(e, "Status"), Bool(e, "InSameGame"));
    private static PlatformLobby ParseLobby(JsonElement e) => new(
        Str(e, "Id", "LobbyId"), Str(e, "Owner"), (uint)Num(e, "MaxMembers"), (uint)Num(e, "MemberCount"), Str(e, "Visibility"),
        Attributes(e), Array(e, m => new PlatformLobbyMember(TryGet(m, out var u, "User") ? ParseUser(u) : ParseUser(m), Attributes(m)), "Members"));
    private static PlatformLeaderboardEntry ParseLeaderboardEntry(JsonElement e) => new(
        TryGet(e, out var u, "User") && u.ValueKind == JsonValueKind.Object ? Str(u, "Id") : Str(e, "User", "UserId"),
        Str(e, "DisplayName", "Name"), (int)Num(e, "Rank"), (long)Num(e, "Score"),
        Array(e, d => d.ValueKind == JsonValueKind.Number ? d.GetInt32() : 0, "Details"));
    private static PlatformAchievement ParseAchievement(JsonElement e) => new(
        Str(e, "Id"), Bool(e, "Unlocked"), (uint)Num(e, "Progress"), (uint)Num(e, "Target", "ProgressTarget"), (long)Num(e, "UnlockTime", "UnlockTimeUtcMicroseconds"));
    private static PlatformMatchmakingTicket ParseTicket(JsonElement e) => new(
        Str(e, "Id", "Ticket"), Str(e, "State"), TryGet(e, out var l, "Lobby") && l.ValueKind == JsonValueKind.Object ? Str(l, "Id") : Str(e, "Lobby", "LobbyId"));
    private static PlatformEvent ParseEvent(JsonElement e)
    {
        string userId = "", userName = "";
        if (TryGet(e, out var user, "User", "UserId"))
        {
            var parsed = ParseUser(user);
            userId = parsed.Id;
            userName = parsed.Name;
        }
        string lobbyId = TryGet(e, out var lobby, "Lobby", "LobbyId") ? (lobby.ValueKind == JsonValueKind.Object ? Str(lobby, "Id") : Str(e, "Lobby", "LobbyId")) : "";
        return new PlatformEvent(Str(e, "Type"), Str(e, "Id"), userId, Str(e, "UserName", "DisplayName") is { Length: > 0 } n ? n : userName, lobbyId,
            Str(e, "Text"), TryGet(e, out var value, "Value") ? value.Clone() : default, (uint)Num(e, "Extra"), Bool(e, "Flag"));
    }
}
