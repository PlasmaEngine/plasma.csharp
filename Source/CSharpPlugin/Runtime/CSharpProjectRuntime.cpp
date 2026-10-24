#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Hosting/CSharpHost.h>
#include <CSharpPlugin/Runtime/CSharpConsoleRegistry.h>
#include <CSharpPlugin/Runtime/CSharpProjectRuntime.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/StringBuilder.h>

namespace
{
  static plCSharpUtf8Span MakeSpan(plStringView value)
  {
    plCSharpUtf8Span span;
    span.m_pData = value.GetStartPointer();
    span.m_uiLength = value.GetElementCount();
    return span;
  }

  /// Fingerprints the entry assembly and its managed/native dependency closure. Symbols do not
  /// affect execution, but changed helpers and runtime configuration must invalidate reuse.
  static plResult HashAssemblyContents(plStringView sAssemblyPath, plStringBuilder& out_sKey)
  {
    out_sKey.Clear();

    plUInt64 uiHash = 0x9E3779B97F4A7C15ULL;
    plDynamicArray<plUInt8> buffer;
    buffer.SetCountUninitialized(64 * 1024);

    plStringBuilder directory = sAssemblyPath;
    directory.PathParentDirectory();
    plDynamicArray<plString> dependencies;
    plFileSystemIterator iterator;
    for (iterator.StartSearch(directory, plFileSystemIteratorFlags::ReportFilesRecursive); iterator.IsValid(); iterator.Next())
    {
      plStringBuilder path = iterator.GetCurrentPath();
      path.AppendPath(iterator.GetStats().m_sName);
      if (path.HasExtension("dll") || path.HasExtension("json"))
        dependencies.PushBack(path);
    }
    dependencies.Sort();
    if (dependencies.IsEmpty() || !plOSFile::ExistsFile(sAssemblyPath))
      return PL_FAILURE;
    for (const plString& dependency : dependencies)
    {
      plStringBuilder relative = dependency;
      PL_SUCCEED_OR_RETURN(relative.MakeRelativeTo(directory));
      relative.ReplaceAll("\\", "/");
      const plUInt32 length = relative.GetElementCount();
      uiHash = plHashingUtils::xxHash64(&length, sizeof(length), uiHash);
      uiHash = plHashingUtils::xxHash64(relative.GetData(), length, uiHash);
      plOSFile file;
      PL_SUCCEED_OR_RETURN(file.Open(dependency, plFileOpenMode::Read));
      const plUInt64 size = file.GetFileSize();
      uiHash = plHashingUtils::xxHash64(&size, sizeof(size), uiHash);
      while (true)
      {
        const plUInt64 uiRead = file.Read(buffer.GetData(), buffer.GetCount());
        if (uiRead == 0)
          break;
        uiHash = plHashingUtils::xxHash64(buffer.GetData(), static_cast<size_t>(uiRead), uiHash);
      }
    }

    out_sKey.SetFormat("{}", plArgU(uiHash, 16, true, 16, true));
    return PL_SUCCESS;
  }
} // namespace

plCSharpProjectRuntime& plCSharpProjectRuntime::GetSingleton()
{
  static plCSharpProjectRuntime runtime;
  return runtime;
}

