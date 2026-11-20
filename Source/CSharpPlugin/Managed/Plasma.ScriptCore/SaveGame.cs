using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Plasma;

public enum SaveGameError : uint
{
    Success, InvalidArgument, NotFound, Busy, Cancelled, CorruptData,
    UnsupportedVersion, MissingContent, MigrationFailed, IoError, Conflict, LimitExceeded
}

public sealed class SaveGameException(SaveGameError error) : Exception($"Save/Load failed: {error}")
{
    public SaveGameError Error { get; } = error;
}

public sealed record SaveGameMetadata(
    Guid Profile, Guid Slot, Guid Revision, string Name, string Scene, string Project,
    string ContentVersion, uint Kind, long UtcMicroseconds, double PlayTimeSeconds);

public sealed class SaveGameResult
{
    public SaveGameError Status { get; init; }
    public Guid Revision { get; init; }
    public SaveGameMetadata[] Entries { get; init; } = [];
    public SaveGameMetadata[] Metadata { get; init; } = [];
    public string? ProfileJson { get; init; }
    public int[] Thumbnail { get; init; } = [];
}

/// <summary>
/// Main-thread client for the game-state-owned Save/Load service. Call Update each frame,
/// and Dispose during shutdown/script reload. Saving captures the world of the current script call.
/// Continuations are completed by Update on the engine thread; no worker touches managed game state.
/// </summary>
public sealed class SaveGame : IDisposable, IOwnedService
{
    private const string Library = "plCSharpPlugin";
    private readonly int _thread = Environment.CurrentManagedThreadId;
    private ulong _session;
    private readonly ComponentScript? _owner;
    private static readonly HashSet<SaveGame> Clients = [];
    private readonly Dictionary<ulong, Pending> _pending = [];
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    private sealed class Pending
    {
        internal readonly TaskCompletionSource<SaveGameResult> Completion = new();
        internal CancellationTokenRegistration Registration;
        internal int Cancel;
    }

    /// <summary>Gets a client for the active game state's initialized save service.</summary>
    public static SaveGame FromGameState(ComponentScript? owner = null) => new("", owner);

    /// <summary>Opens a client. Created in a script callback, it belongs to that script unless another
    /// <paramref name="owner"/> is given: the script calls <see cref="Update"/> each frame and disposes it.</summary>
    public SaveGame(string project, ComponentScript? owner = null)
    {
        Check(NativeOpen(project, out _session));
        _owner = owner ?? ComponentScript.Current;
        _owner?.Own(this);
        if (_owner is null)
            Clients.Add(this);
    }

    void IOwnedService.Pump() => Update();

    internal static void ResetAll()
    {
        foreach (var client in Clients.ToArray()) client.Dispose();
    }

    /// <summary>Opt in named public script fields under a stable, versioned native graph schema.</summary>
    public void RegisterScript(string schema, uint version, params string[] fields)
    {
        VerifyThread();
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(new { command = "registerScript", schema, version, fields });
        Check(NativeRequest(_session, json, (uint)json.Length, out _));
    }

    public Task<SaveGameResult> SaveAsync(Guid profile, Guid slot, string name, string scene, string contentVersion,
        Guid expectedRevision = default, double playTimeSeconds = 0, CancellationToken cancellation = default, byte[]? thumbnailPng = null)
        => Request(new { command = "save", profile, slot, name, scene, contentVersion, expectedRevision, playTimeSeconds, thumbnailHex = thumbnailPng is null ? "" : Convert.ToHexString(thumbnailPng) }, cancellation);

    public Task<SaveGameResult> CheckpointAsync(Guid profile, uint ringIndex, string scene, string contentVersion,
        Guid expectedRevision = default, string checkpoint = "", CancellationToken cancellation = default)
        => Request(new { command = "checkpoint", profile, ringIndex, scene, contentVersion, expectedRevision, checkpoint }, cancellation);

