#pragma once

#include <EditorFramework/Assets/AssetDocumentManager.h>
#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>

class PL_EDITORPLUGINCSHARP_DLL plCSharpScriptAssetMetaData : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpScriptAssetMetaData, plReflectedClass);

public:
  plString m_sProjectAsset;
  plUuid m_ClassGuid;
};

class PL_EDITORPLUGINCSHARP_DLL plCSharpProjectAssetManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpProjectAssetManager, plAssetDocumentManager);

public:
  plCSharpProjectAssetManager();
  ~plCSharpProjectAssetManager();

  virtual OutputReliability GetAssetTypeOutputReliability() const override { return OutputReliability::Perfect; }
  virtual void FillOutSubAssetList(const plAssetDocumentInfo& assetInfo, plDynamicArray<plSubAssetData>& out_subAssets) const override;
  virtual plString GetAssetTableEntry(
    const plSubAsset* pSubAsset, plStringView sDataDirectory, const plPlatformProfile* pAssetProfile) const override;

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& event);

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument,
    plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(
    plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;
  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;

  /// The type of the class sub-assets. No file has it; it exists so asset fields and pickers that ask for a script class
  /// accept the classes inside a project.
  plAssetDocumentTypeDescriptor m_ClassTypeDesc;
};

/// \brief Source-backed C# asset type. Creation writes a .cs file; opening delegates to the managed project.
class PL_EDITORPLUGINCSHARP_DLL plCSharpScriptAssetManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpScriptAssetManager, plAssetDocumentManager);

public:
  plCSharpScriptAssetManager();

  virtual plStatus ReadAssetDocumentInfo(plUniquePtr<plAssetDocumentInfo>& out_pInfo, plStreamReader& inout_stream,
    plStringView sDocumentPath, const plFileStatus& fileStatus) const override;
  virtual bool IsOutputUpToDate(plStringView sDocumentPath, plStringView sOutputTag, plUInt64 uiHash,
    const plAssetDocumentTypeDescriptor* pTypeDescriptor) override;
  virtual plString GetAssetTableEntry(
    const plSubAsset* pSubAsset, plStringView sDataDirectory, const plPlatformProfile* pAssetProfile) const override;
  virtual plResult OpenDocumentExternally(plStringView sFilePath) override;
  virtual bool SupportsExternalDocumentCreation() const override { return true; }
  virtual plStatus CreateDocumentExternally(plStringView sFilePath) override;

private:
  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument,
    plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(
    plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;
  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
