#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EditorApp/EditorApp.moc.h>
#include <EditorFramework/GUI/ExposedParameters.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAssetManager.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAssetWindow.moc.h>
#include <Foundation/IO/FileSystem/DeferredFileWriter.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>
#include <ToolsFoundation/FileSystem/FileSystemModel.h>
#include <ToolsFoundation/Project/ToolsProject.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpProjectAssetManager, 1, plRTTIDefaultAllocator<plCSharpProjectAssetManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpScriptAssetMetaData, 1, plRTTIDefaultAllocator<plCSharpScriptAssetMetaData>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ProjectAsset", m_sProjectAsset),
    PL_MEMBER_PROPERTY("ClassGuid", m_ClassGuid),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpScriptAssetManager, 1, plRTTIDefaultAllocator<plCSharpScriptAssetManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  struct ScriptProjectMatch
  {
    plString m_sProjectAsset;
    plUuid m_ProjectGuid;
    plUuid m_ClassGuid;
    plCSharpClassDescriptor m_ClassDescriptor;
    plUInt64 m_uiCompiledSourceHash = 0;
    bool m_bHasClass = false;
    bool m_bHasCompiledSourceHash = false;
    bool m_bProjectUpToDate = false;
  };

  bool GetAutomaticProjectAssetPath(plStringBuilder& out_sProjectAsset)
  {
    if (!plToolsProject::IsProjectOpen())
      return false;

    out_sProjectAsset = plToolsProject::GetSingleton()->GetProjectDirectory();
    out_sProjectAsset.AppendPath("CSharpProject.plCSharpProjectAsset");
    out_sProjectAsset.MakeCleanPath();
    return true;
  }

  void ResolveProjectFile(const plAssetInfo& assetInfo, const plCSharpProjectAssetMetaData& metadata, plStringBuilder& out_sProjectFile)
  {
    out_sProjectFile = metadata.m_sProjectFile;
    if (out_sProjectFile.IsEmpty())
    {
      out_sProjectFile = assetInfo.m_Path.GetAbsolutePath();
      out_sProjectFile.ChangeFileExtension("csproj");
    }
    if (!out_sProjectFile.IsAbsolutePath())
    {
      plStringBuilder documentDirectory = assetInfo.m_Path.GetAbsolutePath();
      documentDirectory.PathParentDirectory();
      out_sProjectFile.Prepend(documentDirectory, "/");
    }
    out_sProjectFile.MakeCleanPath();
  }

  bool ResolveSourceRoot(const plAssetInfo& assetInfo, const plCSharpProjectAssetMetaData& metadata, plStringBuilder& out_sSourceRoot)
  {
    plStringBuilder projectFile;
    ResolveProjectFile(assetInfo, metadata, projectFile);

    if (metadata.m_sSourceRoot.IsEmpty())
      out_sSourceRoot = "Scripts";
    else
      out_sSourceRoot = metadata.m_sSourceRoot;
    if (out_sSourceRoot == ".")
    {
      out_sSourceRoot = projectFile;
      out_sSourceRoot.PathParentDirectory();
    }
    else if (!out_sSourceRoot.IsAbsolutePath())
    {
      plStringBuilder projectDirectory = projectFile;
      projectDirectory.PathParentDirectory();
      out_sSourceRoot.Prepend(projectDirectory, "/");
    }
    out_sSourceRoot.MakeCleanPath();
    return true;
  }

  bool FindOwningProject(
    plStringView sScriptFile, ScriptProjectMatch& out_match)
  {
    plStringBuilder cleanScript(sScriptFile);
    cleanScript.MakeCleanPath();

    plStringBuilder automaticProjectAsset;
    if (!GetAutomaticProjectAssetPath(automaticProjectAsset))
      return false;

    const auto knownAssets = plAssetCurator::GetSingleton()->GetKnownAssets();
    for (auto it = knownAssets->GetIterator(); it.IsValid(); ++it)
    {
      const plAssetInfo* pInfo = it.Value();
      if (pInfo == nullptr ||
          pInfo->m_Info->m_sAssetsDocumentTypeName != plTempHashedString("C# Script Project") ||
          !pInfo->m_Path.GetAbsolutePath().IsEqual_NoCase(automaticProjectAsset))
      {
        continue;
      }

      const plCSharpProjectAssetMetaData* pMetadata =
        pInfo->m_Info->GetMetaInfo<plCSharpProjectAssetMetaData>();
      if (pMetadata == nullptr)
        continue;

      plStringBuilder sourceRoot;
      if (!ResolveSourceRoot(*pInfo, *pMetadata, sourceRoot) ||
          !plPathUtils::IsSubPath_NoCase(sourceRoot, cleanScript))
      {
        continue;
      }

      out_match.m_sProjectAsset = pInfo->m_Path.GetAbsolutePath();
      out_match.m_ProjectGuid = pInfo->m_Info->m_DocumentID;
      out_match.m_bProjectUpToDate =
        pInfo->m_TransformState == plAssetInfo::TransformState::UpToDate;

      plStringBuilder relativeSource = cleanScript;
      if (relativeSource.MakeRelativeTo(sourceRoot).Succeeded())
      {
        relativeSource.MakeCleanPath();
        relativeSource.ReplaceAll("\\", "/");
        const plUInt32 uiSourceCount = plMath::Min(
          pMetadata->m_LastGoodSources.GetCount(), pMetadata->m_LastGoodSourceHashes.GetCount());
        for (plUInt32 i = 0; i < uiSourceCount; ++i)
        {
          if (pMetadata->m_LastGoodSources[i].IsEqual_NoCase(relativeSource))
          {
            out_match.m_uiCompiledSourceHash = pMetadata->m_LastGoodSourceHashes[i];
            out_match.m_bHasCompiledSourceHash = true;
            break;
          }
        }
      }

      plString entryAssembly;
      plDynamicArray<plCSharpClassDescriptor> classes;
      if (plCSharpProjectDescriptors::Parse(
            pMetadata->m_sDescriptorJson, pInfo->m_Info->m_DocumentID, entryAssembly, classes)
            .Succeeded())
      {
        for (const plCSharpClassDescriptor& classDescriptor : classes)
        {
          plStringBuilder classSource(sourceRoot, "/", classDescriptor.m_sSourceFile);
          classSource.MakeCleanPath();
          bool bMatchesSource = !classDescriptor.m_sSourceFile.IsEmpty() &&
                                classSource.IsEqual_NoCase(cleanScript);

          if (!bMatchesSource &&
              !classDescriptor.m_sSourceFile.IsEmpty() &&
              plPathUtils::GetFileNameAndExtension(classDescriptor.m_sSourceFile)
                .IsEqual_NoCase(plPathUtils::GetFileNameAndExtension(cleanScript)))
          {
            // Older or hand-authored projects may not expose PlasmaSourceRoot to the
            // generator, in which case the descriptor retains only the filename.
            bMatchesSource = true;
          }

          if (!bMatchesSource && classDescriptor.m_sSourceFile.IsEmpty())
          {
            plStringView className = classDescriptor.m_sManagedName;
            if (const char* szDot = className.FindLastSubString("."))
              className = plStringView(szDot + 1, className.GetEndPointer());
            bMatchesSource = className.IsEqual_NoCase(
              plPathUtils::GetFileName(cleanScript));
          }

          if (bMatchesSource)
          {
            out_match.m_ClassGuid = classDescriptor.m_SubAssetGuid;
            out_match.m_ClassDescriptor = classDescriptor;
            out_match.m_bHasClass = true;
            break;
          }
        }
      }
      return true;
    }

    return false;
  }

  bool IsGeneratedProjectFile(plStringView sProjectFile)
  {
    plFileReader reader;
    if (reader.Open(sProjectFile).Failed())
      return false;

    plStringBuilder contents;
    contents.ReadAll(reader);
    return contents.FindSubString("<PlasmaSourceRoot>") != nullptr &&
           contents.FindSubString("Plasma.Engine.Generator.csproj") != nullptr;
  }

  void RemoveLegacyAutomaticProjects(plStringView sAutomaticProjectAsset)
  {
    struct LegacyProject
    {
      plString m_sAsset;
      plString m_sProjectFile;
    };

    plDynamicArray<LegacyProject> legacyProjects;
    const auto knownAssets = plAssetCurator::GetSingleton()->GetKnownAssets();
    for (auto it = knownAssets->GetIterator(); it.IsValid(); ++it)
    {
      const plAssetInfo* pInfo = it.Value();
      if (pInfo == nullptr ||
          pInfo->m_Info->m_sAssetsDocumentTypeName != plTempHashedString("C# Script Project") ||
          pInfo->m_Path.GetAbsolutePath().IsEqual_NoCase(sAutomaticProjectAsset))
      {
        continue;
      }

      LegacyProject& legacy = legacyProjects.ExpandAndGetRef();
      legacy.m_sAsset = pInfo->m_Path.GetAbsolutePath();
      if (const plCSharpProjectAssetMetaData* pMetadata =
            pInfo->m_Info->GetMetaInfo<plCSharpProjectAssetMetaData>())
      {
        plStringBuilder projectFile;
        ResolveProjectFile(*pInfo, *pMetadata, projectFile);
        legacy.m_sProjectFile = projectFile;
      }
    }

    for (const LegacyProject& legacy : legacyProjects)
    {
      plDocumentManager::EnsureDocumentIsClosedInAllManagers(legacy.m_sAsset);
      if (plOSFile::DeleteFile(legacy.m_sAsset).Succeeded())
      {
        plLog::Info("Removed legacy per-folder C# project asset '{}'.", legacy.m_sAsset);
        plAssetCurator::GetSingleton()->NotifyOfFileChange(legacy.m_sAsset);
      }

      if (!legacy.m_sProjectFile.IsEmpty() &&
          IsGeneratedProjectFile(legacy.m_sProjectFile) &&
          plOSFile::DeleteFile(legacy.m_sProjectFile).Succeeded())
      {
        plLog::Info("Removed legacy generated C# project '{}'.", legacy.m_sProjectFile);
        plAssetCurator::GetSingleton()->NotifyOfFileChange(legacy.m_sProjectFile);
      }
    }
  }

  plCSharpProjectAssetDocument* OpenOrCreateAutomaticProject()
  {
    plStringBuilder projectAsset;
    if (!GetAutomaticProjectAssetPath(projectAsset))
      return nullptr;

    plDocument* pDocument = nullptr;

    const plDocumentTypeDescriptor* pProjectType = nullptr;
    if (plDocumentManager::FindDocumentTypeFromPath(projectAsset, false, pProjectType).Succeeded())
      pDocument = pProjectType->m_pManager->GetDocumentByPath(projectAsset);

    if (pDocument == nullptr && plOSFile::ExistsFile(projectAsset))
    {
      pDocument = plQtEditorApp::GetSingleton()->OpenDocument(projectAsset, plDocumentFlags::None);
    }
    else if (pDocument == nullptr)
    {
      pDocument = plQtEditorApp::GetSingleton()->CreateDocument(projectAsset, plDocumentFlags::None);
    }

    if (pDocument != nullptr)
      RemoveLegacyAutomaticProjects(projectAsset);

    return plDynamicCast<plCSharpProjectAssetDocument*>(pDocument);
  }

  void MakeScriptClassName(plStringView sFileName, plStringBuilder& out_sClassName)
  {
    out_sClassName.Clear();
    bool bCapitalizeNext = true;

    for (const char c : sFileName)
    {
      if (c == '#')
      {
        out_sClassName.Append("Sharp");
        bCapitalizeNext = true;
        continue;
      }

      const bool bIsLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      const bool bIsDigit = c >= '0' && c <= '9';
      if (!bIsLetter && !bIsDigit && c != '_')
      {
        bCapitalizeNext = true;
        continue;
      }

      if (out_sClassName.IsEmpty() && bIsDigit)
        out_sClassName.Append("_");

      if (bCapitalizeNext && c >= 'a' && c <= 'z')
        out_sClassName.Append(static_cast<char>(c - 'a' + 'A'));
      else
        out_sClassName.Append(c);
      bCapitalizeNext = false;
    }

    if (out_sClassName.IsEmpty())
      out_sClassName = "NewScript";
  }

  void BuildNewScriptSource(plStringView sClassName, plStringBuilder& out_sSource)
  {
    out_sSource.Clear();
    out_sSource.Append("using Plasma;\n\n");
    out_sSource.Append("namespace Game.Scripts;\n\n");
    out_sSource.Append("[PlasmaScript]\n");
    out_sSource.Append("public sealed partial class ", sClassName, " : ComponentScript\n");
    out_sSource.Append("{\n");
    out_sSource.Append("    [Expose]\n");
    out_sSource.Append("    public bool Enabled { get; set; } = true;\n\n");
    out_sSource.Append("    public override void Update(Time deltaTime)\n");
    out_sSource.Append("    {\n");
    out_sSource.Append("        if (!Enabled)\n");
    out_sSource.Append("            return;\n");
    out_sSource.Append("    }\n");
    out_sSource.Append("}\n");
  }

  plStatus WriteNewScriptSource(plStringView sScriptFile)
  {
    if (plOSFile::ExistsFile(sScriptFile))
      return plStatus(plFmt("C# source file '{}' already exists.", sScriptFile));

    plStringBuilder parentDirectory = sScriptFile;
    parentDirectory.PathParentDirectory();
    if (plOSFile::CreateDirectoryStructure(parentDirectory).Failed())
      return plStatus(plFmt("Could not create directory '{}'.", parentDirectory));

    plStringBuilder className;
    MakeScriptClassName(plPathUtils::GetFileName(sScriptFile), className);

    plStringBuilder source;
    BuildNewScriptSource(className, source);

    plDeferredFileWriter writer;
    writer.SetOutput(sScriptFile);
    if (writer.WriteBytes(source.GetData(), source.GetElementCount()).Failed() ||
        writer.Close().Failed())
    {
      return plStatus(plFmt("Could not write C# source file '{}'.", sScriptFile));
    }

    return plStatus(PL_SUCCESS);
  }

  plStatus SynchronizeUntouchedStarterClassName(plStringView sScriptFile)
  {
    plFileReader reader;
    if (reader.Open(sScriptFile).Failed())
      return plStatus(plFmt("Could not read C# source file '{}'.", sScriptFile));

    plStringBuilder source;
    source.ReadAll(reader);
    reader.Close();

    constexpr plStringView sDeclarationPrefix = "public sealed partial class ";
    constexpr plStringView sDeclarationSuffix = " : ComponentScript";
    const char* szClassStart = source.FindSubString(sDeclarationPrefix);
    if (szClassStart == nullptr)
      return plStatus(PL_SUCCESS);
    szClassStart += sDeclarationPrefix.GetElementCount();

    const char* szClassEnd =
      plStringUtils::FindSubString(szClassStart, sDeclarationSuffix.GetStartPointer());
    if (szClassEnd == nullptr)
      return plStatus(PL_SUCCESS);

    const plStringView currentClass(szClassStart, szClassEnd);
    plStringBuilder expectedStarter;
    BuildNewScriptSource(currentClass, expectedStarter);
    if (source != expectedStarter)
      return plStatus(PL_SUCCESS);

    plStringBuilder desiredClass;
    MakeScriptClassName(plPathUtils::GetFileName(sScriptFile), desiredClass);
    if (currentClass == desiredClass)
      return plStatus(PL_SUCCESS);

    plStringBuilder updatedStarter;
    BuildNewScriptSource(desiredClass, updatedStarter);

    plDeferredFileWriter writer;
    writer.SetOutput(sScriptFile);
    if (writer.WriteBytes(updatedStarter.GetData(), updatedStarter.GetElementCount()).Failed() ||
        writer.Close().Failed())
    {
      return plStatus(plFmt("Could not update the starter class in '{}'.", sScriptFile));
    }

    return plStatus(PL_SUCCESS);
  }
} // namespace

