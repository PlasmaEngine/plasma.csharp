namespace Plasma;

/// <summary>
/// Opts a script into private-state migration on full assembly reload. State is a versioned,
/// UTF-8 JSON string owned by the script, limited to one MiB. Capture must be side-effect free.
/// Restore runs on the replacement before Initialize; a failure retains the previous instance.
/// Store gameplay values and logical references, never transient native handles or delegates.
/// In-place method updates retain the instance and do not call either method.
/// </summary>
public interface IScriptReloadState
{
    string CaptureReloadState();
    void RestoreReloadState(string state);
}
