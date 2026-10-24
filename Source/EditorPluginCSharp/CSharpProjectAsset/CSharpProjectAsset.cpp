#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <Core/Scripting/Bindings/ScriptBindingManifest.h>
#include <Core/Scripting/Bindings/ScriptBindingRegistry.h>
#include <Core/Scripting/ScriptApiDocs.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetProcessor.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/GUI/ExposedParameters.h>
#include <EditorFramework/Scripting/ScriptFieldMapper.h>
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
#include <Foundation/Time/Time.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/Progress.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/EngineUi/EnginePrompt.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <GuiFoundation/UIServices/DynamicEnums.h>
#include <GuiFoundation/UIServices/DynamicBitflags.h>
#include <ToolsFoundation/FileSystem/FileSystemModel.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>
#include <ToolsFoundation/Project/ToolsProject.h>

#include <charconv>
#include <regex>

namespace
{
  /// The engine API's descriptions beside the binding manifest, handed to the generator when they exist. The manifest
  /// property is passed unquoted: MSBuild cannot parse quotes nested inside a condition's quotes.
  constexpr const char* s_szScriptDocsItem = "    <AdditionalFiles Include=\"$([System.IO.Path]::GetDirectoryName($(PlasmaBindingManifest)))/PlasmaScriptDocs.json\" "
                                             "Condition=\"Exists('$([System.IO.Path]::GetDirectoryName($(PlasmaBindingManifest)))/PlasmaScriptDocs.json')\" />\n";