plCSharpProjectAssetManager::plCSharpProjectAssetManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plCSharpProjectAssetManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "C# Script Project";
  m_DocTypeDesc.m_sFileExtension = "plCSharpProjectAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/CSharp.svg";
  m_DocTypeDesc.m_sAssetCategory = "Scripting";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plCSharpProjectAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_sResourceFileExtension = "plBinCSharpProject";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
  m_DocTypeDesc.m_bVisibleInAssetBrowser = false;

  plQtImageCache::GetSingleton()->RegisterTypeImage("C# Script Project", QPixmap(":/AssetIcons/CSharp.svg"));
}

plCSharpProjectAssetManager::~plCSharpProjectAssetManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plCSharpProjectAssetManager::OnDocumentManagerEvent, this));
}

void plCSharpProjectAssetManager::FillOutSubAssetList(
  const plAssetDocumentInfo& assetInfo, plDynamicArray<plSubAssetData>& out_subAssets) const
{
  out_subAssets.Clear();

  const plCSharpProjectAssetMetaData* pMetaData = assetInfo.GetMetaInfo<plCSharpProjectAssetMetaData>();
  if (pMetaData == nullptr || pMetaData->m_sDescriptorJson.IsEmpty())
    return;

  plString entryAssembly;
  plDynamicArray<plCSharpClassDescriptor> classes;
  const plStatus status =
    plCSharpProjectDescriptors::Parse(pMetaData->m_sDescriptorJson, assetInfo.m_DocumentID, entryAssembly, classes);
  if (status.Failed())
  {
    plLog::Warning("Could not publish legacy C# class aliases: {}", status.m_sMessage);
    return;
  }

  plHashedString classAssetType;
  classAssetType.Assign("C# Script Class");

  for (const plCSharpClassDescriptor& classDescriptor : classes)
  {
    plSubAssetData& subAsset = out_subAssets.ExpandAndGetRef();
    subAsset.m_Guid = classDescriptor.m_SubAssetGuid;
    subAsset.m_sSubAssetsDocumentTypeName = classAssetType;
    subAsset.m_sName = classDescriptor.m_sManagedName;
  }
}

