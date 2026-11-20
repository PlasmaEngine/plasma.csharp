using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Plasma.ManagedHost;

public static unsafe partial class Bootstrap
{
    [StructLayout(LayoutKind.Sequential)]
    private struct ReloadStateApi
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ulong, CSharpBuffer*, CSharpStatus> Capture;
        public delegate* unmanaged[Cdecl]<ulong, CSharpUtf8Span, CSharpStatus> Restore;
    }
    private static ReloadStateApi* s_reloadStateApi;
    private static void* GetReloadStateApi()
    {
        lock (Sync)
        {
            if (s_reloadStateApi is null)
            {
                s_reloadStateApi = (ReloadStateApi*)NativeMemory.AllocZeroed((nuint)sizeof(ReloadStateApi));
                s_reloadStateApi->Size = (uint)sizeof(ReloadStateApi); s_reloadStateApi->Version = 1;
                s_reloadStateApi->Capture = &CaptureReloadStateExport;
                s_reloadStateApi->Restore = &RestoreReloadStateExport;
            }
            return s_reloadStateApi;
        }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus CaptureReloadStateExport(ulong id, CSharpBuffer* buffer)
    {
        using var call = new RuntimeCall();
        try
        {
            if (buffer is null) return CSharpStatus.InvalidArgument;
            *buffer = default;
            if (!TryGetInstance(id, out Instance instance, out CSharpStatus status)) return status;
            using ScriptExecutionContext.Scope scope = ScriptExecutionContext.Enter(instance.Script.World);
            string? captured = instance.Script is IScriptReloadState state
                ? state.CaptureReloadState()
                : KeptMembers.Capture(instance.Script);
            if (captured is null) return CSharpStatus.Unsupported;
            byte[] json = Encoding.UTF8.GetBytes(captured);
            if (json.Length > 1024 * 1024) throw new InvalidDataException("C# reload state exceeds one MiB.");
            using var validated = JsonDocument.Parse(json);
            return AllocateBuffer(json, buffer);
        }
        catch (Exception exception) { return Fail(CSharpStatus.ManagedException, exception); }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus RestoreReloadStateExport(ulong id, CSharpUtf8Span json)
    {
        using var call = new RuntimeCall();
        try
        {
            if (json.Length > 1024 * 1024 || (json.Data is null && json.Length != 0)) return CSharpStatus.InvalidArgument;
            if (!TryGetInstance(id, out Instance instance, out CSharpStatus status)) return status;
            // The new code keeps no state: the old state is dropped rather than refusing the reload.
            if (instance.Script is not IScriptReloadState && !KeptMembers.Has(instance.Script.GetType())) return CSharpStatus.Success;
            string text = Decode(json);
            using var validated = JsonDocument.Parse(text);
            using ScriptExecutionContext.Scope scope = ScriptExecutionContext.Enter(instance.Script.World);
            if (instance.Script is IScriptReloadState state)
                state.RestoreReloadState(text);
            else
                KeptMembers.Restore(instance.Script, validated.RootElement);
            return CSharpStatus.Success;
        }
        catch (Exception exception) { return Fail(CSharpStatus.ManagedException, exception); }
    }
}
