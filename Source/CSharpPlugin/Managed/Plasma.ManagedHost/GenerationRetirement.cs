using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Plasma.ManagedHost;

public static unsafe partial class Bootstrap
{
    [StructLayout(LayoutKind.Sequential)]
    private struct GenerationApi
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ulong, CSharpStatus> Retire;
        public delegate* unmanaged[Cdecl]<uint*, uint*, CSharpStatus> GetStatus;
    }
    private static GenerationApi* s_generationApi;
    private sealed record RetiredGeneration(UnloadTicket Ticket, DateTime RetiredAt);
    private static readonly List<RetiredGeneration> Retired = [];
    private static Timer? s_retirementTimer;
    private static void* GetGenerationApi()
    {
        lock (Sync)
        {
            if (s_generationApi is null)
            {
                s_generationApi = (GenerationApi*)NativeMemory.AllocZeroed((nuint)sizeof(GenerationApi));
                s_generationApi->Size = (uint)sizeof(GenerationApi); s_generationApi->Version = 1;
                s_generationApi->Retire = &RetireGenerationExport;
                s_generationApi->GetStatus = &GetRetirementStatusExport;
            }
            return s_generationApi;
        }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus RetireGenerationExport(ulong id)
    {
        try
        {
            var ticket = BeginUnload(id, out _, out CSharpStatus status);
            if (ticket is null) return status;
            lock (Sync)
            {
                Retired.Add(new(ticket, DateTime.UtcNow));
                s_retirementTimer ??= new Timer(SweepRetired, null, TimeSpan.FromSeconds(5), TimeSpan.FromSeconds(5));
            }
            return CSharpStatus.Success;
        }
        catch (Exception exception) { return Fail(CSharpStatus.ManagedException, exception); }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus GetRetirementStatusExport(uint* pending, uint* overdue)
    {
        if (pending is null || overdue is null) return CSharpStatus.InvalidArgument;
        lock (Sync)
        {
            *pending = (uint)Retired.Count;
            *overdue = (uint)Retired.Count(t => DateTime.UtcNow - t.RetiredAt > TimeSpan.FromMinutes(1));
        }
        return CSharpStatus.Success;
    }
    // Runs on a timer thread: an exception escaping it would terminate the process.
    private static void SweepRetired(object? state)
    {
        try
        {
            SweepRetiredCore();
        }
        catch (Exception exception)
        {
            Log(CSharpLogLevel.Warning, $"Collecting retired C# generations failed: {exception.Message}");
        }
    }

    private static void SweepRetiredCore()
    {
        List<UnloadTicket> collected = [];
        bool requestCollection;
        lock (Sync)
        {
            for (int i = Retired.Count - 1; i >= 0; --i)
            {
                if (Retired[i].Ticket.ContextReference.IsAlive) continue;
                collected.Add(Retired[i].Ticket); Retired.RemoveAt(i);
            }
            requestCollection = Retired.Count >= 8 && Retired.Any(t => DateTime.UtcNow - t.RetiredAt > TimeSpan.FromSeconds(30));
            if (Retired.Count == 0) { s_retirementTimer?.Dispose(); s_retirementTimer = null; }
        }
        foreach (var ticket in collected) TryDeleteDirectory(ticket.ShadowDirectory);
        // One nonblocking request under pressure, never finalizer waits in resource loading.
        // Background GC can still pause managed threads; it is not a zero-cost operation.
        if (requestCollection) GC.Collect(2, GCCollectionMode.Optimized, blocking: false, compacting: false);
    }
}