plString plCSharpProjectAssetManager::GetAssetTableEntry(
  const plSubAsset* pSubAsset, plStringView sDataDirectory, const plPlatformProfile* pAssetProfile) const
{
  plStringBuilder result = SUPER::GetAssetTableEntry(pSubAsset, sDataDirectory, pAssetProfile);
  if (!pSubAsset->m_bMainAsset)
  {
    plStringBuilder classGuid;
    plConversionUtils::ToString(pSubAsset->m_Data.m_Guid, classGuid);
    result.Append("|", classGuid);
  }
  return result;
}

void plCSharpProjectAssetManager::OnDocumentManagerEvent(const plDocumentManager::Event& event)
{
  if (event.m_Type == plDocumentManager::Event::Type::DocumentWindowRequested &&
      event.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plCSharpProjectAssetDocument>())
  {
    new plQtCSharpProjectAssetDocumentWindow(static_cast<plCSharpProjectAssetDocument*>(event.m_pDocument)); // NOLINT
  }
}

void plCSharpProjectAssetManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath,
  bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  PL_IGNORE_UNUSED(sDocumentTypeName);
  PL_IGNORE_UNUSED(bCreateNewDocument);
  PL_IGNORE_UNUSED(pOpenContext);
  out_pDocument = new plCSharpProjectAssetDocument(sPath);
}

