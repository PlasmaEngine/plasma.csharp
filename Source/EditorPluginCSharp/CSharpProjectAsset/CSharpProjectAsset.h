#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectDescriptors.h>

class plObjectCommandAccessor;
class plExposedParameters;

class PL_EDITORPLUGINCSHARP_DLL plCSharpProjectAssetMetaData : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpProjectAssetMetaData, plReflectedClass);

public:
  plString m_sDescriptorJson;
  plString m_sProjectFile;
  plString m_sSourceRoot;
  plDynamicArray<plString> m_LastGoodSources;
  plDynamicArray<plUInt64> m_LastGoodSourceHashes;
};

class PL_EDITORPLUGINCSHARP_DLL plCSharpProjectAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpProjectAssetProperties, plReflectedClass);

public:
  plString m_sProjectFile;
  plString m_sSourceRoot = "Scripts";
  plString m_sBuildConfiguration = "Development";
  plString m_sBindingManifest = "Intermediate/PlasmaBindings.json";

  plString m_sLastGoodDescriptorJson;
  plDynamicArray<plString> m_LastGoodSources;
  plDynamicArray<plUInt64> m_LastGoodSourceHashes;
  plDynamicArray<plString> m_LastGoodFiles;
  plString m_sLastGoodEntryAssembly;
  plUInt64 m_uiLastGoodBindingSchemaHash = 0;
  /// Fingerprint of the .csproj that produced m_sLastGoodEntryAssembly. While it matches, the target
  /// assembly name is reused instead of asking MSBuild for it in a second process.
  plUInt64 m_uiLastGoodProjectFileHash = 0;
  /// Fingerprint of the assembly m_sLastGoodDescriptorJson was extracted from. While it matches, the
  /// descriptor inspector does not have to run.
  plUInt64 m_uiLastGoodEntryAssemblyHash = 0;
  plString m_sBuildDiagnostics;
};

class PL_EDITORPLUGINCSHARP_DLL plCSharpProjectAssetDocument : public plSimpleAssetDocument<plCSharpProjectAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpProjectAssetDocument, plSimpleAssetDocument<plCSharpProjectAssetProperties>);

public:
  plCSharpProjectAssetDocument(plStringView sDocumentPath);

  plTransformStatus BuildProject();
  void OpenProject();
  void OpenScript();
  void OpenScript(plStringView sScriptFile);
  plStatus OpenScriptInIDE(plStringView sScriptFile);
  void CreateScript();

  static plStatus UpdateBindingSchemaStamp(bool* out_pWasWritten = nullptr);
  static plStatus UpdateSourceDependencyStamp(bool* out_pWasWritten = nullptr);
  static void InvalidateBindingSchemaCache();
  static plStatus GetSourceDependencyStampPath(plStringBuilder& out_sPath);
  static plExposedParameters* CreateExposedParameters(
    const plCSharpClassDescriptor& classDescriptor, const plUuid& assetGuid);

protected:
  virtual void InitializeAfterLoading(bool bFirstTimeCreation) override;
  virtual plTransformStatus InternalTransformAsset(const char* szTargetFile, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;

private:
  struct BuildFile
  {
    plString m_sRelativePath;
    plDynamicArray<plUInt8> m_Data;
  };

  plStatus ResolveProjectFile(plStringBuilder& out_sProjectFile) const;
  plStatus ResolveSourceRoot(plStringView sProjectFile, plStringBuilder& out_sSourceRoot) const;
  plStatus ResolveBindingManifest(plStringView sProjectFile, plStringBuilder& out_sBindingManifest) const;
  plStatus ExportBindingManifest(plStringView sBindingManifest, plUInt64& out_uiSchemaHash) const;
  plStatus RunProjectBuild(plStringView sProjectFile, plStringView sBindingManifest,
    plStringView sOutputDirectory, plStringBuilder& out_sDiagnostics) const;
  /// Resolves the built assembly. \a sCachedTargetFileName short-circuits the MSBuild property query
  /// when the project file has not changed since it produced that name.
  plStatus FindEntryAssembly(plStringView sProjectFile, plStringView sOutputDirectory,
    plStringView sCachedTargetFileName, plStringBuilder& out_sEntryAssembly) const;
  plStatus HashFileContents(plStringView sFile, plUInt64& out_uiHash) const;
  plStatus RunInspector(plStringView sEntryAssembly, plStringView sDescriptorFile, plStringBuilder& inout_sDiagnostics) const;
  plStatus GatherBuildFiles(plStringView sOutputDirectory, plDynamicArray<BuildFile>& out_files, plDynamicArray<plString>& out_relativeFiles) const;
  plStatus GatherSourceFiles(plStringView sSourceRoot, plDynamicArray<plString>& out_relativeSources) const;
  plStatus GatherSourceHashes(plStringView sSourceRoot, const plDynamicArray<plString>& sourceFiles,
    plDynamicArray<plUInt64>& out_sourceHashes) const;
  plStatus ReadTextFile(plStringView sFile, plStringBuilder& out_sText) const;
  plStatus WriteTextFile(plStringView sFile, plStringView sText) const;
  plStatus CreateScriptFile(plStringView sSourceRoot, plStringView sClassName, plStringBuilder& out_sScriptFile) const;
  plStatus CreateInitialProject();
  static plStatus GetBindingSchemaStampPath(plStringBuilder& out_sPath);
  plUInt64 ComputeTransformSettingsHash() const;
  plStatus UpdateBuildCache(plObjectCommandAccessor& accessor, plStringView sDescriptorJson, const plDynamicArray<plString>& sourceFiles,
    const plDynamicArray<plUInt64>& sourceHashes, const plDynamicArray<plString>& buildFiles, plStringView sEntryAssembly,
    plUInt64 uiSchemaHash, plUInt64 uiProjectFileHash, plUInt64 uiEntryAssemblyHash, plStringView sDiagnostics);
  void UpdateDiagnostics(plStringView sDiagnostics);
  void LogBuildOutput(plStringView sDiagnostics, bool bBuildFailed) const;

  plString m_sPreparedEntryAssembly;
  plUInt64 m_uiPreparedBindingSchemaHash = 0;
  plDynamicArray<BuildFile> m_PreparedFiles;
  plDynamicArray<plCSharpClassDescriptor> m_PreparedClasses;
};