plResult plCSharpProjectRuntime::AcquireGeneration(
  plStringView sAssemblyPath, plStringView sShadowCopyRoot,
  plCSharpGenerationLease& out_lease, plString& out_sDescriptorJson)
{
  out_lease = {};
  out_sDescriptorJson.Clear();

  plCSharpHost* pHost = plCSharpHost::GetSingleton();
  if (pHost == nullptr || pHost->Initialize().Failed())
  {
    plLog::Error("The C# runtime host could not be initialized.");
    return PL_FAILURE;
  }

  const plCSharpManagedApiV1* pApi = pHost->GetManagedApi();
  if (pApi == nullptr)
    return PL_FAILURE;

  // Keyed on contents rather than path. The container is extracted to a directory named after its
  // asset hash, which moves whenever anything in the container changes - including a PDB or a
  // reordered deps.json. Keying on the path alone therefore swapped the load context, and destroyed
  // and recreated every live script instance, for rebuilds that produced exactly the same code.
  plStringBuilder generationKey;
  if (HashAssemblyContents(sAssemblyPath, generationKey).Failed())
  {
    plLog::Error("Could not fingerprint the C# assembly '{}'.", sAssemblyPath);
    return PL_FAILURE;
  }

  plString generationKeyString(generationKey);
  PL_LOCK(m_Mutex);

  Generation* pExisting = nullptr;
  if (m_Generations.TryGetValue(generationKeyString, pExisting) && pExisting != nullptr)
  {
    SetOutcome("C-SAME|The rebuild produced the same code; the running generation was kept.");
    ++pExisting->m_uiRefCount;
    out_lease.m_sGenerationKey = generationKeyString;
    out_lease.m_uiGeneration = pExisting->m_uiId;
    out_sDescriptorJson = pExisting->m_sDescriptorJson;
    return PL_SUCCESS;
  }

  plCSharpGenerationLoadDesc loadDesc;
  loadDesc.m_AssemblyPath = MakeSpan(sAssemblyPath);
  loadDesc.m_ShadowCopyRoot = MakeSpan(sShadowCopyRoot);
  void* pExtension = nullptr;
  if (pApi->m_QueryExtension(MakeSpan("Plasma.HotReload"), 1, &pExtension) == plCSharpStatus::Success && pExtension != nullptr)
  {
    const auto* pUpdates = static_cast<const plCSharpManagedHotReloadApiV1*>(pExtension);
    plUInt64 uiUpdated = 0;
    if (pUpdates->m_uiSize >= sizeof(plCSharpManagedHotReloadApiV1) && pUpdates->m_uiVersion == 1 &&
        pUpdates->m_TryUpdate != nullptr && pUpdates->m_TryUpdate(&loadDesc, &uiUpdated) == plCSharpStatus::Success)
    {
      for (auto it = m_Generations.GetIterator(); it.IsValid(); ++it)
      {
        if (it.Value().m_uiId != uiUpdated)
          continue;
        Generation updated = it.Value();
        ++updated.m_uiRefCount;
        const plString previousKey = it.Key();
        m_Generations.Remove(previousKey);
        m_Generations.Insert(generationKeyString, updated);
        out_lease.m_sGenerationKey = generationKeyString;
        out_lease.m_uiGeneration = uiUpdated;
        out_sDescriptorJson = updated.m_sDescriptorJson;
        plLog::Success("C# generation {} updated in place; live script state retained.", uiUpdated);
        SetOutcome("C-INPLACE|Only method bodies changed; applied as a metadata update. Constructors, field initializers and Initialize did not run.");
        return PL_SUCCESS;
      }
    }

    // The managed side names why it could not patch in place ("CODE|sentence").
    SetOutcome("C-FULL|The full assembly was loaded.");
    plCSharpBuffer error;
    if (pApi->m_GetLastError != nullptr && pApi->m_FreeBuffer != nullptr && pApi->m_GetLastError(&error) == plCSharpStatus::Success && error.m_pData != nullptr)
    {
      const plStringView sError(static_cast<const char*>(error.m_pData), error.m_uiSize);
      if (sError.StartsWith("C-"))
        SetOutcome(sError);
      pApi->m_FreeBuffer(&error);
    }
  }

  if (m_uiIncompleteUnloadCount >= 3)
  {
    plLog::Error(
      "C# scripting has three collectible generations that did not unload. "
      "Restart the editor engine before loading another script build.");
    SetOutcome("C-STUCK|Three script generations did not unload; restart the engine process to load another build.");
    return PL_FAILURE;
  }
  pExtension = nullptr;
  if (pApi->m_QueryExtension(MakeSpan("Plasma.Generation"), 1, &pExtension) == plCSharpStatus::Success && pExtension != nullptr)
  {
    const auto* pRetirement = static_cast<const plCSharpManagedGenerationApiV1*>(pExtension);
    plUInt32 pending = 0, overdue = 0;
    if (pRetirement->m_GetStatus(&pending, &overdue) == plCSharpStatus::Success && pending >= 16)
    {
      plLog::Error("C# reload is waiting for {} retired contexts ({} over one minute old). Retry after collection or restart the engine process.", pending, overdue);
      SetOutcome("C-RETIRED|Sixteen retired script generations are waiting for collection; the reload was refused until they are collected or the engine process restarts.");
      return PL_FAILURE;
    }
  }

  plUInt64 uiGeneration = 0;
  pExtension = nullptr;
  if (pApi->m_QueryExtension(MakeSpan("Plasma.ImmutableGenerations"), 1, &pExtension) == plCSharpStatus::Success)
    loadDesc.m_uiFlags = 1; // The resource backend validated and extracted this process-local, immutable container.
  plCSharpStatus status = pApi->m_LoadGeneration(&loadDesc, &uiGeneration);
  if (status != plCSharpStatus::Success)
  {
    pHost->LogManagedError("load script generation", status);
    return PL_FAILURE;
  }

  plCSharpBuffer descriptor;
  status = pApi->m_GetGenerationDescriptor(uiGeneration, &descriptor);
  if (status != plCSharpStatus::Success || descriptor.m_pData == nullptr)
  {
    pHost->LogManagedError("read script generation descriptor", status);
    plCSharpUnloadReport report;
    const plCSharpStatus unloadStatus =
      pApi->m_UnloadGeneration(uiGeneration, &report);
    if (unloadStatus != plCSharpStatus::Success ||
        report.m_bLoadContextAlive != 0)
    {
      ++m_uiIncompleteUnloadCount;
      pHost->LogManagedError(
        "clean up generation after descriptor failure", unloadStatus);
    }
    return PL_FAILURE;
  }

  plString descriptorJson(plStringView(
    static_cast<const char*>(descriptor.m_pData), descriptor.m_uiSize));
  pApi->m_FreeBuffer(&descriptor);

  Generation& generation = m_Generations[generationKeyString];
  generation.m_uiId = uiGeneration;
  generation.m_uiRefCount = 1;
  generation.m_sDescriptorJson = descriptorJson;

  plCSharpConsoleRegistry::GetSingleton().RegisterGeneration(uiGeneration, descriptorJson);

  plLog::Success("C# script generation {} loaded from '{}' (code {}).", uiGeneration,
    sAssemblyPath, generationKeyString);

  out_lease.m_sGenerationKey = generationKeyString;
  out_lease.m_uiGeneration = uiGeneration;
  out_sDescriptorJson = std::move(descriptorJson);
  return PL_SUCCESS;
}