void plCSharpProjectAssetManager::InternalGetSupportedDocumentTypes(
  plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}

plCSharpScriptAssetManager::plCSharpScriptAssetManager()
{
  m_DocTypeDesc.m_bCanCreate = true;
  m_DocTypeDesc.m_sDocumentTypeName = "C# Script";
  m_DocTypeDesc.m_sFileExtension = "cs";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/CSharp.svg";
  m_DocTypeDesc.m_sAssetCategory = "Scripting";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plCSharpProjectAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_ScriptClass");
  m_DocTypeDesc.m_sResourceFileExtension = "plBinCSharpProject";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::DisableTransform;

  plQtImageCache::GetSingleton()->RegisterTypeImage("C# Script", QPixmap(":/AssetIcons/CSharp.svg"));
}

plStatus plCSharpScriptAssetManager::ReadAssetDocumentInfo(plUniquePtr<plAssetDocumentInfo>& out_pInfo,
  plStreamReader& inout_stream, plStringView sDocumentPath, const plFileStatus& fileStatus) const
{
  PL_IGNORE_UNUSED(inout_stream);

  out_pInfo = PL_DEFAULT_NEW(plAssetDocumentInfo);
  out_pInfo->m_DocumentID = fileStatus.m_DocumentID;
  if (!out_pInfo->m_DocumentID.IsValid())
  {
    plStringBuilder identity("pl-csharp-script-asset/v1:", sDocumentPath);
    identity.MakeCleanPath();
    out_pInfo->m_DocumentID = plUuid::MakeStableUuidFromString(identity);
  }
  out_pInfo->m_uiSettingsHash = fileStatus.m_uiHash;
  out_pInfo->m_sAssetsDocumentTypeName.Assign("C# Script");

  ScriptProjectMatch match;
  if (FindOwningProject(sDocumentPath, match))
  {
    plCSharpScriptAssetMetaData* pScriptMetadata = PL_DEFAULT_NEW(plCSharpScriptAssetMetaData);
    pScriptMetadata->m_sProjectAsset = match.m_sProjectAsset;
    pScriptMetadata->m_ClassGuid = match.m_ClassGuid;
    out_pInfo->m_MetaInfo.PushBack(pScriptMetadata);

    if (match.m_ProjectGuid.IsValid())
    {
      plStringBuilder projectGuid;
      plConversionUtils::ToString(match.m_ProjectGuid, projectGuid);
      out_pInfo->m_TransformDependencies.Insert(projectGuid);
    }

    if (match.m_bHasClass)
    {
      out_pInfo->m_MetaInfo.PushBack(
        plCSharpProjectAssetDocument::CreateExposedParameters(
          match.m_ClassDescriptor, out_pInfo->m_DocumentID));
    }
  }
  return plStatus(PL_SUCCESS);
}

