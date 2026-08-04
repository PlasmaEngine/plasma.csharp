#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <Core/Scripting/Bindings/ScriptBindingManifest.h>
#include <Core/Scripting/Bindings/ScriptBindingRegistry.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EditorApp/EditorApp.moc.h>
#include <EditorFramework/GUI/ExposedParameters.h>
#include <EditorPluginCSharp/CSharpEditorPaths.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpSourceDependencyTracker.h>
#include <EditorPluginCSharp/Preferences/CSharpPreferences.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/FileSystem/DeferredFileWriter.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/JSONWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/Process.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/Progress.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <ToolsFoundation/FileSystem/FileSystemModel.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>
#include <ToolsFoundation/Project/ToolsProject.h>

#include <QInputDialog>

#include <charconv>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpProjectAssetMetaData, 3, plRTTIDefaultAllocator<plCSharpProjectAssetMetaData>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("DescriptorJson", m_sDescriptorJson),
    PL_MEMBER_PROPERTY("ProjectFile", m_sProjectFile),
    PL_MEMBER_PROPERTY("SourceRoot", m_sSourceRoot),
    PL_ARRAY_MEMBER_PROPERTY("LastGoodSources", m_LastGoodSources),
    PL_ARRAY_MEMBER_PROPERTY("LastGoodSourceHashes", m_LastGoodSourceHashes),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpProjectAssetProperties, 2, plRTTIDefaultAllocator<plCSharpProjectAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ProjectFile", m_sProjectFile)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("SourceRoot", m_sSourceRoot)->AddAttributes(new plDefaultValueAttribute("Scripts"), new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("BuildConfiguration", m_sBuildConfiguration)->AddAttributes(
      new plDefaultValueAttribute("Development"),
      new plDynamicStringEnumAttribute("CSharpBuildConfiguration")),
    PL_MEMBER_PROPERTY("BindingManifest", m_sBindingManifest)->AddAttributes(
      new plDefaultValueAttribute("Intermediate/PlasmaBindings.json"),
      new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("LastGoodDescriptorJson", m_sLastGoodDescriptorJson)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("LastGoodSources", m_LastGoodSources)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("LastGoodSourceHashes", m_LastGoodSourceHashes)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("LastGoodFiles", m_LastGoodFiles)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("LastGoodEntryAssembly", m_sLastGoodEntryAssembly)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("LastGoodBindingSchemaHash", m_uiLastGoodBindingSchemaHash)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("BuildDiagnostics", m_sBuildDiagnostics)->AddAttributes(new plHiddenAttribute()),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpProjectAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  bool IsIgnoredSourceDirectory(plStringView sPath);

  bool ContainsCSharpSource(
    plStringView sRelativeDirectory, const plDynamicArray<plString>& sources)
  {
    plStringBuilder prefix = sRelativeDirectory;
    if (!prefix.IsEmpty())
      prefix.Append("/");

    for (const plString& source : sources)
    {
      if (source.StartsWith_NoCase(prefix))
        return true;
    }
    return false;
  }

  void GatherVsCodeExclusions(plStringView sAbsoluteDirectory,
    plStringView sRelativeDirectory, plStringView sProjectFile,
    const plDynamicArray<plString>& sources, plSet<plString>& out_exclusions)
  {
    plFileSystemIterator iterator;
    iterator.StartSearch(sAbsoluteDirectory,
      plFileSystemIteratorFlags::ReportFiles |
        plFileSystemIteratorFlags::ReportFolders);

    for (; iterator.IsValid(); iterator.Next())
    {
      plStringBuilder absolutePath = iterator.GetCurrentPath();
      absolutePath.AppendPath(iterator.GetStats().m_sName);
      absolutePath.MakeCleanPath();

      plStringBuilder relativePath = sRelativeDirectory;
      relativePath.AppendPath(iterator.GetStats().m_sName);
      relativePath.MakeCleanPath();
      relativePath.ReplaceAll("\\", "/");

      if (iterator.GetStats().m_bIsDirectory)
      {
        if (IsIgnoredSourceDirectory(absolutePath) ||
            !ContainsCSharpSource(relativePath, sources))
        {
          out_exclusions.Insert(relativePath);
        }
        else
        {
          GatherVsCodeExclusions(
            absolutePath, relativePath, sProjectFile, sources, out_exclusions);
        }
      }
      else if (!absolutePath.GetFileExtension().IsEqual_NoCase("cs") &&
               !relativePath.IsEqual_NoCase(sProjectFile))
      {
        out_exclusions.Insert(relativePath);
      }
    }
  }

  plStatus WriteVsCodeWorkspace(
    plStringView sProjectFile, plStringBuilder& out_sWorkspaceFile)
  {
    plStringBuilder projectDirectory = sProjectFile;
    projectDirectory.PathParentDirectory();

    plStringBuilder relativeProjectFile = sProjectFile;
    if (relativeProjectFile.MakeRelativeTo(projectDirectory).Failed())
      return plStatus(plFmt(
        "Could not make C# project '{}' relative to its directory.",
        sProjectFile));
    relativeProjectFile.ReplaceAll("\\", "/");

    out_sWorkspaceFile = sProjectFile;
    out_sWorkspaceFile.ChangeFileExtension("code-workspace");
    plStringBuilder relativeWorkspaceFile = out_sWorkspaceFile;
    relativeWorkspaceFile.MakeRelativeTo(projectDirectory).IgnoreResult();
    relativeWorkspaceFile.ReplaceAll("\\", "/");

    plDynamicArray<plString> sources;
    plFileSystemIterator sourceIterator;
    for (sourceIterator.StartSearch(
           projectDirectory, plFileSystemIteratorFlags::ReportFilesRecursive);
         sourceIterator.IsValid(); sourceIterator.Next())
    {
      plStringBuilder sourceFile = sourceIterator.GetCurrentPath();
      sourceFile.AppendPath(sourceIterator.GetStats().m_sName);
      if (!sourceFile.GetFileExtension().IsEqual_NoCase("cs") ||
          IsIgnoredSourceDirectory(sourceFile))
      {
        continue;
      }

      if (sourceFile.MakeRelativeTo(projectDirectory).Failed())
        continue;
      sourceFile.MakeCleanPath();
      sourceFile.ReplaceAll("\\", "/");
      sources.PushBack(sourceFile);
    }
    sources.Sort();

    plSet<plString> exclusions;
    GatherVsCodeExclusions(
      projectDirectory, {}, relativeProjectFile, sources, exclusions);
    exclusions.Insert(relativeWorkspaceFile);

    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter stream(&storage);
    plStandardJSONWriter json;
    json.SetOutputStream(&stream);
    json.SetWhitespaceMode(plJSONWriter::WhitespaceMode::LessIndentation);
    json.BeginObject();
    json.BeginArray("folders");
    json.BeginObject();
    json.AddVariableString("name", "C# Scripts");
    json.AddVariableString("path", ".");
    json.EndObject();
    json.EndArray();
    json.BeginObject("settings");
    json.BeginObject("files.exclude");
    for (const plString& exclusion : exclusions)
      json.AddVariableBool(exclusion, true);
    json.EndObject();
    json.BeginObject("search.exclude");
    for (const plString& exclusion : exclusions)
      json.AddVariableBool(exclusion, true);
    json.EndObject();
    json.EndObject();
    json.EndObject();

    plDeferredFileWriter writer;
    writer.SetOutput(out_sWorkspaceFile);
    if (writer.WriteBytes(storage.GetData(), storage.GetStorageSize32()).Failed() ||
        writer.Close().Failed())
    {
      return plStatus(
        plFmt("Could not write VS Code workspace '{}'.", out_sWorkspaceFile));
    }
    return plStatus(PL_SUCCESS);
  }

  /// \brief Swaps the old ProjectReference pair for references to the shipped assemblies.
  ///
  /// Projects created before this plugin became a package point at csproj files under the engine's
  /// Code/ tree. That directory does not exist in a package install or in any launcher-installed
  /// SDK, so every script fails to resolve the Plasma namespace. There is no managed source to build
  /// against any more - the assemblies ship built - so the references change kind, not just path.
  ///
  /// Done by line rather than by substring: the old entries carry absolute paths that differ per
  /// machine, and one of them spans four lines, so there is no stable string to swap.
  bool MigrateManagedReferences(plStringBuilder& ref_contents, plStringView sDocumentDirectory)
  {
    if (ref_contents.FindSubString("Plasma.ScriptCore.csproj") == nullptr &&
        ref_contents.FindSubString("Plasma.Engine.Generator.csproj") == nullptr)
    {
      return false;
    }

    plStringBuilder managedRoot = plCSharpEditorPaths::FindPayloadRoot();
    managedRoot.AppendPath("CSharp");

    plStringBuilder generator(managedRoot);
    generator.AppendPath("M0Game/Plasma.Engine.Generator.dll");

    auto relativise = [sDocumentDirectory](plStringBuilder& ref_path)
    {
      plStringBuilder sRel = ref_path;
      if (sRel.MakeRelativeTo(sDocumentDirectory).Succeeded())
        ref_path = sRel;

      ref_path.MakeCleanPath();
      ref_path.ReplaceAll("\\", "/");
    };

    relativise(managedRoot);
    relativise(generator);

    plHybridArray<plStringView, 64> lines;
    ref_contents.Split(true, lines, "\n");

    plStringBuilder out;
    bool bInserted = false;
    bool bSkippingElement = false;

    for (plStringView line : lines)
    {
      if (bSkippingElement)
      {
        // The generator reference is written across four lines; swallow to its closing tag.
        if (line.FindSubString("/>") != nullptr)
          bSkippingElement = false;

        continue;
      }

      const bool bManaged = line.FindSubString("<ProjectReference") != nullptr &&
                            (line.FindSubString("Plasma.ScriptCore") != nullptr ||
                              line.FindSubString("Plasma.Engine.Generator") != nullptr);

      if (bManaged)
      {
        if (!bInserted)
        {
          out.Append("    <Reference Include=\"Plasma.ScriptCore\">\n");
          out.Append("      <HintPath>", managedRoot, "/Plasma.ScriptCore.dll</HintPath>\n");
          out.Append("      <Private>false</Private>\n");
          out.Append("    </Reference>\n");
          out.Append("    <Analyzer Include=\"", generator, "\" />\n");
          bInserted = true;
        }

        if (line.FindSubString("/>") == nullptr)
          bSkippingElement = true;

        continue;
      }

      out.Append(line);
      out.Append("\n");
    }

    if (!bInserted)
      return false;

    // Split(true, ...) yields a trailing empty piece for the final newline, which the loop turns
    // back into one; anything more would grow the file a line per migration.
    while (out.EndsWith("\n\n"))
      out.Shrink(0, 1);

    ref_contents = out;
    return true;
  }

  plStatus UpdateGeneratedProjectItemMode(plStringView sProjectFile)
  {
    plFileReader reader;
    if (reader.Open(sProjectFile).Failed())
      return plStatus(plFmt("Could not read C# project '{}'.", sProjectFile));

    plStringBuilder contents;
    contents.ReadAll(reader);
    reader.Close();

    if (contents.FindSubString("<PlasmaSourceRoot>") == nullptr)
      return plStatus(PL_SUCCESS);

    plStringBuilder documentDirectory = sProjectFile;
    documentDirectory.PathParentDirectory();

    bool bChanged = MigrateManagedReferences(contents, documentDirectory);

    if (contents.FindSubString("Plasma.Engine.Generator.csproj") == nullptr && !bChanged)
      return plStatus(PL_SUCCESS);
    if (contents.FindSubString("<EnableDefaultCompileItems>false</EnableDefaultCompileItems>") != nullptr)
    {
      contents.ReplaceAll(
        "<EnableDefaultCompileItems>false</EnableDefaultCompileItems>",
        "<EnableDefaultItems>false</EnableDefaultItems>");
      bChanged = true;
    }

    if (contents.FindSubString("<Configurations>Development</Configurations>") == nullptr)
    {
      contents.ReplaceAll(
        "    <TargetFramework>net10.0</TargetFramework>\n",
        "    <TargetFramework>net10.0</TargetFramework>\n"
        "    <Configurations>Development</Configurations>\n"
        "    <Configuration>Development</Configuration>\n");
      bChanged = true;
    }

    if (contents.FindSubString("    <Configuration Condition=\"'$(Configuration)' == ''\">Development</Configuration>\n") != nullptr)
    {
      contents.ReplaceAll(
        "    <Configuration Condition=\"'$(Configuration)' == ''\">Development</Configuration>\n",
        "    <Configuration>Development</Configuration>\n");
      bChanged = true;
    }

    if (contents.FindSubString("<Configuration>Development</Configuration>") == nullptr)
    {
      contents.ReplaceAll(
        "    <Configurations>Development</Configurations>\n",
        "    <Configurations>Development</Configurations>\n"
        "    <Configuration>Development</Configuration>\n");
      bChanged = true;
    }

    if (contents.FindSubString("    <WarningsNotAsErrors>$(WarningsNotAsErrors);PLB002</WarningsNotAsErrors>\n") != nullptr)
    {
      contents.ReplaceAll(
        "    <WarningsNotAsErrors>$(WarningsNotAsErrors);PLB002</WarningsNotAsErrors>\n", "");
      bChanged = true;
    }

    if (contents.FindSubString("Plasma.ScriptCore.csproj\" AdditionalProperties=\"Configuration=Development\"") == nullptr)
    {
      contents.ReplaceAll(
        "Plasma.ScriptCore/Plasma.ScriptCore.csproj\" />",
        "Plasma.ScriptCore/Plasma.ScriptCore.csproj\" AdditionalProperties=\"Configuration=Development\" />");
      bChanged = true;
    }

    if (contents.FindSubString("Plasma.Engine.Generator.csproj\"\n                      AdditionalProperties=\"Configuration=Development\"") == nullptr)
    {
      contents.ReplaceAll(
        "Plasma.Engine.Generator/Plasma.Engine.Generator.csproj\"\n                      OutputItemType=\"Analyzer\"",
        "Plasma.Engine.Generator/Plasma.Engine.Generator.csproj\"\n"
        "                      AdditionalProperties=\"Configuration=Development\"\n"
        "                      OutputItemType=\"Analyzer\"");
      bChanged = true;
    }

    if (!bChanged)
      return plStatus(PL_SUCCESS);

    plDeferredFileWriter writer;
    writer.SetOutput(sProjectFile);
    if (writer.WriteBytes(contents.GetData(), contents.GetElementCount()).Failed() ||
        writer.Close().Failed())
    {
      return plStatus(
        plFmt("Could not update generated C# project '{}'.", sProjectFile));
    }
    return plStatus(PL_SUCCESS);
  }

  plStatus OpenCSharpIDE(plStringView sProjectFile, plStringView sScriptFile = {})
  {
    PL_SUCCEED_OR_RETURN(UpdateGeneratedProjectItemMode(sProjectFile));

    const plCSharpPreferences* pPreferences =
      plPreferences::QueryPreferences<plCSharpPreferences>();

    switch (pPreferences->m_IDE.GetValue())
    {
      case plCSharpIDE::VisualStudio:
      {
        QStringList arguments;
        arguments.append(plMakeQString(sProjectFile));
        if (!sScriptFile.IsEmpty())
          arguments.append(plMakeQString(sScriptFile));
        return plQtUiServices::OpenInVisualStudio(arguments);
      }

      case plCSharpIDE::Rider:
      {
        QStringList arguments;
        arguments.append(plMakeQString(sProjectFile));
        if (!sScriptFile.IsEmpty())
          arguments.append(plMakeQString(sScriptFile));
        return plQtUiServices::OpenInRider(arguments);
      }

      case plCSharpIDE::VisualStudioCode:
      {
        plStringBuilder workspaceFile;
        PL_SUCCEED_OR_RETURN(WriteVsCodeWorkspace(sProjectFile, workspaceFile));

        QStringList arguments;
        arguments.append(plMakeQString(workspaceFile));
        if (!sScriptFile.IsEmpty())
        {
          arguments.append("-g");
          arguments.append(plMakeQString(sScriptFile));
        }
        return plQtUiServices::OpenInVsCode(arguments);
      }
    }

    return plStatus("The selected C# IDE is not supported.");
  }

  void RefreshCSharpSourceAssets()
  {
    plAssetCurator* pCurator = plAssetCurator::GetSingleton();
    if (pCurator == nullptr)
      return;

    plDynamicArray<plString> scriptFiles;
    {
      const auto knownAssets = pCurator->GetKnownAssets();
      for (auto it = knownAssets->GetIterator(); it.IsValid(); ++it)
      {
        const plAssetInfo* pInfo = it.Value();
        if (pInfo != nullptr &&
            pInfo->m_Info->m_sAssetsDocumentTypeName ==
              plTempHashedString("C# Script"))
        {
          scriptFiles.PushBack(pInfo->m_Path.GetAbsolutePath());
        }
      }
    }

    for (const plString& scriptFile : scriptFiles)
      pCurator->NotifyOfFileChange(scriptFile);
  }

  plMutex s_BindingSnapshotMutex;
  plScriptBindingSnapshot s_CachedBindingSnapshot;
  plUInt64 s_uiCachedBindingSchemaHash = 0;
  bool s_bBindingSnapshotValid = false;

  plStatus GetCachedBindingSnapshot(
    plScriptBindingSnapshot& out_snapshot, plUInt64& out_uiSchemaHash)
  {
    PL_LOCK(s_BindingSnapshotMutex);
    if (!s_bBindingSnapshotValid)
    {
      PL_PROFILE_SCOPE("CSharpEditor.CreateBindingSnapshot");
      if (plScriptBindingRegistry::CreateSnapshot(s_CachedBindingSnapshot).Failed())
        return plStatus("Failed to create the reflected C# binding snapshot.");

      s_uiCachedBindingSchemaHash =
        plScriptBindingManifest::ComputeSchemaHash(s_CachedBindingSnapshot);
      if (s_uiCachedBindingSchemaHash == 0)
      {
        s_CachedBindingSnapshot.Clear();
        return plStatus(
          "The reflected C# binding schema hash could not be computed.");
      }

      s_bBindingSnapshotValid = true;
    }

    out_snapshot = s_CachedBindingSnapshot;
    out_uiSchemaHash = s_uiCachedBindingSchemaHash;
    return plStatus(PL_SUCCESS);
  }

  bool IsBuildError(plStringView sLine)
  {
    return sLine.FindSubString_NoCase(": error ") != nullptr ||
           sLine.FindSubString_NoCase(" error CS") != nullptr ||
           sLine.FindSubString_NoCase("fatal error") != nullptr ||
           sLine.StartsWith_NoCase("error ");
  }

  bool IsBuildWarning(plStringView sLine)
  {
    return sLine.FindSubString_NoCase(": warning ") != nullptr ||
           sLine.FindSubString_NoCase(" warning CS") != nullptr ||
           sLine.StartsWith_NoCase("warning ");
  }

  void RemoveVolatileBuildDiagnostics(plStringBuilder& inout_sDiagnostics)
  {
    plHybridArray<plStringView, 64> lines;
    inout_sDiagnostics.Split(false, lines, "\r", "\n");

    plStringBuilder stableDiagnostics;
    for (plStringView line : lines)
    {
      plStringView trimmedLine = line;
      trimmedLine.Trim(" \t");
      if (trimmedLine.StartsWith_NoCase("Time Elapsed ") ||
          trimmedLine.StartsWith_NoCase("Restored ") ||
          trimmedLine.StartsWith_NoCase("Restore completed "))
      {
        continue;
      }

      stableDiagnostics.Append(line, "\n");
    }

    stableDiagnostics.Trim("\r\n");
    inout_sDiagnostics = stableDiagnostics;
  }

  bool IsIgnoredSourceDirectory(plStringView sPath)
  {
    plStringBuilder clean = sPath;
    clean.MakeCleanPath();
    clean.ToLower();
    clean.Prepend("/");
    clean.Append("/");

    return clean.FindSubString("/assetcache/") != nullptr ||
           clean.FindSubString("/bin/") != nullptr ||
           clean.FindSubString("/obj/") != nullptr ||
           clean.FindSubString("/intermediate/") != nullptr ||
           clean.FindSubString("/.git/") != nullptr;
  }

  plResult ParseSigned(plStringView sValue, plInt64& out_iValue)
  {
    return plConversionUtils::StringToInt64(sValue, out_iValue);
  }

  plResult ParseUnsigned(plStringView sValue, plUInt64& out_uiValue)
  {
    const char* pBegin = sValue.GetStartPointer();
    const char* pEnd = sValue.GetEndPointer();
    const std::from_chars_result result = std::from_chars(pBegin, pEnd, out_uiValue, 10);
    return result.ec == std::errc() && result.ptr == pEnd ? PL_SUCCESS : PL_FAILURE;
  }

  plResult ParseDouble(plStringView sValue, double& out_fValue)
  {
    const char* pLast = nullptr;
    if (plConversionUtils::StringToFloat(sValue, out_fValue, &pLast).Failed())
      return PL_FAILURE;
    return pLast == sValue.GetEndPointer() ? PL_SUCCESS : PL_FAILURE;
  }

  plResult GetComponent(const plCSharpFieldDefaultValue& value, plStringView sName, double& out_fValue)
  {
    const plString* pComponent = nullptr;
    if (!value.m_Components.TryGetValue(sName, pComponent))
      return PL_FAILURE;
    return ParseDouble(*pComponent, out_fValue);
  }

  plVariant MakeDefaultValue(const plCSharpFieldDescriptor& field)
  {
    const plCSharpFieldDefaultValue& value = field.m_DefaultValue;

    if (value.m_sKind == "null")
      return plVariant();
    if (value.m_sKind == "boolean")
      return plVariant(value.m_sValue.IsEqual_NoCase("true"));
    if (value.m_sKind == "string")
      return plVariant(value.m_sValue);

    if (value.m_sKind == "signed" || value.m_sKind == "enumSigned")
    {
      plInt64 iValue = 0;
      if (ParseSigned(value.m_sValue, iValue).Failed())
        return plVariant();

      if (field.m_sManagedType == "System.SByte")
        return plVariant(static_cast<plInt8>(iValue));
      if (field.m_sManagedType == "System.Int16")
        return plVariant(static_cast<plInt16>(iValue));
      if (field.m_sManagedType == "System.Int32")
        return plVariant(static_cast<plInt32>(iValue));
      return plVariant(iValue);
    }

    if (value.m_sKind == "unsigned" || value.m_sKind == "enum" ||
        value.m_sKind == "enumUnsigned")
    {
      plUInt64 uiValue = 0;
      if (ParseUnsigned(value.m_sValue, uiValue).Failed())
        return plVariant();

      if (field.m_sManagedType == "System.Byte")
        return plVariant(static_cast<plUInt8>(uiValue));
      if (field.m_sManagedType == "System.UInt16")
        return plVariant(static_cast<plUInt16>(uiValue));
      if (field.m_sManagedType == "System.UInt32")
        return plVariant(static_cast<plUInt32>(uiValue));
      return plVariant(uiValue);
    }

    if (value.m_sKind == "floating" || value.m_sKind == "time" || value.m_sKind == "angle")
    {
      double fValue = 0.0;
      if (ParseDouble(value.m_sValue, fValue).Failed())
        return plVariant();

      if (value.m_sKind == "time")
        return plVariant(plTime::MakeFromSeconds(fValue));
      if (value.m_sKind == "angle")
        return plVariant(plAngle::MakeFromRadian(static_cast<float>(fValue)));
      if (field.m_sManagedType == "System.Single")
        return plVariant(static_cast<float>(fValue));
      return plVariant(fValue);
    }

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
    if (value.m_sKind == "vec2" &&
        GetComponent(value, "x", x).Succeeded() &&
        GetComponent(value, "y", y).Succeeded())
    {
      return plVariant(plVec2(static_cast<float>(x), static_cast<float>(y)));
    }
    if (value.m_sKind == "vec3" &&
        GetComponent(value, "x", x).Succeeded() &&
        GetComponent(value, "y", y).Succeeded() &&
        GetComponent(value, "z", z).Succeeded())
    {
      return plVariant(plVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
    }
    if (value.m_sKind == "vec4" &&
        GetComponent(value, "x", x).Succeeded() &&
        GetComponent(value, "y", y).Succeeded() &&
        GetComponent(value, "z", z).Succeeded() &&
        GetComponent(value, "w", w).Succeeded())
    {
      return plVariant(plVec4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)));
    }
    if (value.m_sKind == "quat" &&
        GetComponent(value, "x", x).Succeeded() &&
        GetComponent(value, "y", y).Succeeded() &&
        GetComponent(value, "z", z).Succeeded() &&
        GetComponent(value, "w", w).Succeeded())
    {
      return plVariant(plQuat(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)));
    }
    if (value.m_sKind == "color")
    {
      double r = 0.0;
      double g = 0.0;
      double b = 0.0;
      double a = 1.0;
      if (GetComponent(value, "r", r).Succeeded() &&
          GetComponent(value, "g", g).Succeeded() &&
          GetComponent(value, "b", b).Succeeded() &&
          GetComponent(value, "a", a).Succeeded())
      {
        return plVariant(plColor(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a)));
      }
    }
    if (value.m_sKind == "transform")
    {
      double px = 0.0;
      double py = 0.0;
      double pz = 0.0;
      double rx = 0.0;
      double ry = 0.0;
      double rz = 0.0;
      double rw = 1.0;
      double sx = 1.0;
      double sy = 1.0;
      double sz = 1.0;
      if (GetComponent(value, "px", px).Succeeded() &&
          GetComponent(value, "py", py).Succeeded() &&
          GetComponent(value, "pz", pz).Succeeded() &&
          GetComponent(value, "rx", rx).Succeeded() &&
          GetComponent(value, "ry", ry).Succeeded() &&
          GetComponent(value, "rz", rz).Succeeded() &&
          GetComponent(value, "rw", rw).Succeeded() &&
          GetComponent(value, "sx", sx).Succeeded() &&
          GetComponent(value, "sy", sy).Succeeded() &&
          GetComponent(value, "sz", sz).Succeeded())
      {
        return plVariant(plTransform(
          plVec3(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz)),
          plQuat(static_cast<float>(rx), static_cast<float>(ry), static_cast<float>(rz), static_cast<float>(rw)),
          plVec3(static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz))));
      }
    }

    return plVariant();
  }

  plVariant ConvertNumericMetadata(plStringView sValue, const plVariant& defaultValue)
  {
    double fValue = 0.0;
    if (ParseDouble(sValue, fValue).Failed())
      return plVariant();

    switch (defaultValue.GetType())
    {
      case plVariantType::Int8:
        return static_cast<plInt8>(fValue);
      case plVariantType::UInt8:
        return static_cast<plUInt8>(fValue);
      case plVariantType::Int16:
        return static_cast<plInt16>(fValue);
      case plVariantType::UInt16:
        return static_cast<plUInt16>(fValue);
      case plVariantType::Int32:
        return static_cast<plInt32>(fValue);
      case plVariantType::UInt32:
        return static_cast<plUInt32>(fValue);
      case plVariantType::Int64:
        return static_cast<plInt64>(fValue);
      case plVariantType::UInt64:
        return static_cast<plUInt64>(fValue);
      case plVariantType::Float:
        return static_cast<float>(fValue);
      case plVariantType::Double:
        return fValue;
      default:
        return fValue;
    }
  }

  const plRTTI* GetExposedParameterType(plStringView sManagedType, plStringView sDefaultKind)
  {
    if (sManagedType == "System.Boolean")
      return plGetStaticRTTI<bool>();
    if (sManagedType == "System.Byte")
      return plGetStaticRTTI<plUInt8>();
    if (sManagedType == "System.SByte")
      return plGetStaticRTTI<plInt8>();
    if (sManagedType == "System.Int16")
      return plGetStaticRTTI<plInt16>();
    if (sManagedType == "System.UInt16")
      return plGetStaticRTTI<plUInt16>();
    if (sManagedType == "System.Int32")
      return plGetStaticRTTI<plInt32>();
    if (sManagedType == "System.UInt32")
      return plGetStaticRTTI<plUInt32>();
    if (sManagedType == "System.Int64")
      return plGetStaticRTTI<plInt64>();
    if (sManagedType == "System.UInt64")
      return plGetStaticRTTI<plUInt64>();
    if (sManagedType == "System.Single")
      return plGetStaticRTTI<float>();
    if (sManagedType == "System.Double")
      return plGetStaticRTTI<double>();
    if (sManagedType == "System.String")
      return plGetStaticRTTI<plString>();
    if (sManagedType == "Plasma.Time")
      return plGetStaticRTTI<plTime>();
    if (sManagedType == "Plasma.Angle")
      return plGetStaticRTTI<plAngle>();
    if (sManagedType == "Plasma.Vec2")
      return plGetStaticRTTI<plVec2>();
    if (sManagedType == "Plasma.Vec3")
      return plGetStaticRTTI<plVec3>();
    if (sManagedType == "Plasma.Vec4")
      return plGetStaticRTTI<plVec4>();
    if (sManagedType == "Plasma.Quat")
      return plGetStaticRTTI<plQuat>();
    if (sManagedType == "Plasma.Color")
      return plGetStaticRTTI<plColor>();
    if (sManagedType == "Plasma.Transform")
      return plGetStaticRTTI<plTransform>();
    if (sDefaultKind == "enumSigned")
      return plGetStaticRTTI<plInt64>();
    if (sDefaultKind == "enum" || sDefaultKind == "enumUnsigned")
      return plGetStaticRTTI<plUInt64>();
    return nullptr;
  }

  bool IsValidCSharpIdentifier(plStringView sName)
  {
    if (sName.IsEmpty())
      return false;

    const char* pCharacter = sName.GetStartPointer();
    if (!((*pCharacter >= 'A' && *pCharacter <= 'Z') || (*pCharacter >= 'a' && *pCharacter <= 'z') || *pCharacter == '_'))
      return false;

    for (++pCharacter; pCharacter < sName.GetEndPointer(); ++pCharacter)
    {
      if (!((*pCharacter >= 'A' && *pCharacter <= 'Z') || (*pCharacter >= 'a' && *pCharacter <= 'z') ||
            (*pCharacter >= '0' && *pCharacter <= '9') || *pCharacter == '_'))
        return false;
    }

    return true;
  }
} // namespace