  constexpr const char* s_szBrokenScriptDocsItem = "    <AdditionalFiles Include=\"$([System.IO.Path]::GetDirectoryName('$(PlasmaBindingManifest)'))/PlasmaScriptDocs.json\" "
                                                   "Condition=\"Exists('$([System.IO.Path]::GetDirectoryName('$(PlasmaBindingManifest)'))/PlasmaScriptDocs.json')\" />\n";
} // namespace

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

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpProjectAssetProperties, 3, plRTTIDefaultAllocator<plCSharpProjectAssetProperties>)
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
    PL_MEMBER_PROPERTY("LastGoodProjectFileHash", m_uiLastGoodProjectFileHash)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("LastGoodEntryAssemblyHash", m_uiLastGoodEntryAssemblyHash)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("BuildDiagnostics", m_sBuildDiagnostics)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("UsePersistentCompiler", m_bUsePersistentCompiler)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("BuildRequest", m_uiBuildRequest)->AddAttributes(new plHiddenAttribute()),
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

    // Projects generated before packages could be installed into <project>/Plugins compile the
    // whole of that directory, including the source checkout of any package built locally.
    if (contents.FindSubString("**/Plugins/**/*.cs") == nullptr)
    {
      const plUInt32 uiBefore = contents.GetElementCount();

      contents.ReplaceAll("**/obj/**/*.cs\" />", "**/obj/**/*.cs;**/Plugins/**/*.cs\" />");

      if (contents.GetElementCount() != uiBefore)
        bChanged = true;
    }

    // Projects generated before the bindings had doc comments do not hand the generator the API descriptions.
    if (contents.FindSubString("PlasmaScriptDocs.json") == nullptr && contents.FindSubString("    <AdditionalFiles Include=\"$(PlasmaBindingManifest)\" />\n") != nullptr)
    {
      plStringBuilder sWithDocs("    <AdditionalFiles Include=\"$(PlasmaBindingManifest)\" />\n", s_szScriptDocsItem);
      contents.ReplaceAll("    <AdditionalFiles Include=\"$(PlasmaBindingManifest)\" />\n", sWithDocs);
      bChanged = true;
    }

    // An earlier form of that line quoted the manifest inside the condition, which MSBuild cannot parse.
    if (contents.FindSubString(s_szBrokenScriptDocsItem) != nullptr)
    {
      contents.ReplaceAll(s_szBrokenScriptDocsItem, s_szScriptDocsItem);
      bChanged = true;
    }

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

  plStatus OpenCSharpIDE(plStringView sProjectFile, plStringView sScriptFile = {}, plUInt32 uiLine = 0, plUInt32 uiColumn = 0)
  {
    PL_SUCCEED_OR_RETURN(UpdateGeneratedProjectItemMode(sProjectFile));

    const plCSharpPreferences* pPreferences =
      plPreferences::QueryPreferences<plCSharpPreferences>();

    switch (pPreferences->m_IDE.GetValue())
    {
      case plCSharpIDE::VisualStudio:
      {
        plHybridArray<plString, 4> arguments;
        arguments.PushBack(sProjectFile);
        if (!sScriptFile.IsEmpty())
          arguments.PushBack(sScriptFile);
        if (uiLine != 0)
        {
          plStringBuilder sGoTo;
          sGoTo.SetFormat("Edit.GoTo {}", uiLine);
          arguments.PushBack("/Command");
          arguments.PushBack(sGoTo);
        }
        return plEditorUiServices::OpenInVisualStudio(arguments);
      }

      case plCSharpIDE::Rider:
      {
        plHybridArray<plString, 4> arguments;
        arguments.PushBack(sProjectFile);
        if (uiLine != 0)
        {
          plStringBuilder sLine;
          sLine.SetFormat("{}", uiLine);
          arguments.PushBack("--line");
          arguments.PushBack(sLine);
        }
        if (!sScriptFile.IsEmpty())
          arguments.PushBack(sScriptFile);
        return plEditorUiServices::OpenInRider(arguments);
      }

      case plCSharpIDE::VisualStudioCode:
      {
        plStringBuilder workspaceFile;
        PL_SUCCEED_OR_RETURN(WriteVsCodeWorkspace(sProjectFile, workspaceFile));

        plHybridArray<plString, 3> arguments;
        arguments.PushBack(workspaceFile);
        if (!sScriptFile.IsEmpty())
        {
          arguments.PushBack("-g");
          plStringBuilder sLocation(sScriptFile);
          if (uiLine != 0)
            sLocation.AppendFormat(":{}:{}", uiLine, plMath::Max(uiColumn, 1U));
          arguments.PushBack(sLocation);
        }
        return plEditorUiServices::OpenInVsCode(arguments);
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

  /// Collects the compiler's own error lines out of a build log.
  ///
  /// A failed background transform reports only its status message to the editor; everything the
  /// compiler said stays in the asset processor's log, in another process, which is not where anyone
  /// looks. Carrying the error lines in the message puts them in front of whoever pressed build.
  void AppendBuildErrors(plStringView sDiagnostics, plStringBuilder& ref_message)
  {
    constexpr plUInt32 uiMaxLines = 20;
    constexpr plUInt32 uiMaxCharacters = 4000;

    plHybridArray<plStringView, 64> lines;
    sDiagnostics.Split(false, lines, "\r", "\n");

    plUInt32 uiWritten = 0;
    for (plStringView line : lines)
    {
      line.Trim(" \t");
      if (line.IsEmpty() || !IsBuildError(line))
        continue;

      if (uiWritten == uiMaxLines || ref_message.GetElementCount() >= uiMaxCharacters)
      {
        ref_message.Append("\n... see the log for the remaining diagnostics.");
        return;
      }

      ref_message.Append("\n", line);
      ++uiWritten;
    }
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
          trimmedLine.StartsWith_NoCase("Restore completed ") ||
          trimmedLine.StartsWith("Compiler stages:") ||
          trimmedLine.StartsWith("C# compiler:"))
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
           clean.FindSubString("/.git/") != nullptr ||
           // Installed packages live under <project>/Plugins, and one built from source keeps its
           // whole checkout in Plugins/.source. That is somebody else's code: compiling it into the
           // game assembly duplicates every type the package already ships, and indexing it turns
           // hundreds of plugin files into project script assets.
           clean.FindSubString("/plugins/") != nullptr;
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

  /// The C# keywords, and the contextual keywords that current C# versions reject as a type name ('record' only warns).
  bool IsCSharpKeyword(plStringView sName)
  {
    static const char* s_Keywords[] = {
      "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const", "continue",
      "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern", "false", "finally",
      "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
      "long", "namespace", "new", "null", "object", "operator", "out", "override", "params", "private", "protected",
      "public", "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string",
      "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort",
      "using", "virtual", "void", "volatile", "while",
      "extension", "file", "record", "required", "scoped"};

    for (const char* szKeyword : s_Keywords)
    {
      if (sName == szKeyword)
        return true;
    }

    return false;
  }

  /// A new script's file: the class name with ".cs", directly in the source root.
  void MakeScriptFilePath(plStringView sSourceRoot, plStringView sClassName, plStringBuilder& out_sScriptFile)
  {
    out_sScriptFile.Set(sSourceRoot, "/", sClassName, ".cs");
    out_sScriptFile.MakeCleanPath();
  }

  /// The source file, relative to the source root, that a new script of this class would clash with; empty when there
  /// is none. Any folder counts: the script template declares a partial class, so two files of one name would merge.
  plString FindScriptFileClash(plStringView sSourceRoot, const plDynamicArray<plString>& existingSources, plStringView sClassName)
  {
    plStringBuilder sScriptFile;
    MakeScriptFilePath(sSourceRoot, sClassName, sScriptFile);
    if (plOSFile::ExistsFile(sScriptFile))
      return sScriptFile.GetFileNameAndExtension();

    for (const plString& sSource : existingSources)
    {
      if (sSource.GetFileNameAndExtension().IsEqual_NoCase(sScriptFile.GetFileNameAndExtension()))
        return sSource;
    }

    return {};
  }

  /// The message the "New C# Script" prompt shows under its field; empty when the name can become a new script.
  plString CheckNewScriptName(plStringView sSourceRoot, const plDynamicArray<plString>& existingSources, plStringView sName)
  {
    plStringBuilder sClassName = sName;
    sClassName.Trim();
    if (!IsValidCSharpIdentifier(sClassName) || IsCSharpKeyword(sClassName))
      return "The C# script name must be a valid identifier.";

    const plString sClash = FindScriptFileClash(sSourceRoot, existingSources, sClassName);
    if (sClash.IsEmpty())
      return {};

    // the path relative to the source root: the full path does not fit the line under the field
    plStringBuilder sMessage;
    sMessage.SetFormat("C# source file '{}' already exists.", sClash);
    return sMessage;
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

  // Only touch the file when the reflected surface actually changed. The manifest is an
  // AdditionalFiles input of the generated project, so a rewritten timestamp alone invalidates
  // CoreCompile and re-runs the binding generator over the whole reflected API.
  plDeferredFileWriter writer;
  writer.SetOutput(sBindingManifest, true);
  if (plScriptBindingManifest::Write(snapshot, writer).Failed() || writer.Close().Failed())
    return plStatus(plFmt("Could not write C# binding manifest '{}'.", sBindingManifest));

  plAssetCurator::GetSingleton()->NotifyOfFileChange(sBindingManifest);

  // The API descriptions travel beside the manifest; the generator turns them into the bindings' doc comments. Missing
  // docs only mean bindings without comments.
  {
    plStringBuilder sDocs = directory;
    sDocs.AppendPath("PlasmaScriptDocs.json");

    plFileReader source;
    if (source.Open(plScriptApiDocs::GetDefaultFile()).Succeeded())
    {
      plDeferredFileWriter docsWriter;
      docsWriter.SetOutput(sDocs, true);

      plUInt8 buffer[4096];
      while (const plUInt64 uiRead = source.ReadBytes(buffer, sizeof(buffer)))
        docsWriter.WriteBytes(buffer, uiRead).IgnoreResult();

      if (docsWriter.Close().Failed())
        plLog::Warning("Could not write the C# API docs to '{}'.", sDocs);
    }
  }

  const plStatus stampStatus = UpdateBindingSchemaStamp();
  if (stampStatus.Failed())
    return stampStatus;

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::RunProjectBuild(
  plStringView sProjectFile, plStringView sBindingManifest,
  plStringView sOutputDirectory, plStringBuilder& out_sDiagnostics, bool bUseCli) const
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
  const plStringView configuration = GetProperties()->m_sBuildConfiguration.IsEmpty() ? plStringView("Development") : GetProperties()->m_sBuildConfiguration.GetView();
  plStringBuilder compiler = plCSharpEditorPaths::FindPayloadRoot();
  compiler.AppendPath("CSharp/Tools/Plasma.ScriptCompiler.dll");
  const bool bPersistentCompiler = !bUseCli && GetProperties()->m_bUsePersistentCompiler && configuration.IsEqual_NoCase("Development") && plOSFile::ExistsFile(compiler);
  if (bPersistentCompiler)
  {
    options.AddArgument(compiler);
    options.AddArgument("--project");
    options.AddArgument(sProjectFile);
    options.AddArgument("--manifest");
    options.AddArgument(sBindingManifest);
  }
  else
  {
    options.AddArgument("build");
    options.AddArgument(sProjectFile);
    options.AddArgument("--nologo");
    plStringBuilder bindingManifestArgument("-property:PlasmaBindingManifest=", sBindingManifest);
    options.AddArgument(bindingManifestArgument);
    if (configuration.IsEqual_NoCase("Development"))
    {
      options.AddArgument("-property:Optimize=false");
      options.AddArgument("-property:DebugSymbols=true");
      options.AddArgument("-property:DebugType=portable");
    }
  }
  options.AddArgument("--configuration");
  options.AddArgument(configuration);
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
  if (bPersistentCompiler && iReturnCode == 2)
  {
    out_sDiagnostics.Append("\nPersistent compilation is unavailable for these project inputs. Using the standard build.\n");
    return RunProjectBuild(sProjectFile, sBindingManifest, sOutputDirectory, out_sDiagnostics, true);
  }
  if (iReturnCode != 0)
    return plStatus(plFmt("'dotnet build' failed with exit code {}.", iReturnCode));

  return plStatus(PL_SUCCESS);
}

plStatus plCSharpProjectAssetDocument::FindEntryAssembly(plStringView sProjectFile, plStringView sOutputDirectory,
  plStringView sCachedTargetFileName, plStringBuilder& out_sEntryAssembly) const
{
  // The assembly name only moves when the project file does, and asking MSBuild for it costs a whole
  // second process. The caller passes the cached name while the project file's fingerprint still matches.
  if (!sCachedTargetFileName.IsEmpty())
  {
    out_sEntryAssembly.Set(sOutputDirectory, "/", sCachedTargetFileName);
    out_sEntryAssembly.MakeCleanPath();
    if (plOSFile::ExistsFile(out_sEntryAssembly))
      return plStatus(PL_SUCCESS);
  }

  // The persistent compiler already evaluated TargetFileName and returned a validated descriptor.
  // Avoid another MSBuild process on first import or after a project configuration change.
  plStringBuilder descriptorPath(sOutputDirectory, "/Plasma.ScriptDescriptors.json");
  plStringBuilder descriptorJson;
  plString descriptorEntry;
  plDynamicArray<plCSharpClassDescriptor> descriptorClasses;
  if (ReadTextFile(descriptorPath, descriptorJson).Succeeded() &&
      plCSharpProjectDescriptors::Parse(descriptorJson, GetGuid(), descriptorEntry, descriptorClasses).Succeeded() &&
      descriptorEntry == plPathUtils::GetFileNameAndExtension(descriptorEntry) && descriptorEntry.HasExtension("dll"))
  {
    out_sEntryAssembly.Set(sOutputDirectory, "/", descriptorEntry);
    out_sEntryAssembly.MakeCleanPath();
    if (plOSFile::ExistsFile(out_sEntryAssembly))
      return plStatus(PL_SUCCESS);
  }

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
    return plStatus(plFmt("The packaged C# descriptor inspector is missing at '{}'. Repair or reinstall plasma.csharp.", inspector));
  }

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

plStatus plCSharpProjectAssetDocument::HashFileContents(plStringView sFile, plUInt64& out_uiHash) const
{
  // Seeded, so an empty file does not fingerprint as 0 - which is also the "never fingerprinted" value
  // every cached hash starts out at.
  plUInt64 uiHash = 0x9E3779B97F4A7C15ULL;
  out_uiHash = 0;

  plOSFile file;
  if (file.Open(sFile, plFileOpenMode::Read).Failed())
    return plStatus(plFmt("Could not open '{}' to fingerprint it.", sFile));

  plDynamicArray<plUInt8> buffer;
  buffer.SetCountUninitialized(64 * 1024);

  while (true)
  {
    const plUInt64 uiRead = file.Read(buffer.GetData(), buffer.GetCount());
    if (uiRead == 0)
      break;

    uiHash = plHashingUtils::xxHash64(buffer.GetData(), static_cast<size_t>(uiRead), uiHash);
  }

  out_uiHash = uiHash;
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

  MakeScriptFilePath(sSourceRoot, sClassName, out_sScriptFile);
  if (plOSFile::ExistsFile(out_sScriptFile))
    return plStatus(plFmt("C# source file '{}' already exists.", out_sScriptFile));

  plStringBuilder source;
  source.Append("using Plasma;\n\n");
  source.Append("namespace Game.Scripts;\n\n");
  plStringBuilder persistentId;
  plConversionUtils::ToString(plUuid::MakeUuid(), persistentId);
  persistentId.ReplaceAll(" ", "");
  source.Append("[PlasmaScript(Id = \"", persistentId, "\")]\n");
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
  projectXml.Append("    <Configurations>Development;Shipping</Configurations>\n");
  projectXml.Append("    <Configuration>Development</Configuration>\n");
  projectXml.Append("    <ImplicitUsings>enable</ImplicitUsings>\n");
  projectXml.Append("    <Nullable>enable</Nullable>\n");
  projectXml.Append("    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>\n");
  // Doc comments must be parsed for exposed fields' XML summaries to become editor tooltips.
  projectXml.Append("    <GenerateDocumentationFile>true</GenerateDocumentationFile>\n");
  projectXml.Append("    <NoWarn>$(NoWarn);CS1591</NoWarn>\n");
  projectXml.Append("    <EnableDefaultItems>false</EnableDefaultItems>\n");
  projectXml.Append("    <PlasmaSeparateBindings>true</PlasmaSeparateBindings>\n");
  projectXml.Append("    <PlasmaBindingManifest Condition=\"'$(PlasmaBindingManifest)' == ''\">Intermediate/PlasmaBindings.json</PlasmaBindingManifest>\n");
  projectXml.Append("    <PlasmaSourceRoot>$(MSBuildProjectDirectory)</PlasmaSourceRoot>\n");
  projectXml.Append("    <AssemblyName>", projectName, "</AssemblyName>\n");
  projectXml.Append("    <RootNamespace>Game.Scripts</RootNamespace>\n");
  projectXml.Append("  </PropertyGroup>\n\n");
  projectXml.Append("  <PropertyGroup Condition=\"'$(Configuration)' == 'Development'\">\n"
                    "    <Optimize>false</Optimize>\n    <DebugSymbols>true</DebugSymbols>\n    <DebugType>portable</DebugType>\n  </PropertyGroup>\n"
                    "  <PropertyGroup Condition=\"'$(Configuration)' == 'Shipping'\">\n    <Optimize>true</Optimize>\n  </PropertyGroup>\n\n");
  projectXml.Append("  <ItemGroup>\n");
  // Plugins/ holds installed packages, and a package built from source keeps its entire checkout in
  // Plugins/.source. Compiling that into the game assembly duplicates every type the package already
  // ships as a reference, which fails in ways that point at the game's own scripts.
  projectXml.Append("    <Compile Include=\"**/*.cs\" Exclude=\"**/.git/**/*.cs;**/AssetCache/**/*.cs;**/bin/**/*.cs;"
                    "**/Intermediate/**/*.cs;**/obj/**/*.cs;**/Plugins/**/*.cs\" />\n");
  projectXml.Append("    <CompilerVisibleProperty Include=\"PlasmaSourceRoot\" />\n");
  projectXml.Append("    <AdditionalFiles Include=\"$(PlasmaBindingManifest)\" />\n");
  // The engine API's descriptions, written beside the manifest; they become the bindings' doc comments.
  projectXml.Append(s_szScriptDocsItem);
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
  plStringBuilder sdkPolicy(documentDirectory, "/global.json");
  if (!plOSFile::ExistsFile(sdkPolicy))
    PL_SUCCEED_OR_RETURN(WriteTextFile(sdkPolicy, "{\"sdk\":{\"version\":\"10.0.301\",\"rollForward\":\"latestFeature\",\"allowPrerelease\":false}}\n"));

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
  plUInt64 uiSchemaHash, plUInt64 uiProjectFileHash, plUInt64 uiEntryAssemblyHash, plStringView sDiagnostics)
{
  if (sourceFiles.GetCount() != sourceHashes.GetCount())
    return plStatus("The C# source compilation receipt is incomplete.");

  const plDocumentObject* pObject = GetPropertyObject();
  if (accessor.SetValueByName(pObject, "LastGoodDescriptorJson", sDescriptorJson).Failed() ||
      accessor.SetValueByName(pObject, "LastGoodEntryAssembly", sEntryAssembly).Failed() ||
      accessor.SetValueByName(pObject, "LastGoodBindingSchemaHash", uiSchemaHash).Failed() ||
      accessor.SetValueByName(pObject, "LastGoodProjectFileHash", uiProjectFileHash).Failed() ||
      accessor.SetValueByName(pObject, "LastGoodEntryAssemblyHash", uiEntryAssemblyHash).Failed() ||
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

void plCSharpProjectAssetDocument::ReportBuildDiagnostics(plStringView sDiagnostics) const
{
  // Tagged lines in the transform log reach the editor's script tools from any process, and replace the project's old set.
  plHybridArray<plScriptDiagnostic, 16> diagnostics;
  ParseBuildDiagnostics(sDiagnostics, diagnostics);

  plStringBuilder sGuid;
  plConversionUtils::ToString(GetGuid(), sGuid);
  plScriptDiagnostics::LogAssetResult(sGuid, "C#", diagnostics);
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

  // Per-stage timing. The inner loop is meant to stay close to a single 'dotnet build', so a stage
  // quietly growing back is the regression worth catching, and it only shows up if it is measured.
  plTime stageStart = plTime::Now();
  plStringBuilder stageTimings;
  PL_SCOPE_EXIT(plLog::Info("C# transform stages: {}", stageTimings));
  const auto stage = [&stageStart, &stageTimings](plStringView sName)
  {
    const plTime now = plTime::Now();
    stageTimings.AppendFormat("{}{} {}ms", stageTimings.IsEmpty() ? "" : ", ", sName,
      plArgF((now - stageStart).GetMilliseconds(), 0));
    stageStart = now;
  };

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

  plStringBuilder stagingDirectory(plToolsProject::GetSingleton()->GetProjectDirectory());
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
  stage("bindings");

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
  stage("build");

  progress.BeginNextStep("Discovering C# script classes");

  // The project file is rewritten by RunProjectBuild's migration step, so it is only worth
  // fingerprinting afterwards.
  plUInt64 uiProjectFileHash = 0;
  if (status.Succeeded())
    status = HashFileContents(projectFile, uiProjectFileHash);

  const plCSharpProjectAssetProperties* pCachedProperties = GetProperties();
  const plStringView cachedTargetFileName =
    (uiProjectFileHash != 0 && uiProjectFileHash == pCachedProperties->m_uiLastGoodProjectFileHash)
      ? pCachedProperties->m_sLastGoodEntryAssembly.GetView()
      : plStringView();

  plStringBuilder entryAssemblyPath;
  if (status.Succeeded())
    status = FindEntryAssembly(projectFile, stagingDirectory, cachedTargetFileName, entryAssemblyPath);

  plUInt64 uiEntryAssemblyHash = 0;
  if (status.Succeeded())
    status = HashFileContents(entryAssemblyPath, uiEntryAssemblyHash);

  // Builds are deterministic, so identical sources produce identical IL. The inspector only ever reads
  // the descriptor table baked into this assembly, and that cannot have changed if the bytes did not.
  const bool bReuseDescriptors = status.Succeeded() && uiEntryAssemblyHash != 0 &&
                                 uiEntryAssemblyHash == pCachedProperties->m_uiLastGoodEntryAssemblyHash &&
                                 !pCachedProperties->m_sLastGoodDescriptorJson.IsEmpty();

  plStringBuilder descriptorFile(stagingDirectory, "/Plasma.ScriptDescriptors.json");
  plStringBuilder descriptorJson;
  if (bReuseDescriptors)
  {
    descriptorJson = pCachedProperties->m_sLastGoodDescriptorJson;
  }
  else
  {
    if (status.Succeeded() && !plOSFile::ExistsFile(descriptorFile))
      status = RunInspector(entryAssemblyPath, descriptorFile, diagnostics);
    if (status.Succeeded())
      status = ReadTextFile(descriptorFile, descriptorJson);
  }
  stage(bReuseDescriptors ? "inspect (cached)" : "inspect");

  if (status.Succeeded())
    status = plCSharpProjectDescriptors::Parse(descriptorJson, GetGuid(), m_sPreparedEntryAssembly, m_PreparedClasses);

  if (status.Succeeded() && !m_sPreparedEntryAssembly.IsEqual(plPathUtils::GetFileNameAndExtension(entryAssemblyPath)))
    status = plStatus("The C# inspector entry assembly does not match the build output.");

  progress.BeginNextStep("Packaging the managed assembly");
  plDynamicArray<plString> buildFiles;
  if (status.Succeeded())
    status = GatherBuildFiles(stagingDirectory, m_PreparedFiles, buildFiles);
  stage("package");

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
    // Logged as an error in its own right. A stage that fails after the compiler succeeded - the
    // inspector, packaging, the source re-check - leaves no compiler diagnostic behind, and
    // LogBuildOutput only promotes lines that look like compiler errors. Without this the log says
    // the build failed and nothing about which step or why.
    plLog::Error("C# build failed during '{}': {}", stageTimings.IsEmpty() ? plStringView("startup") : stageTimings.GetView(),
      status.m_sMessage);

    LogBuildOutput(diagnostics, true);
    ReportBuildDiagnostics(diagnostics);

    if (!transformFlags.IsSet(plTransformFlags::BackgroundProcessing))
      UpdateDiagnostics(diagnostics);

    // The compiler's own errors travel with the status. Everything logged above went to whichever
    // process ran the transform, and for a background build that is the asset processor - a log
    // nobody has open. The status is the one thing that reaches the editor.
    plStringBuilder failureMessage = status.m_sMessage;
    AppendBuildErrors(diagnostics, failureMessage);

    if (bHadLastGoodOutput)
    {
      plLog::Warning("C# build failed; keeping the last-known-good project container '{}'.", szTargetFile);
      failureMessage.Append("\nKept the last-known-good output.");
    }

    return preserveLastGood(failureMessage);
  }

  LogBuildOutput(diagnostics, false);
  ReportBuildDiagnostics(diagnostics);

  const plCSharpProjectAssetProperties* pProperties = GetProperties();
  const bool bDescriptorCacheChanged =
    pProperties->m_sLastGoodDescriptorJson != descriptorJson ||
    pProperties->m_LastGoodSources != sourceFiles ||
    pProperties->m_LastGoodSourceHashes != sourceHashes ||
    pProperties->m_LastGoodFiles != buildFiles ||
    pProperties->m_sLastGoodEntryAssembly != m_sPreparedEntryAssembly ||
    pProperties->m_uiLastGoodBindingSchemaHash != m_uiPreparedBindingSchemaHash ||
    pProperties->m_uiLastGoodProjectFileHash != uiProjectFileHash ||
    pProperties->m_uiLastGoodEntryAssemblyHash != uiEntryAssemblyHash;
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
      m_uiPreparedBindingSchemaHash, uiProjectFileHash, uiEntryAssemblyHash, diagnostics);
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

  stage("write");

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

  // Serialized dependency headers travel with copied projects and templates. Keep project-local
  // paths relative to the data directory instead of pinning the original checkout location.
  const auto AddDependency = [pInfo](plStringBuilder path)
  {
    plStringBuilder relativePath = path;
    if (plEditorApp::GetSingleton()->MakePathDataDirectoryRelative(relativePath))
      path = relativePath;
    pInfo->m_TransformDependencies.Insert(path);
  };

  plStringBuilder bindingSchemaStamp;
  if (GetBindingSchemaStampPath(bindingSchemaStamp).Succeeded())
    AddDependency(bindingSchemaStamp);

  plStringBuilder sourceDependencyStamp;
  if (GetSourceDependencyStampPath(sourceDependencyStamp).Succeeded())
    AddDependency(sourceDependencyStamp);

  if (plToolsProject::IsProjectOpen())
  {
    // Which plugins a project uses decides the reflected API the bindings expose, so a change there
    // has to force a rebuild. Packages.ddl is where that lives now; PluginSelection.ddl is the
    // pre-package file, still present in a converted project and absent from a new one.
    //
    // Only whichever actually exists is registered. A transform dependency on a missing file puts
    // the asset permanently in MissingTransformDependency, which reads as "can't transform" with no
    // hint that the blocker is a file the project was never going to have.
    const plStringView candidates[] = {"Editor/Packages.ddl", "Editor/PluginSelection.ddl"};

    for (plStringView sCandidate : candidates)
    {
      plStringBuilder sPath = plToolsProject::GetSingleton()->GetProjectDirectory();
      sPath.AppendPath(sCandidate);
      sPath.MakeCleanPath();

      if (plOSFile::ExistsFile(sPath))
        AddDependency(sPath);
    }
  }

  plStringBuilder projectFile;
  if (ResolveProjectFile(projectFile).Succeeded())
  {
    AddDependency(projectFile);

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
    plScriptFieldDesc desc;
    desc.m_sName = field.m_sName;

    // A GameObject field is picked in the scene; the script component resolves it to a handle.
    const bool bObjectField = field.m_sManagedType == "Plasma.GameObject";
    const plRTTI* pType = bObjectField ? plGetStaticRTTI<plString>() : GetExposedParameterType(field.m_sManagedType, field.m_DefaultValue.m_sKind);
    if (pType == nullptr)
    {
      plLog::Warning("C# exposed field '{}.{}' uses managed reference type '{}', which is retained in the project "
                     "descriptor but cannot yet be represented by the existing exposed-parameter property grid.",
        classDescriptor.m_sManagedName, field.m_sName, field.m_sManagedType);
      continue;
    }

    desc.m_sType = pType->GetTypeName();
    desc.m_DefaultValue = bObjectField ? plVariant(plString()) : MakeDefaultValue(field);
    desc.m_bObjectReference = bObjectField;

    const plString* pMinimum = nullptr;
    const plString* pMaximum = nullptr;
    if (desc.m_DefaultValue.IsValid() &&
        field.m_Metadata.TryGetValue("Range.Minimum", pMinimum) &&
        field.m_Metadata.TryGetValue("Range.Maximum", pMaximum))
    {
      desc.m_Minimum = ConvertNumericMetadata(*pMinimum, desc.m_DefaultValue);
      desc.m_Maximum = ConvertNumericMetadata(*pMaximum, desc.m_DefaultValue);
    }

    const plString* pText = nullptr;
    if (field.m_Metadata.TryGetValue("Category", pText))
      desc.m_sGroup = *pText;
    if (field.m_Metadata.TryGetValue("DisplayName", pText))
      desc.m_sDisplayName = *pText;
    if (field.m_Metadata.TryGetValue("Tooltip", pText))
      desc.m_sTooltip = *pText;
    if (field.m_Metadata.TryGetValue("Asset", pText) && pType == plGetStaticRTTI<plString>())
      desc.m_sAssetFilter = *pText;

    plExposedParameter* pParameter = plScriptFieldMapper::CreateParameter(desc);

    const plString* pFlags = nullptr;
    if (field.m_Metadata.TryGetValue("Enum.Flags", pFlags))
    {
      plStringBuilder enumName;
      plConversionUtils::ToString(assetGuid, enumName);
      enumName.Prepend("CSharp.");
      enumName.Append(".", field.m_sManagedType);
      if (*pFlags == "true")
      {
        plDynamicBitflags& flags = plDynamicBitflags::GetDynamicBitflags(enumName);
        flags.Clear();
        bool bSupported = true;
        for (const auto& metadata : field.m_Metadata)
        {
          if (!metadata.Key().StartsWith("Enum.Value."))
            continue;
          plUInt64 uiValue = 0;
          if (ParseUnsigned(metadata.Value(), uiValue).Failed())
          {
            bSupported = false;
            break;
          }
          // Composite aliases and zero are represented by the combination of individual flags.
          if (uiValue == 0 || (uiValue & (uiValue - 1)) != 0)
            continue;
          plUInt32 uiBit = 0;
          while ((uiValue >> uiBit) != 1)
            ++uiBit;
          plStringView name = metadata.Key().GetView();
          name.Shrink(11, 0);
          flags.SetValueAndName(uiBit, name);
        }
        if (bSupported)
          pParameter->m_Attributes.PushBack(PL_DEFAULT_NEW(plDynamicBitflagsAttribute, enumName));
        else
          plLog::Warning("C# flags '{}.{}' contain negative values; using a numeric inspector control.", classDescriptor.m_sManagedName, field.m_sName);
      }
      else
      {
        plDynamicEnum& values = plDynamicEnum::GetDynamicEnum(enumName.GetData());
        values.Clear();
        bool bSupported = true;
        for (const auto& metadata : field.m_Metadata)
        {
          if (!metadata.Key().StartsWith("Enum.Value."))
            continue;
          plInt64 iValue = 0;
          if (ParseSigned(metadata.Value(), iValue).Failed() || iValue < -2147483648LL || iValue > 2147483647LL)
          {
            bSupported = false;
            break;
          }
          plStringView name = metadata.Key().GetView();
          name.Shrink(11, 0);
          values.SetValueAndName(static_cast<plInt32>(iValue), name);
        }
        if (bSupported)
          pParameter->m_Attributes.PushBack(PL_DEFAULT_NEW(plDynamicEnumAttribute, enumName.GetData()));
        else
          plLog::Warning("C# enum '{}.{}' exceeds the named control's 32-bit range; using a numeric inspector control.", classDescriptor.m_sManagedName, field.m_sName);
      }
    }

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
  constexpr plUInt32 uiSettingsVersion = 4;
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
  uiHash = plHashingUtils::xxHash64(&pProperties->m_bUsePersistentCompiler, sizeof(pProperties->m_bUsePersistentCompiler), uiHash);
  uiHash = plHashingUtils::xxHash64(&pProperties->m_uiBuildRequest, sizeof(pProperties->m_uiBuildRequest), uiHash);
  return uiHash;
}

plStatus plCSharpProjectAssetDocument::QueueBuildProject()
{
  PL_SUCCEED_OR_RETURN(UpdateSourceDependencyStamp());
  plObjectCommandAccessor accessor(GetCommandHistory());
  accessor.StartTransaction("Queue C# Build");
  const plStatus status = accessor.SetValueByName(GetPropertyObject(), "BuildRequest", GetProperties()->m_uiBuildRequest + 1);
  if (status.Failed())
  {
    accessor.CancelTransaction();
    return status;
  }
  accessor.FinishTransaction();
  // Manual and automatic builds use the same processor scheduler. Saving publishes the new request
  // revision; subsequent clicks supersede it rather than blocking the UI in TransformAsset.
  plAssetProcessor::GetSingleton()->StartProcessor();
  PL_SUCCEED_OR_RETURN(SaveDocument());
  plAssetCurator::GetSingleton()->NotifyOfAssetChange(GetGuid());
  plEditorUiServices::ShowGlobalStatusBarMessage("C# build queued. Previous successful code remains available.");
  return plStatus(PL_SUCCESS);
}

plTransformStatus plCSharpProjectAssetDocument::BuildProject()
{
  plEditorUiServices::ShowGlobalStatusBarMessage("Compiling C# scripts...");

  const plStatus status = UpdateSourceDependencyStamp();
  if (status.Failed())
  {
    plEditorUiServices::ShowGlobalStatusBarMessage(
      "C# compilation could not start. See the log for details.", plEditorUiServices::Event::TextType::Error);
    return plTransformStatus(status);
  }

  const plTransformStatus result = TransformAsset(plTransformFlags::TriggeredManually | plTransformFlags::ForceTransform);
  if (result.Succeeded())
  {
    RefreshCSharpSourceAssets();
    plEditorUiServices::ShowGlobalStatusBarMessage(
      plFmt("C# compilation succeeded. {} script file(s) are up to date.", GetProperties()->m_LastGoodSources.GetCount()),
      plEditorUiServices::Event::TextType::Success);
  }
  else
  {
    plEditorUiServices::ShowGlobalStatusBarMessage(
      "C# compilation failed. See the log for compiler diagnostics.", plEditorUiServices::Event::TextType::Error);
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
    if (!plEditorUiServices::OpenFileInDefaultProgram(projectFile.GetData()))
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
    bool bAccepted = false;
    const plString selected = plEnginePrompt::GetItem("Open C# Script", "Source file:", sourceFiles, 0, false, &bAccepted);
    if (!bAccepted)
      return;
    selectedSource = selected;
  }

  plStringBuilder scriptFile(sourceRoot, "/", selectedSource);
  OpenScript(scriptFile);
}

void plCSharpProjectAssetDocument::ParseBuildDiagnostics(plStringView sDiagnostics, plDynamicArray<plScriptDiagnostic>& out_diagnostics)
{
  // "File(line,col[,endline,endcol]): error|warning CODE: message [project]"
  static const std::regex diagnosticPattern(
    R"(^\s*(.+?)\((\d+),(\d+)(?:,(\d+),(\d+))?\):\s*(error|warning)\s+(\w+):\s*(.*?)(?:\s+\[.*\])?$)", std::regex::ECMAScript);

  plSet<plString> seen;
  plHybridArray<plStringView, 64> lines;
  sDiagnostics.Split(false, lines, "\r", "\n");

  for (plStringView line : lines)
  {
    line.Trim(" \t");

    const std::string text(line.GetStartPointer(), line.GetElementCount());
    std::smatch match;
    if (!std::regex_match(text, match, diagnosticPattern))
      continue;

    plStringBuilder identity;
    identity.SetFormat("{}:{}:{}:{}", match[1].str().c_str(), match[2].str().c_str(), match[3].str().c_str(), match[7].str().c_str());
    if (seen.Contains(identity))
      continue;
    seen.Insert(identity);

    plScriptDiagnostic& diag = out_diagnostics.ExpandAndGetRef();
    diag.m_sLanguage = "C#";
    diag.m_Severity = match[6].str() == "error" ? plScriptDiagnosticSeverity::Error : plScriptDiagnosticSeverity::Warning;
    diag.m_Origin = plStringView(match[7].str().c_str()).StartsWith("PL") ? plScriptDiagnosticOrigin::Binding : plScriptDiagnosticOrigin::Compile;
    diag.m_sCode = match[7].str().c_str();
    diag.m_sMessage = match[8].str().c_str();
    diag.m_sFile = match[1].str().c_str();
    plConversionUtils::StringToUInt(match[2].str().c_str(), diag.m_uiLine).IgnoreResult();
    plConversionUtils::StringToUInt(match[3].str().c_str(), diag.m_uiColumn).IgnoreResult();
    if (match[4].matched)
      plConversionUtils::StringToUInt(match[4].str().c_str(), diag.m_uiEndLine).IgnoreResult();
    if (match[5].matched)
      plConversionUtils::StringToUInt(match[5].str().c_str(), diag.m_uiEndColumn).IgnoreResult();
  }
}

bool plCSharpProjectAssetDocument::OpenLocation(const plScriptDiagnostic& location)
{
  plStringBuilder sFile = location.m_sFile;
  if (sFile.IsEmpty())
    return false;

  if (!sFile.IsAbsolutePath())
  {
    plStringBuilder sAbsolute;
    if (plFileSystem::ResolvePath(sFile, &sAbsolute, nullptr).Succeeded())
      sFile = sAbsolute;
    else
    {
      plStringBuilder sInProject = plToolsProject::GetSingleton()->GetProjectDirectory();
      sInProject.AppendPath(sFile);
      sFile = sInProject;
    }
  }
  sFile.MakeCleanPath();

  if (!plOSFile::ExistsFile(sFile))
    return false;

  // The project the file belongs to: the asset named by the problem, else the first C# project whose sources contain it.
  plStringBuilder sProjectFile;
  auto findProject = [&](const plAssetInfo* pInfo) -> bool
  {
    if (pInfo == nullptr || pInfo->m_Info == nullptr)
      return false;

    const plCSharpProjectAssetMetaData* pMeta = pInfo->m_Info->GetMetaInfo<plCSharpProjectAssetMetaData>();
    if (pMeta == nullptr || pMeta->m_sProjectFile.IsEmpty())
      return false;

    plStringBuilder sProject = pMeta->m_sProjectFile;
    if (!sProject.IsAbsolutePath())
    {
      sProject = pInfo->m_Path.GetAbsolutePath();
      sProject.PathParentDirectory();
      sProject.AppendPath(pMeta->m_sProjectFile);
    }
    sProject.MakeCleanPath();

    plStringBuilder sProjectDir = sProject.GetFileDirectory();
    if (!plPathUtils::IsSubPath_NoCase(sProjectDir, sFile))
      return false;

    sProjectFile = sProject;
    return true;
  };

  if (!location.m_sAsset.IsEmpty())
  {
    auto pAsset = plAssetCurator::GetSingleton()->FindSubAsset(location.m_sAsset);
    if (pAsset.isValid())
      findProject(pAsset->m_pAssetInfo);
  }

  if (sProjectFile.IsEmpty())
  {
    const plAssetCurator::plLockedAssetTable assets = plAssetCurator::GetSingleton()->GetKnownAssets();
    for (auto it : *assets)
    {
      if (findProject(it.Value()))
        break;
    }
  }

  if (!sProjectFile.IsEmpty() && OpenCSharpIDE(sProjectFile, sFile, location.m_uiLine, location.m_uiColumn).Succeeded())
    return true;

  return plEditorUiServices::OpenFileInDefaultProgram(sFile.GetData());
}

void plCSharpProjectAssetDocument::OpenScript(plStringView sScriptFile)
{
  const plStatus status = OpenScriptInIDE(sScriptFile);
  if (status.Failed())
    ShowDocumentStatus(plFmt("{}", status.m_sMessage));
}

plStatus plCSharpProjectAssetDocument::OpenScriptInIDE(plStringView sScriptFile, plUInt32 uiLine, plUInt32 uiColumn)
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

  const plStatus openStatus = OpenCSharpIDE(projectFile, scriptFile, uiLine, uiColumn);
  if (openStatus.Succeeded())
    return plStatus(PL_SUCCESS);

  plLog::Warning(
    "Could not open C# project '{}' and script '{}' in the selected IDE: {}",
    projectFile, scriptFile, openStatus.m_sMessage);
  if (plEditorUiServices::OpenFileInDefaultProgram(scriptFile.GetData()))
    return plStatus(PL_SUCCESS);

  return plStatus(plFmt(
    "Could not open C# project '{}' and script '{}': {}",
    projectFile, scriptFile, openStatus.m_sMessage));
}

void plCSharpProjectAssetDocument::CreateScript()
{
  // the source root comes first: the prompt refuses names whose file already exists under it
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

  // the files found before a failure still count; the source root itself is checked on disk
  plDynamicArray<plString> existingSources;
  GatherSourceFiles(sourceRoot, existingSources).IgnoreResult();

  bool bAccepted = false;
  const plString className = plEnginePrompt::GetText("New C# Script", "Class name:", "NewScript", &bAccepted, [&sourceRoot, &existingSources](plStringView sText)
    { return CheckNewScriptName(sourceRoot, existingSources, sText); });
  if (!bAccepted)
    return;

  plStringBuilder scriptName = className;
  scriptName.Trim();

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