bool plCSharpScriptAssetManager::IsOutputUpToDate(plStringView sDocumentPath, plStringView sOutputTag,
  plUInt64 uiHash, const plAssetDocumentTypeDescriptor* pTypeDescriptor)
{
  PL_IGNORE_UNUSED(sOutputTag);
  PL_IGNORE_UNUSED(uiHash);
  PL_IGNORE_UNUSED(pTypeDescriptor);

  ScriptProjectMatch match;
  if (!FindOwningProject(sDocumentPath, match) ||
      !match.m_bProjectUpToDate || !match.m_bHasCompiledSourceHash)
  {
    return false;
  }

  plFileStatus sourceStatus;
  if (plFileSystemModel::GetSingleton()->HashFile(sDocumentPath, sourceStatus).Failed())
    return false;

  return sourceStatus.m_uiHash == match.m_uiCompiledSourceHash;
}

plString plCSharpScriptAssetManager::GetAssetTableEntry(
  const plSubAsset* pSubAsset, plStringView sDataDirectory, const plPlatformProfile* pAssetProfile) const
{
  const plCSharpScriptAssetMetaData* pMetadata =
    pSubAsset->m_pAssetInfo->m_Info->GetMetaInfo<plCSharpScriptAssetMetaData>();
  if (pMetadata == nullptr || pMetadata->m_sProjectAsset.IsEmpty() ||
      !pMetadata->m_ClassGuid.IsValid())
    return {};

  plStringBuilder result = pMetadata->m_sProjectAsset;
  result.MakeRelativeTo(sDataDirectory).IgnoreResult();
  GenerateOutputFilename(
    result, DetermineFinalTargetProfile(pAssetProfile), "plBinCSharpProject", false);
  plStringBuilder classGuid;
  plConversionUtils::ToString(pMetadata->m_ClassGuid, classGuid);
  result.Append("|", classGuid);
  return result;
}

