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

  /// Fingerprints an assembly's contents. Roslyn builds deterministically, so unchanged sources
  /// produce the same bytes and therefore the same key.
  static plResult HashAssemblyContents(plStringView sAssemblyPath, plStringBuilder& out_sKey)
  {
    out_sKey.Clear();

    plOSFile file;
    if (file.Open(sAssemblyPath, plFileOpenMode::Read).Failed())
      return PL_FAILURE;

    plUInt64 uiHash = 0x9E3779B97F4A7C15ULL;
    plDynamicArray<plUInt8> buffer;
    buffer.SetCountUninitialized(64 * 1024);

    while (true)
    {
      const plUInt64 uiRead = file.Read(buffer.GetData(), buffer.GetCount());
      if (uiRead == 0)
        break;

      uiHash = plHashingUtils::xxHash64(buffer.GetData(), static_cast<size_t>(uiRead), uiHash);
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
    ++pExisting->m_uiRefCount;
    out_lease.m_sGenerationKey = generationKeyString;
    out_lease.m_uiGeneration = pExisting->m_uiId;
    out_sDescriptorJson = pExisting->m_sDescriptorJson;
    return PL_SUCCESS;
  }

  if (m_uiIncompleteUnloadCount >= 3)
  {
    plLog::Error(
      "C# scripting has three collectible generations that did not unload. "
      "Restart the editor engine before loading another script build.");
    return PL_FAILURE;
  }

  plCSharpGenerationLoadDesc loadDesc;
  loadDesc.m_AssemblyPath = MakeSpan(sAssemblyPath);
  loadDesc.m_ShadowCopyRoot = MakeSpan(sShadowCopyRoot);

  plUInt64 uiGeneration = 0;
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

void plCSharpProjectRuntime::ReleaseGeneration(plCSharpGenerationLease& inout_lease)
{
  if (!inout_lease.IsValid())
    return;

  plUInt64 uiGeneration = 0;
  {
    PL_LOCK(m_Mutex);

    Generation* pGeneration = nullptr;
    if (!m_Generations.TryGetValue(inout_lease.m_sGenerationKey, pGeneration) ||
        pGeneration == nullptr || pGeneration->m_uiId != inout_lease.m_uiGeneration)
    {
      inout_lease = {};
      return;
    }

    if (--pGeneration->m_uiRefCount > 0)
    {
      inout_lease = {};
      return;
    }

    uiGeneration = pGeneration->m_uiId;
    m_Generations.Remove(inout_lease.m_sGenerationKey);
  }

  inout_lease = {};

  // Strictly before the unload. A registered command holds a method id into the load context that is
  // about to be collected, and unlike a script instance it is reachable from a console keystroke at
  // any time. This also has to happen when the unload below fails.
  plCSharpConsoleRegistry::GetSingleton().UnregisterGeneration(uiGeneration);

  const plCSharpManagedApiV1* pApi = GetManagedApi();
  if (pApi == nullptr)
    return;

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