void plCSharpProjectRuntime::SetOutcome(plStringView sCodeAndReason)
{
  const char* szBar = sCodeAndReason.FindSubString("|");
  if (szBar == nullptr)
  {
    m_sLastCode = sCodeAndReason;
    m_sLastReason.Clear();
    return;
  }

  m_sLastCode = plStringView(sCodeAndReason.GetStartPointer(), szBar);
  m_sLastReason = plStringView(szBar + 1, sCodeAndReason.GetEndPointer());
}

void plCSharpProjectRuntime::GetLastOutcome(plString& out_sCode, plString& out_sReason) const
{
  PL_LOCK(m_Mutex);
  out_sCode = m_sLastCode.GetView();
  out_sReason = m_sLastReason.GetView();
}

void plCSharpProjectRuntime::ReleaseGeneration(plCSharpGenerationLease& inout_lease)
{
  if (!inout_lease.IsValid())
    return;

  plUInt64 uiGeneration = 0;
  {
    PL_LOCK(m_Mutex);

    Generation* pGeneration = nullptr;
    // An in-place update changes the content key while existing class resources still own
    // leases to the preceding key. The generation id remains the ownership identity.
    plString currentKey = inout_lease.m_sGenerationKey;
    if (!m_Generations.TryGetValue(inout_lease.m_sGenerationKey, pGeneration) ||
        pGeneration == nullptr || pGeneration->m_uiId != inout_lease.m_uiGeneration)
    {
      pGeneration = nullptr;
      for (auto it = m_Generations.GetIterator(); it.IsValid(); ++it)
      {
        if (it.Value().m_uiId == inout_lease.m_uiGeneration)
        {
          pGeneration = &it.Value();
          currentKey = it.Key();
          break;
        }
      }
      if (pGeneration == nullptr)
      {
        inout_lease = {};
        return;
      }
    }

    if (--pGeneration->m_uiRefCount > 0)
    {
      inout_lease = {};
      return;
    }

    uiGeneration = pGeneration->m_uiId;
    m_Generations.Remove(currentKey);
    if (m_Generations.IsEmpty())
      m_Generations.Compact();
  }

  inout_lease = {};

  // Strictly before the unload. A registered command holds a method id into the load context that is
  // about to be collected, and unlike a script instance it is reachable from a console keystroke at
  // any time. This also has to happen when the unload below fails.
  plCSharpConsoleRegistry::GetSingleton().UnregisterGeneration(uiGeneration);

  const plCSharpManagedApiV1* pApi = GetManagedApi();
  if (pApi == nullptr)
    return;

  void* pExtension = nullptr;
  if (pApi->m_QueryExtension(MakeSpan("Plasma.Generation"), 1, &pExtension) == plCSharpStatus::Success && pExtension != nullptr)
  {
    const auto* pRetirement = static_cast<const plCSharpManagedGenerationApiV1*>(pExtension);
    if (pRetirement->m_Retire(uiGeneration) == plCSharpStatus::Success)
      return;
  }

  plCSharpUnloadReport report;
  const plCSharpStatus status = pApi->m_UnloadGeneration(uiGeneration, &report);
  if (status == plCSharpStatus::Success && report.m_bLoadContextAlive == 0)
    return;

  plUInt32 uiIncompleteUnloadCount = 0;
  {
    PL_LOCK(m_Mutex);
    ++m_uiIncompleteUnloadCount;
    uiIncompleteUnloadCount = m_uiIncompleteUnloadCount;
  }

  if (status == plCSharpStatus::UnloadIncomplete ||
      report.m_bLoadContextAlive != 0)
  {
    plLog::Error(
      "C# generation {} did not unload after {} GC cycles. Live managed instances: {}. "
      "Incomplete unload count: {}.",
      uiGeneration, report.m_uiGcCycles, report.m_uiLiveInstances,
      uiIncompleteUnloadCount);
  }
  else if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("unload script generation", status);

    plLog::Error(
      "C# generation {} could not be unloaded. Incomplete unload count: {}.",
      uiGeneration, uiIncompleteUnloadCount);
  }

  if (uiIncompleteUnloadCount >= 3)
  {
    plLog::Error(
      "C# hot reload has leaked three collectible generations. Restart the editor engine "
      "before loading further builds.");
  }
}

const plCSharpManagedApiV1* plCSharpProjectRuntime::GetManagedApi() const
{
  plCSharpHost* pHost = plCSharpHost::GetSingleton();
  return pHost != nullptr ? pHost->GetManagedApi() : nullptr;
}