plResult plCSharpScriptAssetManager::OpenDocumentExternally(plStringView sFilePath)
{
  SynchronizeUntouchedStarterClassName(sFilePath).LogFailure();

  plCSharpProjectAssetDocument* pProjectDocument = OpenOrCreateAutomaticProject();

  if (pProjectDocument == nullptr)
  {
    plLog::Error(
      "Could not open the hidden C# project for script '{}'. Opening the source file directly instead.",
      sFilePath);
    plStringBuilder scriptFile(sFilePath);
    if (!plQtUiServices::OpenFileInDefaultProgram(scriptFile))
    {
      plQtUiServices::ShowGlobalStatusBarMessage(
        plFmt("Could not open C# script '{}'. See the log for details.", sFilePath));
    }

    // The .cs extension is externally handled. Returning failure here would make
    // the editor try to deserialize source text as an asset document.
    return PL_SUCCESS;
  }

  const plStatus openStatus = pProjectDocument->OpenScriptInIDE(sFilePath);
  if (openStatus.Failed())
  {
    plLog::Error("Could not open C# script '{}': {}", sFilePath, openStatus.m_sMessage);
    plQtUiServices::ShowGlobalStatusBarMessage(
      plFmt("Could not open C# script '{}': {}", sFilePath, openStatus.m_sMessage));
  }

  // This file type is always externally handled, even when launching the IDE
  // fails. Otherwise the editor attempts to load the .cs file as a document.
  return PL_SUCCESS;
}

plStatus plCSharpScriptAssetManager::CreateDocumentExternally(plStringView sFilePath)
{
  PL_SUCCEED_OR_RETURN(WriteNewScriptSource(sFilePath));

  if (OpenOrCreateAutomaticProject() == nullptr)
  {
    plLog::Warning(
      "Created C# script '{}', but its automatic project could not be created yet.",
      sFilePath);
  }

  plFileSystemModel::GetSingleton()->NotifyOfChange(sFilePath);
  plAssetCurator::GetSingleton()->NotifyOfFileChange(sFilePath);

  return plStatus(PL_SUCCESS);
}

void plCSharpScriptAssetManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath,
  bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  PL_IGNORE_UNUSED(sDocumentTypeName);
  PL_IGNORE_UNUSED(bCreateNewDocument);
  PL_IGNORE_UNUSED(pOpenContext);
  out_pDocument = new plCSharpProjectAssetDocument(sPath);
}

void plCSharpScriptAssetManager::InternalGetSupportedDocumentTypes(
  plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
