#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Runtime/CSharpInstance.h>
#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <Core/Scripting/ScriptClassResource.h>
#include <Foundation/Containers/Blob.h>
#include <Foundation/IO/MemoryStream.h>

class PL_CSHARPPLUGIN_DLL plCSharpClassResource : public plScriptClassResource
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpClassResource, plScriptClassResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plCSharpClassResource);

public:
  plCSharpClassResource();
  ~plCSharpClassResource();

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;
  virtual plUniquePtr<plScriptInstance> Instantiate(
    plReflectedClass& inout_owner, plWorld* pWorld) const override;

  plSharedPtr<const plCSharpClassData> m_pClassData;
};

/// Resolves "<shared-project-output>|<class-subasset-guid>" and loads the shared container.
class PL_CSHARPPLUGIN_DLL plCSharpClassResourceLoader : public plResourceTypeLoader
{
public:
  struct LoadedData
  {
    plBlob m_Storage;
    plRawMemoryStreamReader m_Reader;
  };

  virtual plResourceLoadData OpenDataStream(const plResource* pResource) override;
  virtual void CloseDataStream(
    const plResource* pResource, const plResourceLoadData& loaderData) override;
  virtual bool IsResourceOutdated(const plResource* pResource) const override;
};