plCSharpProjectAssetDocument::plCSharpProjectAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plCSharpProjectAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

void plCSharpProjectAssetDocument::InitializeAfterLoading(bool bFirstTimeCreation)
{
  SUPER::InitializeAfterLoading(bFirstTimeCreation);

  if (!GetProperties()->m_sBuildConfiguration.IsEqual("Development"))
  {
    GetProperties()->m_sBuildConfiguration = "Development";
    ApplyNativePropertyChangesToObjectManager();
    GetCommandHistory()->ClearUndoHistory();
  }

  if (!bFirstTimeCreation)
    return;

  const plStatus status = CreateInitialProject();
  if (status.Failed())
    plLog::Error("Could not initialize the C# script project asset '{}': {}", GetDocumentPath(), status.m_sMessage);
}

plStatus plCSharpProjectAssetDocument::ResolveProjectFile(plStringBuilder& out_sProjectFile) const
{
  const plString& configuredPath = GetProperties()->m_sProjectFile;
  if (configuredPath.IsEmpty())
    return plStatus("No C# project file is configured.");

  out_sProjectFile = configuredPath;
  if (out_sProjectFile.IsAbsolutePath() && plOSFile::ExistsFile(out_sProjectFile))
  {
    out_sProjectFile.MakeCleanPath();
    return plStatus(PL_SUCCESS);
  }

  if (plFileSystem::ResolvePath(configuredPath, &out_sProjectFile, nullptr).Succeeded() && plOSFile::ExistsFile(out_sProjectFile))
  {
    out_sProjectFile.MakeCleanPath();
    return plStatus(PL_SUCCESS);
  }

  out_sProjectFile = GetDocumentPath();
  out_sProjectFile.PathParentDirectory();
  out_sProjectFile.AppendPath(configuredPath);
  out_sProjectFile.MakeCleanPath();
  return plOSFile::ExistsFile(out_sProjectFile)
           ? plStatus(PL_SUCCESS)
           : plStatus(plFmt("C# project file '{}' does not exist.", out_sProjectFile));
}