    public Task<SaveGameResult> LoadAsync(Guid profile, Guid slot, string contentVersion, CancellationToken cancellation = default)
        => Request(new { command = "load", profile, slot, contentVersion }, cancellation);
    public Task<SaveGameResult> EnumerateSlotsAsync(Guid profile, CancellationToken cancellation = default)
        => Request(new { command = "list", profile }, cancellation);
    public Task<SaveGameResult> EnumerateProfilesAsync(CancellationToken cancellation = default)
        => Request(new { command = "profiles" }, cancellation);
    public Task<SaveGameResult> ReadMetadataAsync(Guid profile, Guid slot, CancellationToken cancellation = default)
        => Request(new { command = "metadata", profile, slot }, cancellation);
    public Task<SaveGameResult> ReadThumbnailAsync(Guid profile, Guid slot, CancellationToken cancellation = default)
        => Request(new { command = "thumbnail", profile, slot }, cancellation);
    public Task<SaveGameResult> DeleteSlotAsync(Guid profile, Guid slot, Guid expectedRevision, CancellationToken cancellation = default)
        => Request(new { command = "delete", profile, slot, expectedRevision }, cancellation);
    public Task<SaveGameResult> SaveProfileAsync<T>(Guid profile, string name, T data, Guid expectedRevision = default, CancellationToken cancellation = default)
        => Request(new { command = "saveProfile", profile, name, profileJson = JsonSerializer.Serialize(data), expectedRevision }, cancellation);
    public Task<SaveGameResult> LoadProfileAsync(Guid profile, CancellationToken cancellation = default)
        => Request(new { command = "load", profile }, cancellation);

    private Task<SaveGameResult> Request(object request, CancellationToken cancellation)
    {
        VerifyThread();
        if (cancellation.IsCancellationRequested) return Task.FromCanceled<SaveGameResult>(cancellation);
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(request);
        Check(NativeRequest(_session, json, checked((uint)json.Length), out ulong job));
        Pending pending = new();
        pending.Registration = cancellation.Register(() => Interlocked.Exchange(ref pending.Cancel, 1));
        _pending.Add(job, pending);
        return pending.Completion.Task;
    }

    public void Update()
    {
        VerifyThread();
        foreach (var (job, pending) in _pending.ToArray())
        {
            if (_session == 0) break; // A completed continuation may dispose this service.
            if (Volatile.Read(ref pending.Cancel) != 0) NativeCancel(_session, job);
            SaveGameError status = NativePoll(_session, job, null, 0, out uint size);
            if (status == SaveGameError.Busy) continue;
            _pending.Remove(job);
            pending.Registration.Dispose();
            try
            {
                Check(status);
                if (size == 0 || size > 8 * 1024 * 1024) throw new SaveGameException(SaveGameError.LimitExceeded);
                byte[] output = new byte[size];
                Check(NativePoll(_session, job, output, size, out _));
                var result = JsonSerializer.Deserialize<SaveGameResult>(output.AsSpan(0, output.Length - 1), JsonOptions)
                    ?? throw new SaveGameException(SaveGameError.CorruptData);
                if (result.Status == SaveGameError.Cancelled) pending.Completion.TrySetCanceled();
                else { Check(result.Status); pending.Completion.TrySetResult(result); }
            }
            catch (Exception error) { pending.Completion.TrySetException(error); }
        }
    }

    public void Dispose()
    {
        if (_session == 0) return;
        VerifyThread();
        NativeClose(_session);
        _session = 0;
        Clients.Remove(this);
        _owner?.Release(this);
        var pending = _pending.Values.ToArray(); _pending.Clear();
        foreach (var job in pending) { job.Registration.Dispose(); job.Completion.TrySetCanceled(); }
    }
    private void VerifyThread()
    {
        ObjectDisposedException.ThrowIf(_session == 0, this);
        if (Environment.CurrentManagedThreadId != _thread) throw new InvalidOperationException("SaveGame must be used on its engine thread.");
    }
    private static void Check(SaveGameError error) { if (error != SaveGameError.Success) throw new SaveGameException(error); }

    [DllImport(Library, EntryPoint = "plSaveGameManagedOpen", CallingConvention = CallingConvention.Cdecl)]
    private static extern SaveGameError NativeOpen([MarshalAs(UnmanagedType.LPUTF8Str)] string project, out ulong session);
    [DllImport(Library, EntryPoint = "plSaveGameManagedClose", CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeClose(ulong session);
    [DllImport(Library, EntryPoint = "plSaveGameManagedRequest", CallingConvention = CallingConvention.Cdecl)]
    private static extern SaveGameError NativeRequest(ulong session, byte[] json, uint size, out ulong job);
    [DllImport(Library, EntryPoint = "plSaveGameManagedCancel", CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeCancel(ulong session, ulong job);
    [DllImport(Library, EntryPoint = "plSaveGameManagedPoll", CallingConvention = CallingConvention.Cdecl)]
    private static extern SaveGameError NativePoll(ulong session, ulong job, [Out] byte[]? output, uint capacity, out uint required);
}
