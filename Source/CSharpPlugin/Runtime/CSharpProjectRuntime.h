#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/Mutex.h>

struct plCSharpGenerationLease
{
  plString m_sAssemblyPath;
  plUInt64 m_uiGeneration = 0;

  bool IsValid() const { return m_uiGeneration != 0; }
};

/// Shares one collectible AssemblyLoadContext across all script-class resources from a project build.
class PL_CSHARPPLUGIN_DLL plCSharpProjectRuntime
{
public:
  static plCSharpProjectRuntime& GetSingleton();

  plResult AcquireGeneration(plStringView sAssemblyPath, plStringView sShadowCopyRoot,
    plCSharpGenerationLease& out_lease, plString& out_sDescriptorJson);
  void ReleaseGeneration(plCSharpGenerationLease& inout_lease);

  const plCSharpManagedApiV1* GetManagedApi() const;

private:
  struct Generation
  {
    plUInt64 m_uiId = 0;
    plUInt32 m_uiRefCount = 0;
    plString m_sDescriptorJson;
  };

  mutable plMutex m_Mutex;
  plHashTable<plString, Generation> m_Generations;
  plUInt32 m_uiIncompleteUnloadCount = 0;
};
