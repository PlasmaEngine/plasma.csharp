using System.Diagnostics;
using System.Reflection.Metadata;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Plasma.ManagedHost;

public static unsafe partial class Bootstrap
{
    [StructLayout(LayoutKind.Sequential)]
    private struct HotReloadApi
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<CSharpGenerationLoadDesc*, ulong*, CSharpStatus> TryUpdate;
    }
    private static HotReloadApi* s_hotReloadApi;
    private static int s_activeCalls;
    private readonly struct RuntimeCall : IDisposable
    {
        public RuntimeCall() { lock (Sync) ++s_activeCalls; }
        public void Dispose() { lock (Sync) --s_activeCalls; }
    }
    private static void* GetHotReloadApi()
    {
        lock (Sync)
        {
            if (s_hotReloadApi is null)
            {
                s_hotReloadApi = (HotReloadApi*)NativeMemory.AllocZeroed((nuint)sizeof(HotReloadApi));
                s_hotReloadApi->Size = (uint)sizeof(HotReloadApi);
                s_hotReloadApi->Version = 1;
                s_hotReloadApi->TryUpdate = &TryUpdateExport;
            }
            return s_hotReloadApi;
        }
    }
    private sealed record CodeDelta(byte[] Metadata, byte[] IL, byte[] Pdb);
    private sealed record CodeUpdate(Guid BaseMvid, CodeDelta[] Deltas);
    private sealed record CodeUpdates(int Version, Guid CurrentMvid, CodeUpdate[] Chains, string? FullReason = null);

    // A full reload's reason as "CODE|sentence" in the last error; the native runtime shows it in the editor's Live view.
    private static CSharpStatus FullReload(string codeAndReason) => Fail(CSharpStatus.Unsupported, codeAndReason);

    private static string DeltaFingerprint(CodeDelta delta)
    {
        using var hash = System.Security.Cryptography.IncrementalHash.CreateHash(System.Security.Cryptography.HashAlgorithmName.SHA256);
        foreach (byte[] bytes in new[] { delta.Metadata, delta.IL, delta.Pdb })
        {
            hash.AppendData(BitConverter.GetBytes(bytes.Length));
            hash.AppendData(bytes);
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    private static string DependencyFingerprint(string assemblyPath)
    {
        string root = Path.GetDirectoryName(Path.GetFullPath(assemblyPath))!;
        using var hash = System.Security.Cryptography.IncrementalHash.CreateHash(System.Security.Cryptography.HashAlgorithmName.SHA256);
        foreach (string file in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            if (Path.GetExtension(file) is not (".dll" or ".json") ||
                Path.GetFullPath(file).Equals(Path.GetFullPath(assemblyPath), StringComparison.OrdinalIgnoreCase) ||
                Path.GetFileName(file) == "Plasma.HotReload.json") continue;
            byte[] name = System.Text.Encoding.UTF8.GetBytes(Path.GetRelativePath(root, file).Replace('\\', '/'));
            hash.AppendData(BitConverter.GetBytes(name.Length));
            hash.AppendData(name);
            using var stream = File.OpenRead(file);
            hash.AppendData(BitConverter.GetBytes(stream.Length));
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = stream.Read(buffer)) != 0) hash.AppendData(buffer.AsSpan(0, read));
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus TryUpdateExport(CSharpGenerationLoadDesc* desc, ulong* generation)
    {
        if (desc is null || generation is null || desc->Size < (uint)sizeof(CSharpGenerationLoadDesc) || desc->Version != 1)
            return CSharpStatus.InvalidArgument;
        *generation = 0;
        try
        {
            if (!MetadataUpdater.IsSupported) return FullReload("C-UNSUPPORTED|This .NET runtime does not apply in-place updates here.");
            if (Debugger.IsAttached) return FullReload("C-DEBUGGER|A managed debugger is attached, so in-place updates are off.");
            string assemblyPath = Decode(desc->AssemblyPath);
            string updatesPath = Path.Combine(Path.GetDirectoryName(assemblyPath)!, "Plasma.HotReload.json");
            if (!File.Exists(updatesPath) || new FileInfo(updatesPath).Length > 64 * 1024 * 1024)
                return FullReload("C-NO-DELTA|This build carries no in-place update (the standard build ran, or the update file is too large).");
            CodeUpdates? updates = JsonSerializer.Deserialize<CodeUpdates>(File.ReadAllText(updatesPath));
            if (updates is null || updates.Version != 1 || updates.Chains.Length > 8) return FullReload("C-NO-DELTA|The in-place update file is missing or invalid.");
            using var pe = new System.Reflection.PortableExecutable.PEReader(File.OpenRead(assemblyPath));
            MetadataReader metadata = pe.GetMetadataReader();
            if (metadata.GetGuid(metadata.GetModuleDefinition().Mvid) != updates.CurrentMvid)
                return CSharpStatus.InvalidArgument;
            string dependencies = DependencyFingerprint(assemblyPath);
            lock (Sync)
            {
                // Never patch while a lifecycle, message, property accessor, or console callback
                // is executing. Busy hosts take the ordinary full-generation reload path.
                if (s_activeCalls != 0) return FullReload("C-BUSY|A script call was running, so the update was not applied mid-call.");
                foreach (Generation current in Generations.Values)
                {
                    if (current.UpdateFailed || current.Assembly is null || current.DependencyFingerprint != dependencies) continue;
                    CodeUpdate? chain = updates.Chains.FirstOrDefault(c => c.BaseMvid == current.BaseMvid);
                    if (chain is null || chain.Deltas.Length < current.DeltaSequence || chain.Deltas.Length > 64) continue;
                    // A restarted compiler can emit a different chain from the same original PE.
                    // Sequence numbers alone cannot prove that our already-applied prefix matches.
                    string[] deltaHashes = chain.Deltas.Select(DeltaFingerprint).ToArray();
                    if (current.AppliedDeltaHashes.Count != current.DeltaSequence ||
                        !current.AppliedDeltaHashes.SequenceEqual(deltaHashes.Take(current.DeltaSequence))) continue;
                    try
                    {
                        for (int i = current.DeltaSequence; i < chain.Deltas.Length; ++i)
                        {
                            CodeDelta delta = chain.Deltas[i];
                            MetadataUpdater.ApplyUpdate(current.Assembly, delta.Metadata, delta.IL, delta.Pdb);
                            current.AppliedDeltaHashes.Add(deltaHashes[i]);
                            current.DeltaSequence = i + 1;
                        }
                        current.CurrentMvid = updates.CurrentMvid;
                        *generation = current.Id;
                        return CSharpStatus.Success;
                    }
                    catch (Exception exception)
                    {
                        // Applying metadata has no rollback. Do not reuse a partly updated context.
                        current.UpdateFailed = true;
                        return FullReload("C-APPLY-FAILED|The runtime rejected the in-place update: " + exception.Message);
                    }
                }
            }
            // No running generation has a chain: the compiler dropped it because this edit needs a new type.
            return FullReload(updates.FullReason ?? "C-BASELINE|No running generation matches the update's baseline (an earlier full reload or a restart).");
        }
        catch (Exception exception) { return FullReload("C-APPLY-FAILED|" + exception.Message); }
    }
}