plStatus plCSharpProjectAssetDocument::ResolveSourceRoot(plStringView sProjectFile, plStringBuilder& out_sSourceRoot) const
{
  out_sSourceRoot = GetProperties()->m_sSourceRoot;
  if (out_sSourceRoot == ".")
  {
    out_sSourceRoot = sProjectFile;
    out_sSourceRoot.PathParentDirectory();
  }
  else if (out_sSourceRoot.IsAbsolutePath())
  {
    out_sSourceRoot.MakeCleanPath();
  }
  else
  {
    plStringBuilder projectDirectory = sProjectFile;
    projectDirectory.PathParentDirectory();
    out_sSourceRoot.Prepend(projectDirectory, "/");
    out_sSourceRoot.MakeCleanPath();
  }

  return plOSFile::ExistsDirectory(out_sSourceRoot)
           ? plStatus(PL_SUCCESS)
           : plStatus(plFmt("C# source root '{}' does not exist.", out_sSourceRoot));
}

plStatus plCSharpProjectAssetDocument::ResolveBindingManifest(plStringView sProjectFile, plStringBuilder& out_sBindingManifest) const
{
  out_sBindingManifest = GetProperties()->m_sBindingManifest;
  if (out_sBindingManifest.IsEmpty())
    out_sBindingManifest = "Intermediate/PlasmaBindings.json";

  if (!out_sBindingManifest.IsAbsolutePath())
  {
    plStringBuilder projectDirectory = sProjectFile;
    projectDirectory.PathParentDirectory();
    out_sBindingManifest.Prepend(projectDirectory, "/");
  }
  out_sBindingManifest.MakeCleanPath();
  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::GetBindingSchemaStampPath(plStringBuilder& out_sPath)
{
  if (!plToolsProject::IsProjectOpen())
    return plStatus("No project is open, so the C# binding-schema dependency has no project root.");

  out_sPath = plToolsProject::GetSingleton()->GetProjectDirectory();
  out_sPath.AppendPath("Intermediate/PlasmaBuild/CSharpBindings/BindingSchema.stamp");
  out_sPath.MakeCleanPath();
  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::GetSourceDependencyStampPath(plStringBuilder& out_sPath)
{
  if (!plToolsProject::IsProjectOpen())
    return plStatus("No project is open, so the C# source dependency has no project root.");

  out_sPath = plToolsProject::GetSingleton()->GetProjectDirectory();
  out_sPath.AppendPath("Intermediate/PlasmaBuild/CSharpBindings/CSharpSourceInputs.stamp");
  out_sPath.MakeCleanPath();
  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::UpdateBindingSchemaStamp(bool* out_pWasWritten)
{
  plScriptBindingSnapshot snapshot;
  plUInt64 uiSchemaHash = 0;
  PL_SUCCEED_OR_RETURN(GetCachedBindingSnapshot(snapshot, uiSchemaHash));

  plStringBuilder stampContent;
  stampContent.SetFormat("{}\n", plArgU(uiSchemaHash, 16, true, 16, false));

  plStringBuilder stampPath;
  PL_SUCCEED_OR_RETURN(GetBindingSchemaStampPath(stampPath));

  plStringBuilder stampDirectory = stampPath;
  stampDirectory.PathParentDirectory();
  if (plOSFile::CreateDirectoryStructure(stampDirectory).Failed())
    return plStatus(plFmt("Could not create C# binding-schema stamp directory '{}'.", stampDirectory));

  plDeferredFileWriter writer;
  writer.SetOutput(stampPath, true);
  if (writer.WriteBytes(stampContent.GetData(), stampContent.GetElementCount()).Failed())
  {
    writer.Discard();
    return plStatus("Could not buffer the C# binding-schema stamp.");
  }

  bool bWasWritten = false;
  if (writer.CloseAtomically(&bWasWritten).Failed())
    return plStatus(plFmt("Could not write C# binding-schema stamp '{}'.", stampPath));

  if (out_pWasWritten != nullptr)
    *out_pWasWritten = bWasWritten;

  if (bWasWritten && plAssetCurator::GetSingleton() != nullptr)
    plAssetCurator::GetSingleton()->NotifyOfFileChange(stampPath);

  return plStatus(PL_SUCCESS);
}

void plCSharpProjectAssetDocument::InvalidateBindingSchemaCache()
{
  PL_LOCK(s_BindingSnapshotMutex);
  s_CachedBindingSnapshot.Clear();
  s_uiCachedBindingSchemaHash = 0;
  s_bBindingSnapshotValid = false;
}

plStatus plCSharpProjectAssetDocument::UpdateSourceDependencyStamp(bool* out_pWasWritten)
{
  plCSharpSourceDependencyTracker* pTracker =
    plCSharpSourceDependencyTracker::GetSingleton();
  if (pTracker == nullptr)
    return plStatus("The C# source dependency tracker is not initialized.");

  return pTracker->EnsureCurrent(out_pWasWritten);
}

plStatus plCSharpProjectAssetDocument::ExportBindingManifest(plStringView sBindingManifest, plUInt64& out_uiSchemaHash) const
{
  plScriptBindingSnapshot snapshot;
  PL_SUCCEED_OR_RETURN(
    GetCachedBindingSnapshot(snapshot, out_uiSchemaHash));

  plStringBuilder directory = sBindingManifest;
  directory.PathParentDirectory();
  if (plOSFile::CreateDirectoryStructure(directory).Failed())
    return plStatus(plFmt("Could not create C# binding-manifest directory '{}'.", directory));

  plDeferredFileWriter writer;
  writer.SetOutput(sBindingManifest);
  if (plScriptBindingManifest::Write(snapshot, writer).Failed() || writer.Close().Failed())
    return plStatus(plFmt("Could not write C# binding manifest '{}'.", sBindingManifest));

  plAssetCurator::GetSingleton()->NotifyOfFileChange(sBindingManifest);

  const plStatus stampStatus = UpdateBindingSchemaStamp();
  if (stampStatus.Failed())
    return stampStatus;

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::RunProjectBuild(
  plStringView sProjectFile, plStringView sBindingManifest,
  plStringView sOutputDirectory, plStringBuilder& out_sDiagnostics) const
{
  // Every build funnels through here, whichever way it was started. The project file is generated
  // once and then kept, so one written before this plugin became a package still references the
  // engine's old Code/ tree and has to be brought forward before dotnet sees it. Doing it on the
  // IDE-open path alone left anyone who only ever presses Build with a project that could not
  // resolve a single Plasma type.
  if (const plStatus migrated = UpdateGeneratedProjectItemMode(sProjectFile); migrated.Failed())
  {
    return migrated;
  }

  plProcessOptions options;
  options.m_sProcess = "dotnet";
  options.AddArgument("build");
  options.AddArgument(sProjectFile);
  options.AddArgument("--nologo");
  options.AddArgument("--configuration");
  options.AddArgument(GetProperties()->m_sBuildConfiguration.IsEmpty() ? plStringView("Development") : GetProperties()->m_sBuildConfiguration.GetView());
  plStringBuilder bindingManifestArgument(
    "-property:PlasmaBindingManifest=", sBindingManifest);
  options.AddArgument(bindingManifestArgument);
  options.AddArgument("--output");
  options.AddArgument(sOutputDirectory);
  options.m_sWorkingDirectory = plPathUtils::GetFileDirectory(sProjectFile);
  options.m_bHideConsoleWindow = true;

  plMutex outputMutex;
  options.m_onStdOut = [&](plStringView sText)
  {
    PL_LOCK(outputMutex);
    out_sDiagnostics.Append(sText);
  };
  options.m_onStdError = [&](plStringView sText)
  {
    PL_LOCK(outputMutex);
    out_sDiagnostics.Append(sText);
  };

  plInt32 iReturnCode = -1;
  if (plProcess::Execute(options, &iReturnCode).Failed())
    return plStatus("Failed to start 'dotnet build'. Install the pinned .NET 10 SDK and ensure dotnet is on PATH.");
  if (iReturnCode != 0)
    return plStatus(plFmt("'dotnet build' failed with exit code {}.", iReturnCode));

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::FindEntryAssembly(
  plStringView sProjectFile, plStringView sOutputDirectory, plStringBuilder& out_sEntryAssembly) const
{
  plProcessOptions options;
  options.m_sProcess = "dotnet";
  options.AddArgument("msbuild");
  options.AddArgument(sProjectFile);
  options.AddArgument("--nologo");
  options.AddArgument("-getProperty:TargetFileName");
  const plStringView configuration =
    GetProperties()->m_sBuildConfiguration.IsEmpty() ? plStringView("Development") : GetProperties()->m_sBuildConfiguration.GetView();
  plStringBuilder configurationArgument("-property:Configuration=", configuration);
  options.AddArgument(configurationArgument);
  options.m_sWorkingDirectory = plPathUtils::GetFileDirectory(sProjectFile);
  options.m_bHideConsoleWindow = true;

  plStringBuilder targetFileName;
  plStringBuilder errorOutput;
  plMutex outputMutex;
  options.m_onStdOut = [&](plStringView sText)
  {
    PL_LOCK(outputMutex);
    targetFileName.Append(sText);
  };
  options.m_onStdError = [&](plStringView sText)
  {
    PL_LOCK(outputMutex);
    errorOutput.Append(sText);
  };

  plInt32 iReturnCode = -1;
  if (plProcess::Execute(options, &iReturnCode).Failed())
    return plStatus("Failed to query the C# project's target assembly name.");
  if (iReturnCode != 0)
    return plStatus(plFmt("Could not query the C# project's target assembly name: {}", errorOutput));

  targetFileName.Trim(" \t\r\n");
  if (targetFileName.IsEmpty() ||
      targetFileName != plPathUtils::GetFileNameAndExtension(targetFileName) ||
      !targetFileName.HasExtension("dll"))
  {
    return plStatus(plFmt("The C# project reported invalid target assembly name '{}'.", targetFileName));
  }

  out_sEntryAssembly.Set(sOutputDirectory, "/", targetFileName);
  out_sEntryAssembly.MakeCleanPath();
  if (!plOSFile::ExistsFile(out_sEntryAssembly))
    return plStatus(plFmt("The C# entry assembly '{}' was not produced by the build.", out_sEntryAssembly));

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::RunInspector(
  plStringView sEntryAssembly, plStringView sDescriptorFile, plStringBuilder& inout_sDiagnostics) const
{
  // Beside this plugin first, then the application directory. Installed from a package the editor
  // plugin lives in the package store rather than next to the editor executable, and the payload
  // travels with it.
  plStringBuilder inspector = plCSharpEditorPaths::FindPayloadRoot();
  inspector.AppendPath("CSharp/Tools/Plasma.ScriptInspector.dll");

  if (!plOSFile::ExistsFile(inspector))
  {
    plStringBuilder inspectorProject(plFileSystem::GetSdkRootDirectory());
    inspectorProject.AppendPath("Code/EditorPlugins/CSharp/Managed/Plasma.ScriptInspector/Plasma.ScriptInspector.csproj");

    plStringBuilder inspectorOutput(plFileSystem::GetSdkRootDirectory());
    inspectorOutput.AppendPath("Intermediate/PlasmaBuild/CSharpInspector");
    PL_SUCCEED_OR_RETURN(plOSFile::CreateDirectoryStructure(inspectorOutput));

    plProcessOptions buildOptions;
    buildOptions.m_sProcess = "dotnet";
    buildOptions.AddArgument("build");
    buildOptions.AddArgument(inspectorProject);
    buildOptions.AddArgument("--nologo");
    buildOptions.AddArgument("--configuration");
    buildOptions.AddArgument("Release");
    buildOptions.AddArgument("--output");
    buildOptions.AddArgument(inspectorOutput);
    buildOptions.m_sWorkingDirectory = plFileSystem::GetSdkRootDirectory();
    buildOptions.m_bHideConsoleWindow = true;

    plMutex buildOutputMutex;
    buildOptions.m_onStdOut = [&](plStringView sText)
    {
      PL_LOCK(buildOutputMutex);
      inout_sDiagnostics.Append(sText);
    };
    buildOptions.m_onStdError = [&](plStringView sText)
    {
      PL_LOCK(buildOutputMutex);
      inout_sDiagnostics.Append(sText);
    };

    plInt32 iBuildReturnCode = -1;
    if (plProcess::Execute(buildOptions, &iBuildReturnCode).Failed() || iBuildReturnCode != 0)
      return plStatus("The managed C# descriptor inspector could not be built.");

    inspector = inspectorOutput;
    inspector.AppendPath("Plasma.ScriptInspector.dll");
  }

  if (!plOSFile::ExistsFile(inspector))
    return plStatus(plFmt("The managed C# descriptor inspector is missing at '{}'.", inspector));

  plProcessOptions options;
  options.m_sProcess = "dotnet";
  options.AddArgument(inspector);
  options.AddArgument("--assembly");
  options.AddArgument(sEntryAssembly);
  options.AddArgument("--output");
  options.AddArgument(sDescriptorFile);
  options.m_sWorkingDirectory = plPathUtils::GetFileDirectory(sEntryAssembly);
  options.m_bHideConsoleWindow = true;

  plMutex outputMutex;
  options.m_onStdOut = [&](plStringView sText)
  {
    PL_LOCK(outputMutex);
    inout_sDiagnostics.Append(sText);
  };
  options.m_onStdError = [&](plStringView sText)
  {
    PL_LOCK(outputMutex);
    inout_sDiagnostics.Append(sText);
  };

  plInt32 iReturnCode = -1;
  if (plProcess::Execute(options, &iReturnCode).Failed())
    return plStatus("Failed to start the managed C# descriptor inspector.");
  if (iReturnCode != 0)
    return plStatus(plFmt("The managed C# descriptor inspector failed with exit code {}.", iReturnCode));

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::GatherBuildFiles(
  plStringView sOutputDirectory, plDynamicArray<BuildFile>& out_files, plDynamicArray<plString>& out_relativeFiles) const
{
  out_files.Clear();
  out_relativeFiles.Clear();

  plStringBuilder searchPath(sOutputDirectory);
  searchPath.MakeCleanPath();
  plFileSystemIterator iterator;
  for (iterator.StartSearch(searchPath, plFileSystemIteratorFlags::ReportFilesRecursive); iterator.IsValid(); iterator.Next())
  {
    plStringBuilder absolutePath = iterator.GetCurrentPath();
    absolutePath.AppendPath(iterator.GetStats().m_sName);

    const plStringView extension = absolutePath.GetFileExtension();
    if (!extension.IsEqual_NoCase("dll") && !extension.IsEqual_NoCase("pdb") && !extension.IsEqual_NoCase("json"))
      continue;
    if (absolutePath.GetFileNameAndExtension().IsEqual_NoCase("Plasma.ScriptDescriptors.json"))
      continue;

    BuildFile& file = out_files.ExpandAndGetRef();
    file.m_sRelativePath = absolutePath;
    if (plStringBuilder relativePath = file.m_sRelativePath; relativePath.MakeRelativeTo(sOutputDirectory).Succeeded())
    {
      relativePath.MakeCleanPath();
      relativePath.ReplaceAll("\\", "/");
      file.m_sRelativePath = relativePath;
    }
    else
    {
      return plStatus(plFmt("C# build output '{}' is outside its staging directory.", absolutePath));
    }

    plFileReader reader;
    if (reader.Open(absolutePath).Failed())
      return plStatus(plFmt("Could not read C# build output '{}'.", absolutePath));

    const plUInt64 uiSize = reader.GetFileSize();
    if (uiSize > plMath::MaxValue<plUInt32>())
      return plStatus(plFmt("C# build output '{}' is too large to package.", absolutePath));

    file.m_Data.SetCountUninitialized(static_cast<plUInt32>(uiSize));
    if (reader.ReadBytes(file.m_Data.GetData(), uiSize) != uiSize)
      return plStatus(plFmt("Could not read all bytes from C# build output '{}'.", absolutePath));
  }

  out_files.Sort([](const BuildFile& a, const BuildFile& b)
    { return a.m_sRelativePath < b.m_sRelativePath; });
  for (const BuildFile& file : out_files)
  {
    out_relativeFiles.PushBack(file.m_sRelativePath);
  }

  return out_files.IsEmpty() ? plStatus("The C# build produced no packageable files.") : plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::GatherSourceFiles(plStringView sSourceRoot, plDynamicArray<plString>& out_relativeSources) const
{
  out_relativeSources.Clear();

  plStringBuilder searchPath(sSourceRoot);
  searchPath.MakeCleanPath();
  plFileSystemIterator iterator;
  for (iterator.StartSearch(searchPath, plFileSystemIteratorFlags::ReportFilesRecursive); iterator.IsValid(); iterator.Next())
  {
    plStringBuilder absolutePath = iterator.GetCurrentPath();
    absolutePath.AppendPath(iterator.GetStats().m_sName);
    if (!absolutePath.GetFileExtension().IsEqual_NoCase("cs"))
      continue;
    if (IsIgnoredSourceDirectory(absolutePath))
      continue;

    plStringBuilder relativePath = absolutePath;
    if (relativePath.MakeRelativeTo(sSourceRoot).Failed())
      return plStatus(plFmt("C# source '{}' is outside source root '{}'.", absolutePath, sSourceRoot));

    relativePath.MakeCleanPath();
    relativePath.ReplaceAll("\\", "/");
    out_relativeSources.PushBack(relativePath);
  }

  out_relativeSources.Sort();
  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::GatherSourceHashes(plStringView sSourceRoot,
  const plDynamicArray<plString>& sourceFiles, plDynamicArray<plUInt64>& out_sourceHashes) const
{
  out_sourceHashes.Clear();
  out_sourceHashes.Reserve(sourceFiles.GetCount());

  for (const plString& sourceFile : sourceFiles)
  {
    plStringBuilder absolutePath(sSourceRoot, "/", sourceFile);
    absolutePath.MakeCleanPath();

    plFileStatus status;
    if (plFileSystemModel::GetSingleton()->HashFile(absolutePath, status).Failed())
      return plStatus(plFmt("Could not fingerprint C# source '{}'.", absolutePath));

    out_sourceHashes.PushBack(status.m_uiHash);
  }

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::ReadTextFile(plStringView sFile, plStringBuilder& out_sText) const
{
  plFileReader reader;
  if (reader.Open(sFile).Failed())
    return plStatus(plFmt("Could not read '{}'.", sFile));
  out_sText.ReadAll(reader);
  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::WriteTextFile(plStringView sFile, plStringView sText) const
{
  plStringBuilder parentDirectory = sFile;
  parentDirectory.PathParentDirectory();
  if (plOSFile::CreateDirectoryStructure(parentDirectory).Failed())
    return plStatus(plFmt("Could not create directory '{}'.", parentDirectory));

  plDeferredFileWriter writer;
  writer.SetOutput(sFile);
  if (writer.WriteBytes(sText.GetStartPointer(), sText.GetElementCount()).Failed() || writer.Close().Failed())
    return plStatus(plFmt("Could not write '{}'.", sFile));

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::CreateScriptFile(
  plStringView sSourceRoot, plStringView sClassName, plStringBuilder& out_sScriptFile) const
{
  if (!IsValidCSharpIdentifier(sClassName))
    return plStatus(plFmt("'{}' is not a valid C# class name.", sClassName));

  out_sScriptFile.Set(sSourceRoot, "/", sClassName, ".cs");
  out_sScriptFile.MakeCleanPath();
  if (plOSFile::ExistsFile(out_sScriptFile))
    return plStatus(plFmt("C# source file '{}' already exists.", out_sScriptFile));

  plStringBuilder source;
  source.Append("using Plasma;\n\n");
  source.Append("namespace Game.Scripts;\n\n");
  source.Append("[PlasmaScript]\n");
  source.Append("public sealed partial class ", sClassName, " : ComponentScript\n");
  source.Append("{\n");
  source.Append("    [Expose]\n");
  source.Append("    public bool Enabled { get; set; } = true;\n\n");
  source.Append("    public override void Update(Time deltaTime)\n");
  source.Append("    {\n");
  source.Append("        if (!Enabled)\n");
  source.Append("            return;\n");
  source.Append("    }\n");
  source.Append("}\n");

  return WriteTextFile(out_sScriptFile, source);
}

plStatus plCSharpProjectAssetDocument::CreateInitialProject()
{
  plStringBuilder documentDirectory = GetDocumentPath();
  documentDirectory.PathParentDirectory();

  plStringBuilder projectName = GetDocumentPath().GetFileName();
  if (!IsValidCSharpIdentifier(projectName))
    projectName = "PlasmaScripts";

  plStringBuilder sourceRoot(documentDirectory);
  PL_SUCCEED_OR_RETURN(plOSFile::CreateDirectoryStructure(sourceRoot));

  // The managed assemblies ship built, beside the plugin - there is no managed source to build
  // against. Referencing csproj files under the engine's Code/ tree only ever worked while this
  // plugin lived in the engine; from a package, and from any launcher-installed SDK, that directory
  // does not exist and every script fails to resolve the Plasma namespace.
  plStringBuilder managedRoot = plCSharpEditorPaths::FindPayloadRoot();
  managedRoot.AppendPath("CSharp");

  plStringBuilder relativeManagedRoot = managedRoot;
  if (relativeManagedRoot.MakeRelativeTo(documentDirectory).Failed())
    relativeManagedRoot = managedRoot;
  relativeManagedRoot.MakeCleanPath();
  relativeManagedRoot.ReplaceAll("\\", "/");

  // The source generator is shipped alongside the game assemblies rather than in the payload root.
  plStringBuilder generatorPath(managedRoot);
  generatorPath.AppendPath("M0Game/Plasma.Engine.Generator.dll");

  plStringBuilder relativeGenerator = generatorPath;
  if (relativeGenerator.MakeRelativeTo(documentDirectory).Failed())
    relativeGenerator = generatorPath;
  relativeGenerator.MakeCleanPath();
  relativeGenerator.ReplaceAll("\\", "/");

  plStringBuilder projectFile(documentDirectory, "/", projectName, ".csproj");
  plStringBuilder projectXml;
  projectXml.Append("<Project Sdk=\"Microsoft.NET.Sdk\">\n");
  projectXml.Append("  <PropertyGroup>\n");
  projectXml.Append("    <TargetFramework>net10.0</TargetFramework>\n");
  projectXml.Append("    <Configurations>Development</Configurations>\n");
  projectXml.Append("    <Configuration>Development</Configuration>\n");
  projectXml.Append("    <ImplicitUsings>enable</ImplicitUsings>\n");
  projectXml.Append("    <Nullable>enable</Nullable>\n");
  projectXml.Append("    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>\n");
  projectXml.Append("    <EnableDefaultItems>false</EnableDefaultItems>\n");
  projectXml.Append("    <PlasmaBindingManifest Condition=\"'$(PlasmaBindingManifest)' == ''\">Intermediate/PlasmaBindings.json</PlasmaBindingManifest>\n");
  projectXml.Append("    <PlasmaSourceRoot>$(MSBuildProjectDirectory)</PlasmaSourceRoot>\n");
  projectXml.Append("    <AssemblyName>", projectName, "</AssemblyName>\n");
  projectXml.Append("    <RootNamespace>Game.Scripts</RootNamespace>\n");
  projectXml.Append("  </PropertyGroup>\n\n");
  projectXml.Append("  <ItemGroup>\n");
  projectXml.Append("    <Compile Include=\"**/*.cs\" Exclude=\"**/.git/**/*.cs;**/AssetCache/**/*.cs;**/bin/**/*.cs;**/Intermediate/**/*.cs;**/obj/**/*.cs\" />\n");
  projectXml.Append("    <CompilerVisibleProperty Include=\"PlasmaSourceRoot\" />\n");
  projectXml.Append("    <AdditionalFiles Include=\"$(PlasmaBindingManifest)\" />\n");
  // Private, so the script assembly does not re-export the engine API to anything referencing it.
  // The .xml beside the DLL gives IntelliSense the doc comments without any further wiring.
  projectXml.Append("    <Reference Include=\"Plasma.ScriptCore\">\n");
  projectXml.Append("      <HintPath>", relativeManagedRoot, "/Plasma.ScriptCore.dll</HintPath>\n");
  projectXml.Append("      <Private>false</Private>\n");
  projectXml.Append("    </Reference>\n");
  projectXml.Append("    <Analyzer Include=\"", relativeGenerator, "\" />\n");
  projectXml.Append("  </ItemGroup>\n");
  projectXml.Append("</Project>\n");

  // The project is an implementation detail of the C# asset workflow. Keep it
  // deterministic and migrate older generated variants whenever it is opened.
  PL_SUCCEED_OR_RETURN(WriteTextFile(projectFile, projectXml));

  plDynamicArray<plString> existingSources;
  PL_SUCCEED_OR_RETURN(GatherSourceFiles(sourceRoot, existingSources));
  plStringBuilder starterScript;
  if (existingSources.IsEmpty())
  {
    PL_SUCCEED_OR_RETURN(CreateScriptFile(sourceRoot, "StarterScript", starterScript));
  }

  plCSharpProjectAssetProperties* pProperties = GetProperties();
  plStringBuilder configuredProjectFile = projectFile;
  configuredProjectFile.MakeRelativeTo(documentDirectory).IgnoreResult();
  configuredProjectFile.ReplaceAll("\\", "/");
  pProperties->m_sProjectFile = configuredProjectFile;
  pProperties->m_sSourceRoot = ".";
  pProperties->m_sBuildConfiguration = "Development";
  pProperties->m_sBindingManifest = "Intermediate/PlasmaBindings.json";
  ApplyNativePropertyChangesToObjectManager();
  GetCommandHistory()->ClearUndoHistory();

  plStringBuilder initialBindingManifest(documentDirectory, "/Intermediate/PlasmaBindings.json");
  plUInt64 uiInitialSchemaHash = 0;
  const plStatus manifestStatus = ExportBindingManifest(initialBindingManifest, uiInitialSchemaHash);
  if (manifestStatus.Failed())
  {
    plLog::Warning("The initial C# binding manifest could not be generated yet: {}", manifestStatus.m_sMessage);
  }

  plAssetCurator::GetSingleton()->NotifyOfFileChange(projectFile);
  if (!starterScript.IsEmpty())
    plAssetCurator::GetSingleton()->NotifyOfFileChange(starterScript);
  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::UpdateBuildCache(plObjectCommandAccessor& accessor, plStringView sDescriptorJson,
  const plDynamicArray<plString>& sourceFiles,
  const plDynamicArray<plUInt64>& sourceHashes, const plDynamicArray<plString>& buildFiles, plStringView sEntryAssembly,
  plUInt64 uiSchemaHash, plStringView sDiagnostics)
{
  if (sourceFiles.GetCount() != sourceHashes.GetCount())
    return plStatus("The C# source compilation receipt is incomplete.");

  const plDocumentObject* pObject = GetPropertyObject();
  if (accessor.SetValueByName(pObject, "LastGoodDescriptorJson", sDescriptorJson).Failed() ||
      accessor.SetValueByName(pObject, "LastGoodEntryAssembly", sEntryAssembly).Failed() ||
      accessor.SetValueByName(pObject, "LastGoodBindingSchemaHash", uiSchemaHash).Failed() ||
      accessor.SetValueByName(pObject, "BuildDiagnostics", sDiagnostics).Failed() ||
      accessor.ClearByName(pObject, "LastGoodSources").Failed() ||
      accessor.ClearByName(pObject, "LastGoodSourceHashes").Failed() ||
      accessor.ClearByName(pObject, "LastGoodFiles").Failed())
  {
    return plStatus("Could not update the C# project build cache.");
  }

  const plAbstractProperty* pSourcesProperty = accessor.FindPropertyByName(pObject, "LastGoodSources");
  const plAbstractProperty* pSourceHashesProperty = accessor.FindPropertyByName(pObject, "LastGoodSourceHashes");
  const plAbstractProperty* pFilesProperty = accessor.FindPropertyByName(pObject, "LastGoodFiles");
  for (plUInt32 i = 0; i < sourceFiles.GetCount(); ++i)
  {
    if (accessor.InsertValue(pObject, pSourcesProperty, sourceFiles[i], -1).Failed() ||
        accessor.InsertValue(pObject, pSourceHashesProperty, sourceHashes[i], -1).Failed())
    {
      return plStatus("Could not update the C# source cache.");
    }
  }
  for (const plString& file : buildFiles)
  {
    if (accessor.InsertValue(pObject, pFilesProperty, file, -1).Failed())
    {
      return plStatus("Could not update the C# build-file cache.");
    }
  }

  return plStatus(PL_SUCCESS);
}

void plCSharpProjectAssetDocument::UpdateDiagnostics(plStringView sDiagnostics)
{
  if (GetProperties()->m_sBuildDiagnostics == sDiagnostics)
    return;

  plObjectCommandAccessor accessor(GetCommandHistory());
  accessor.StartTransaction("Update C# Build Diagnostics");
  if (accessor.SetValueByName(GetPropertyObject(), "BuildDiagnostics", sDiagnostics).Failed())
    accessor.CancelTransaction();
  else
    accessor.FinishTransaction();
}

void plCSharpProjectAssetDocument::LogBuildOutput(plStringView sDiagnostics, bool bBuildFailed) const
{
  plHybridArray<plStringView, 64> lines;
  sDiagnostics.Split(false, lines, "\r", "\n");
  for (plStringView line : lines)
  {
    line.Trim(" \t");
    if (line.IsEmpty())
      continue;

    if (IsBuildError(line))
      plLog::Error("{}", line);
    else if (IsBuildWarning(line))
      plLog::Warning("{}", line);
    else if (bBuildFailed)
      plLog::Info("{}", line);
  }
}

plTransformStatus plCSharpProjectAssetDocument::InternalTransformAsset(const char* szTargetFile, plStringView sOutputTag,
  const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  plProgressRange progress("Compiling C# Scripts", 6, false);
  progress.BeginNextStep("Checking C# source changes");

  const bool bHadLastGoodOutput = plOSFile::ExistsFile(szTargetFile);
  const auto preserveLastGood = [](plStringView sMessage) -> plTransformStatus
  {
    // This importer only replaces the real target after a complete staged build. Even a first-build
    // failure must not delete output produced concurrently by another editor or processor.
    return plTransformStatus(plTransformResult::FailureKeepOutput, sMessage);
  };

  const plStatus sourceDependencyStatus = UpdateSourceDependencyStamp();
  if (sourceDependencyStatus.Failed())
    return preserveLastGood(sourceDependencyStatus.m_sMessage);

  m_sPreparedEntryAssembly.Clear();
  m_uiPreparedBindingSchemaHash = 0;
  m_PreparedFiles.Clear();
  m_PreparedClasses.Clear();

  plStringBuilder projectFile;
  plStringBuilder sourceRoot;
  plStringBuilder bindingManifest;
  plStringBuilder diagnostics;
  plStatus status = ResolveProjectFile(projectFile);
  if (status.Succeeded())
    status = ResolveSourceRoot(projectFile, sourceRoot);
  if (status.Succeeded())
    status = ResolveBindingManifest(projectFile, bindingManifest);

  plStringBuilder stagingDirectory(plFileSystem::GetSdkRootDirectory());
  plStringBuilder guidText;
  plConversionUtils::ToString(GetGuid(), guidText);
  guidText.ReplaceAll(" ", "");
  plStringBuilder uniqueText;
  plConversionUtils::ToString(plUuid::MakeUuid(), uniqueText);
  uniqueText.ReplaceAll(" ", "");
  stagingDirectory.AppendPath("Intermediate/PlasmaBuild/CSharpAssets", guidText, uniqueText);

  if (status.Succeeded() && plOSFile::CreateDirectoryStructure(stagingDirectory).Failed())
    status = plStatus(plFmt("Could not create C# staging directory '{}'.", stagingDirectory));

  PL_SCOPE_EXIT(plOSFile::DeleteFolder(stagingDirectory).IgnoreResult());

  plDynamicArray<plString> sourceFiles;
  plDynamicArray<plUInt64> sourceHashes;
  if (status.Succeeded())
    status = GatherSourceFiles(sourceRoot, sourceFiles);
  if (status.Succeeded())
    status = GatherSourceHashes(sourceRoot, sourceFiles, sourceHashes);

  progress.BeginNextStep("Generating C# engine bindings");
  if (status.Succeeded())
    status = ExportBindingManifest(bindingManifest, m_uiPreparedBindingSchemaHash);

  plStringBuilder compileStep;
  compileStep.SetFormat("Compiling {} C# source file(s) ({})", sourceFiles.GetCount(),
    GetProperties()->m_sBuildConfiguration.IsEmpty() ? plStringView("Development") : GetProperties()->m_sBuildConfiguration.GetView());
  progress.BeginNextStep(compileStep);
  if (status.Succeeded())
  {
    plLog::Info("Compiling {} C# source file(s) in the '{}' configuration...", sourceFiles.GetCount(),
      GetProperties()->m_sBuildConfiguration.IsEmpty() ? plStringView("Development") : GetProperties()->m_sBuildConfiguration.GetView());
    status = RunProjectBuild(
      projectFile, bindingManifest, stagingDirectory, diagnostics);
  }

  progress.BeginNextStep("Discovering C# script classes");
  plStringBuilder entryAssemblyPath;
  if (status.Succeeded())
    status = FindEntryAssembly(projectFile, stagingDirectory, entryAssemblyPath);

  plStringBuilder descriptorFile(stagingDirectory, "/Plasma.ScriptDescriptors.json");
  if (status.Succeeded())
    status = RunInspector(entryAssemblyPath, descriptorFile, diagnostics);

  plStringBuilder descriptorJson;
  if (status.Succeeded())
    status = ReadTextFile(descriptorFile, descriptorJson);

  if (status.Succeeded())
    status = plCSharpProjectDescriptors::Parse(descriptorJson, GetGuid(), m_sPreparedEntryAssembly, m_PreparedClasses);

  if (status.Succeeded() && !m_sPreparedEntryAssembly.IsEqual(plPathUtils::GetFileNameAndExtension(entryAssemblyPath)))
    status = plStatus("The C# inspector entry assembly does not match the build output.");

  progress.BeginNextStep("Packaging the managed assembly");
  plDynamicArray<plString> buildFiles;
  if (status.Succeeded())
    status = GatherBuildFiles(stagingDirectory, m_PreparedFiles, buildFiles);

  progress.BeginNextStep("Updating C# script asset status");
  plDynamicArray<plString> verifiedSourceFiles;
  plDynamicArray<plUInt64> verifiedSourceHashes;
  if (status.Succeeded())
    status = GatherSourceFiles(sourceRoot, verifiedSourceFiles);
  if (status.Succeeded())
    status = GatherSourceHashes(sourceRoot, verifiedSourceFiles, verifiedSourceHashes);
  if (status.Succeeded() &&
      (sourceFiles != verifiedSourceFiles || sourceHashes != verifiedSourceHashes))
  {
    status = plStatus("C# sources changed while they were compiling. The project will be compiled again with the new revision.");
  }

  if (status.Failed())
    diagnostics.Append("\n", status.m_sMessage);

  // The staging directory is deliberately unique so concurrent editor/processor builds cannot
  // collide. Keep that implementation detail out of the persistent diagnostics cache, otherwise
  // every successful build would modify the asset even when its inputs and diagnostics are stable.
  diagnostics.ReplaceAll(stagingDirectory, "<CSharpBuild>");
  plStringBuilder nativeStagingDirectory = stagingDirectory;
  nativeStagingDirectory.ReplaceAll("/", "\\");
  diagnostics.ReplaceAll(nativeStagingDirectory, "<CSharpBuild>");
  RemoveVolatileBuildDiagnostics(diagnostics);

  if (status.Failed())
  {
    LogBuildOutput(diagnostics, true);

    if (!transformFlags.IsSet(plTransformFlags::BackgroundProcessing))
      UpdateDiagnostics(diagnostics);

    if (bHadLastGoodOutput)
    {
      plLog::Warning("C# build failed; keeping the last-known-good project container '{}'.", szTargetFile);
      return preserveLastGood("C# build failed; kept the last-known-good output.");
    }

    return preserveLastGood(status.m_sMessage.GetView());
  }

  LogBuildOutput(diagnostics, false);

  const plCSharpProjectAssetProperties* pProperties = GetProperties();
  const bool bDescriptorCacheChanged =
    pProperties->m_sLastGoodDescriptorJson != descriptorJson ||
    pProperties->m_LastGoodSources != sourceFiles ||
    pProperties->m_LastGoodSourceHashes != sourceHashes ||
    pProperties->m_LastGoodFiles != buildFiles ||
    pProperties->m_sLastGoodEntryAssembly != m_sPreparedEntryAssembly ||
    pProperties->m_uiLastGoodBindingSchemaHash != m_uiPreparedBindingSchemaHash;
  const bool bDiagnosticsChanged = pProperties->m_sBuildDiagnostics != diagnostics;

  if ((bDescriptorCacheChanged || bDiagnosticsChanged) &&
      transformFlags.IsSet(plTransformFlags::BackgroundProcessing))
    return plTransformStatus(
      plTransformResult::NeedsImport, "C# descriptors, dependencies, or diagnostics changed and must be imported into the asset.");

  plObjectCommandAccessor accessor(GetCommandHistory());
  const bool bUpdateBuildCache = bDescriptorCacheChanged || bDiagnosticsChanged;
  if (bUpdateBuildCache)
  {
    accessor.StartTransaction("Update C# Build Cache");
    status = UpdateBuildCache(
      accessor, descriptorJson, sourceFiles, sourceHashes, buildFiles, m_sPreparedEntryAssembly,
      m_uiPreparedBindingSchemaHash, diagnostics);
    if (status.Failed())
    {
      accessor.CancelTransaction();
      return preserveLastGood(status.m_sMessage.GetView());
    }
  }

  plStringBuilder stagedTarget(szTargetFile, ".csharp-stage-", uniqueText);
  PL_SCOPE_EXIT(plOSFile::DeleteFile(stagedTarget).IgnoreResult());

  const plTransformStatus writeStatus =
    SUPER::InternalTransformAsset(stagedTarget, sOutputTag, pAssetProfile, AssetHeader, transformFlags);
  if (!writeStatus.Succeeded())
  {
    if (bUpdateBuildCache)
      accessor.CancelTransaction();

    return preserveLastGood(
      writeStatus.m_sMessage.IsEmpty() ? plStringView("Could not write the C# project container.") : writeStatus.m_sMessage.GetView());
  }

  if (plOSFile::AtomicReplaceFile(stagedTarget, szTargetFile).Failed())
  {
    if (bUpdateBuildCache)
      accessor.CancelTransaction();

    return preserveLastGood("Could not atomically replace the C# project container.");
  }

  if (bUpdateBuildCache)
    accessor.FinishTransaction();

  if (bDescriptorCacheChanged)
    RefreshCSharpSourceAssets();

  plLog::Success("Compiled C# scripts successfully. {} source asset(s) are up to date.", sourceFiles.GetCount());

  return plTransformStatus(plTransformResult::Success);
}

plTransformStatus plCSharpProjectAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag,
  const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  stream.WriteVersion(1);
  stream << m_sPreparedEntryAssembly;
  stream << m_uiPreparedBindingSchemaHash;

  stream << m_PreparedFiles.GetCount();
  for (const BuildFile& file : m_PreparedFiles)
  {
    stream << file.m_sRelativePath;
    stream << static_cast<plUInt64>(file.m_Data.GetCount());
    PL_SUCCEED_OR_RETURN(stream.WriteBytes(file.m_Data.GetData(), file.m_Data.GetCount()));
  }

  stream << m_PreparedClasses.GetCount();
  for (const plCSharpClassDescriptor& classDescriptor : m_PreparedClasses)
  {
    stream << classDescriptor.m_SubAssetGuid;
    stream << classDescriptor.m_PersistentGuid;
    stream << classDescriptor.m_sTypeIdHex;
    stream << classDescriptor.m_sManagedName;
    stream << classDescriptor.m_Fields.GetCount();

    for (const plCSharpFieldDescriptor& field : classDescriptor.m_Fields)
    {
      stream << field.m_sIdHex;
      stream << field.m_sName;
      stream << field.m_sManagedType;
      stream << field.m_DefaultValue.m_sKind;
      stream << field.m_DefaultValue.m_sValue;

      stream << field.m_DefaultValue.m_Components.GetCount();
      for (const auto& component : field.m_DefaultValue.m_Components)
      {
        stream << component.Key();
        stream << component.Value();
      }

      stream << field.m_Metadata.GetCount();
      for (const auto& metadata : field.m_Metadata)
      {
        stream << metadata.Key();
        stream << metadata.Value();
      }
    }
  }

  return plTransformStatus(PL_SUCCESS);
}

void plCSharpProjectAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  // LastGood* and BuildDiagnostics are persisted derived data. They must not make the output stale
  // immediately after a successful build, so hash only the author-controlled transform settings.
  pInfo->m_uiSettingsHash = ComputeTransformSettingsHash();

  plStringBuilder bindingSchemaStamp;
  if (GetBindingSchemaStampPath(bindingSchemaStamp).Succeeded())
    pInfo->m_TransformDependencies.Insert(bindingSchemaStamp);

  plStringBuilder sourceDependencyStamp;
  if (GetSourceDependencyStampPath(sourceDependencyStamp).Succeeded())
    pInfo->m_TransformDependencies.Insert(sourceDependencyStamp);

  if (plToolsProject::IsProjectOpen())
  {
    plStringBuilder pluginSelection = plToolsProject::GetSingleton()->GetProjectDirectory();
    pluginSelection.AppendPath("Editor/PluginSelection.ddl");
    pluginSelection.MakeCleanPath();
    pInfo->m_TransformDependencies.Insert(pluginSelection);
  }

  plStringBuilder projectFile;
  if (ResolveProjectFile(projectFile).Succeeded())
  {
    pInfo->m_TransformDependencies.Insert(projectFile);

    // The per-project binding manifest is generated by the transform itself. The shared schema
    // and source stamps above invalidate every C# project after reflected API or source changes.
  }

  plCSharpProjectAssetMetaData* pProjectMetaData = PL_DEFAULT_NEW(plCSharpProjectAssetMetaData);
  pProjectMetaData->m_sDescriptorJson = GetProperties()->m_sLastGoodDescriptorJson;
  pProjectMetaData->m_sProjectFile = GetProperties()->m_sProjectFile;
  pProjectMetaData->m_sSourceRoot = GetProperties()->m_sSourceRoot;
  pProjectMetaData->m_LastGoodSources = GetProperties()->m_LastGoodSources;
  pProjectMetaData->m_LastGoodSourceHashes = GetProperties()->m_LastGoodSourceHashes;
  pInfo->m_MetaInfo.PushBack(pProjectMetaData);

  plString entryAssembly;
  plDynamicArray<plCSharpClassDescriptor> classes;
  if (plCSharpProjectDescriptors::Parse(GetProperties()->m_sLastGoodDescriptorJson, GetGuid(), entryAssembly, classes).Failed())
    return;

  for (const plCSharpClassDescriptor& classDescriptor : classes)
  {
    pInfo->m_MetaInfo.PushBack(
      CreateExposedParameters(classDescriptor, classDescriptor.m_SubAssetGuid));
  }
}

plExposedParameters* plCSharpProjectAssetDocument::CreateExposedParameters(
  const plCSharpClassDescriptor& classDescriptor, const plUuid& assetGuid)
{
  plExposedParameters* pExposedParameters = PL_DEFAULT_NEW(plExposedParameters);
  pExposedParameters->m_SubAssetGuid = assetGuid;

  for (const plCSharpFieldDescriptor& field : classDescriptor.m_Fields)
  {
    const plRTTI* pType = GetExposedParameterType(field.m_sManagedType, field.m_DefaultValue.m_sKind);
    if (pType == nullptr)
    {
      plLog::Warning("C# exposed field '{}.{}' uses managed reference type '{}', which is retained in the project "
                     "descriptor but cannot yet be represented by the existing exposed-parameter property grid.",
        classDescriptor.m_sManagedName, field.m_sName, field.m_sManagedType);
      continue;
    }

    plExposedParameter* pParameter = PL_DEFAULT_NEW(plExposedParameter);
    pParameter->m_sName = field.m_sName;
    pParameter->m_DefaultValue = MakeDefaultValue(field);
    pParameter->m_Category = plPropertyCategory::Member;
    pParameter->m_sType = pType->GetTypeName();

    const plString* pMinimum = nullptr;
    const plString* pMaximum = nullptr;
    if (pParameter->m_DefaultValue.IsValid() &&
        field.m_Metadata.TryGetValue("Range.Minimum", pMinimum) &&
        field.m_Metadata.TryGetValue("Range.Maximum", pMaximum))
    {
      const plVariant minimum = ConvertNumericMetadata(*pMinimum, pParameter->m_DefaultValue);
      const plVariant maximum = ConvertNumericMetadata(*pMaximum, pParameter->m_DefaultValue);
      if (minimum.IsValid() && maximum.IsValid())
        pParameter->m_Attributes.PushBack(PL_DEFAULT_NEW(plClampValueAttribute, minimum, maximum));
    }

    const plString* pCategory = nullptr;
    if (field.m_Metadata.TryGetValue("Category", pCategory) && !pCategory->IsEmpty())
      pParameter->m_Attributes.PushBack(PL_DEFAULT_NEW(plGroupAttribute, pCategory->GetData()));

    pExposedParameters->m_Parameters.PushBack(pParameter);
  }

  return pExposedParameters;
}

plUInt64 plCSharpProjectAssetDocument::ComputeTransformSettingsHash() const
{
  const plUuid documentGuid = GetGuid();
  plUInt64 uiHash = plHashingUtils::xxHash64(&documentGuid, sizeof(documentGuid));
  // Bump this whenever generated binding call sites or ScriptCore behavior
  // changes in a way that requires existing project assemblies to be rebuilt.
  constexpr plUInt32 uiSettingsVersion = 3;
  uiHash = plHashingUtils::xxHash64(&uiSettingsVersion, sizeof(uiSettingsVersion), uiHash);

  const auto hashString = [&uiHash](plStringView value)
  {
    const plUInt64 uiLength = value.GetElementCount();
    uiHash = plHashingUtils::xxHash64(&uiLength, sizeof(uiLength), uiHash);
    uiHash = plHashingUtils::xxHash64(value.GetStartPointer(), value.GetElementCount(), uiHash);
  };

  const plCSharpProjectAssetProperties* pProperties = GetProperties();
  hashString(pProperties->m_sProjectFile);
  hashString(pProperties->m_sSourceRoot);
  hashString(pProperties->m_sBuildConfiguration);
  hashString(pProperties->m_sBindingManifest);
  return uiHash;
}

plTransformStatus plCSharpProjectAssetDocument::BuildProject()
{
  plQtUiServices::ShowGlobalStatusBarMessage("Compiling C# scripts...");

  const plStatus status = UpdateSourceDependencyStamp();
  if (status.Failed())
  {
    plQtUiServices::ShowGlobalStatusBarMessage("C# compilation could not start. See the log for details.");
    return plTransformStatus(status);
  }

  const plTransformStatus result = TransformAsset(plTransformFlags::TriggeredManually | plTransformFlags::ForceTransform);
  if (result.Succeeded())
  {
    RefreshCSharpSourceAssets();
    plQtUiServices::ShowGlobalStatusBarMessage(
      plFmt("C# compilation succeeded. {} script file(s) are up to date.", GetProperties()->m_LastGoodSources.GetCount()));
  }
  else
  {
    plQtUiServices::ShowGlobalStatusBarMessage("C# compilation failed. See the log for compiler diagnostics.");
  }

  return result;
}

void plCSharpProjectAssetDocument::OpenProject()
{
  plStringBuilder projectFile;
  const plStatus status = ResolveProjectFile(projectFile);
  if (status.Failed())
  {
    ShowDocumentStatus(plFmt("{}", status.m_sMessage));
    return;
  }

  const plStatus openStatus = OpenCSharpIDE(projectFile);
  if (openStatus.Failed())
  {
    plLog::Warning("Could not open C# project '{}' in the selected IDE: {}", projectFile, openStatus.m_sMessage);
    if (!plQtUiServices::OpenFileInDefaultProgram(projectFile))
      ShowDocumentStatus(plFmt("Could not open the C# project: {}", openStatus.m_sMessage));
  }
}

void plCSharpProjectAssetDocument::OpenScript()
{
  plStringBuilder projectFile;
  plStringBuilder sourceRoot;
  plDynamicArray<plString> sourceFiles;
  plStatus status = ResolveProjectFile(projectFile);
  if (status.Succeeded())
    status = ResolveSourceRoot(projectFile, sourceRoot);
  if (status.Succeeded())
    status = GatherSourceFiles(sourceRoot, sourceFiles);
  if (status.Failed() || sourceFiles.IsEmpty())
  {
    if (status.Failed())
      ShowDocumentStatus(plFmt("{}", status.m_sMessage));
    else
      ShowDocumentStatus("The C# project has no source files.");
    return;
  }

  plString selectedSource = sourceFiles[0];
  if (sourceFiles.GetCount() > 1)
  {
    QStringList choices;
    for (const plString& sourceFile : sourceFiles)
      choices.push_back(plMakeQString(sourceFile));

    bool bAccepted = false;
    const QString selected =
      QInputDialog::getItem(nullptr, "Open C# Script", "Source file:", choices, 0, false, &bAccepted);
    if (!bAccepted)
      return;
    selectedSource = selected.toUtf8().data();
  }

  plStringBuilder scriptFile(sourceRoot, "/", selectedSource);
  OpenScript(scriptFile);
}

void plCSharpProjectAssetDocument::OpenScript(plStringView sScriptFile)
{
  const plStatus status = OpenScriptInIDE(sScriptFile);
  if (status.Failed())
    ShowDocumentStatus(plFmt("{}", status.m_sMessage));
}

plStatus plCSharpProjectAssetDocument::OpenScriptInIDE(plStringView sScriptFile)
{
  plStringBuilder projectFile;
  plStringBuilder sourceRoot;
  plStatus status = ResolveProjectFile(projectFile);
  if (status.Succeeded())
    status = ResolveSourceRoot(projectFile, sourceRoot);
  if (status.Failed())
    return status;

  plStringBuilder scriptFile(sScriptFile);
  scriptFile.MakeCleanPath();
  if (!plOSFile::ExistsFile(scriptFile))
    return plStatus(plFmt("The C# source file '{}' does not exist.", scriptFile));
  if (!plPathUtils::IsSubPath_NoCase(sourceRoot, scriptFile))
    return plStatus(plFmt("The C# source file '{}' is outside this script project.", scriptFile));

  const plStatus openStatus = OpenCSharpIDE(projectFile, scriptFile);
  if (openStatus.Succeeded())
    return plStatus(PL_SUCCESS);

  plLog::Warning(
    "Could not open C# project '{}' and script '{}' in the selected IDE: {}",
    projectFile, scriptFile, openStatus.m_sMessage);
  if (plQtUiServices::OpenFileInDefaultProgram(scriptFile))
    return plStatus(PL_SUCCESS);

  return plStatus(plFmt(
    "Could not open C# project '{}' and script '{}': {}",
    projectFile, scriptFile, openStatus.m_sMessage));
}

void plCSharpProjectAssetDocument::CreateScript()
{
  bool bAccepted = false;
  const QString className = QInputDialog::getText(nullptr, "New C# Script", "Class name:", QLineEdit::Normal, "NewScript", &bAccepted).trimmed();
  if (!bAccepted)
    return;

  const plStringBuilder scriptName = className.toUtf8().data();
  if (!IsValidCSharpIdentifier(scriptName))
  {
    ShowDocumentStatus("The C# script name must be a valid identifier.");
    return;
  }

  plStringBuilder projectFile;
  plStringBuilder sourceRoot;
  plStatus status = ResolveProjectFile(projectFile);
  if (status.Succeeded())
    status = ResolveSourceRoot(projectFile, sourceRoot);
  if (status.Failed())
  {
    ShowDocumentStatus(plFmt("{}", status.m_sMessage));
    return;
  }

  plStringBuilder scriptFile;
  status = CreateScriptFile(sourceRoot, scriptName, scriptFile);
  if (status.Failed())
  {
    ShowDocumentStatus(plFmt("{}", status.m_sMessage));
    return;
  }

  plAssetCurator::GetSingleton()->NotifyOfFileChange(scriptFile);
  SaveDocument().LogFailure();

  const plStatus openStatus = OpenScriptInIDE(scriptFile);
  if (openStatus.Failed())
    ShowDocumentStatus(plFmt("{}", openStatus.m_sMessage));
}
