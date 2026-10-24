#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/ProjectChecks/ProjectChecks.h>
#include <EditorPluginCSharp/CSharpEditorPaths.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <EditorPluginCSharp/Tests/CSharpTestRunner.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/Process.h>
#include <Foundation/Threading/Mutex.h>
#include <ToolsFoundation/Project/ToolsProject.h>

namespace
{
  /// The .csproj files of the project that build scripts: they reference ScriptCore or the generator.
  void FindScriptProjects(plDynamicArray<plString>& out_projects)
  {
    if (!plToolsProject::IsProjectOpen())
      return;

    plFileSystemIterator it;
    for (it.StartSearch(plToolsProject::GetSingleton()->GetProjectDirectory(), plFileSystemIteratorFlags::ReportFilesAndFoldersRecursive); it.IsValid(); it.Next())
    {
      const plStringView sName = it.GetStats().m_sName;
      if (it.GetStats().m_bIsDirectory)
      {
        if (sName.StartsWith(".") || sName == "AssetCache" || sName == "bin" || sName == "obj" || sName == "Intermediate")
          it.SkipFolder();
        continue;
      }

      if (!plPathUtils::HasExtension(sName, "csproj"))
        continue;

      plStringBuilder sPath = it.GetCurrentPath();
      sPath.AppendPath(sName);

      plFileReader file;
      if (file.Open(sPath).Failed())
        continue;

      plStringBuilder sText;
      sText.ReadAll(file);
      if (sText.FindSubString("Plasma.ScriptCore") != nullptr || sText.FindSubString("Plasma.Engine.Generator") != nullptr)
        out_projects.PushBack(sPath);
    }
  }

  plResult RunDotNet(const plProcessOptions& options, plStringBuilder& inout_sOutput, plInt32& out_iExitCode)
  {
    plProcessOptions opt = options;
    opt.m_sProcess = "dotnet";
    opt.m_bHideConsoleWindow = true;

    plMutex outputMutex;
    opt.m_onStdOut = [&](plStringView sText)
    {
      PL_LOCK(outputMutex);
      inout_sOutput.Append(sText);
    };
    opt.m_onStdError = opt.m_onStdOut;

    out_iExitCode = -1;
    return plProcess::Execute(opt, &out_iExitCode);
  }

  plString ToRelative(plStringView sFile)
  {
    plStringBuilder sRelative = sFile;
    sRelative.MakeCleanPath();
    plEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sRelative);
    return sRelative;
  }

  void RunProject(plProjectChecks::Context& ref_context, plStringView sProject, plStringView sInspector, plUInt32& inout_uiPassed, plUInt32& inout_uiFailed)
  {
    const plString sRelativeProject = ToRelative(sProject);
    const plStringView sName = plPathUtils::GetFileName(sProject);

    plStringBuilder sOutput = plOSFile::GetTempDataFolder("PlasmaChecks");
    sOutput.AppendPath("CSharpTests", sName);
    sOutput.MakeCleanPath();

    // Build.
    {
      plProcessOptions options;
      options.AddArgument("build");
      options.AddArgument(sProject);
      options.AddArgument("--nologo");
      options.AddArgument("-c");
      options.AddArgument("Development");
      options.AddArgument("-o");
      options.AddArgument(sOutput);
      options.m_sWorkingDirectory = plPathUtils::GetFileDirectory(sProject);

      plStringBuilder sLog;
      plInt32 iExitCode = -1;
      if (RunDotNet(options, sLog, iExitCode).Failed() || iExitCode != 0)
      {
        ++inout_uiFailed;

        plDynamicArray<plScriptDiagnostic> diagnostics;
        plCSharpProjectAssetDocument::ParseBuildDiagnostics(sLog, diagnostics);

        bool bReported = false;
        for (const plScriptDiagnostic& diag : diagnostics)
        {
          if (diag.m_Severity != plScriptDiagnosticSeverity::Error)
            continue;
          ref_context.Error(diag.m_sMessage, sRelativeProject, diag.m_sFile, diag.m_uiLine);
          ref_context.m_pProblems->PeekBack().m_sLanguage = "C#";
          bReported = true;
        }

        if (!bReported)
          ref_context.Error(plStringBuilder(sName, " does not build. Is the .NET SDK installed?"), sRelativeProject, sRelativeProject);
        return;
      }
    }

    // Run.
    plStringBuilder sAssembly(sOutput, "/", sName, ".dll");
    plStringBuilder sResults(sOutput, "/PlasmaTestResults.json");
    plOSFile::DeleteFile(sResults).IgnoreResult();

    {
      plProcessOptions options;
      options.AddArgument(sInspector);
      options.AddArgument("--test");
      options.AddArgument("--assembly");
      options.AddArgument(sAssembly);
      options.AddArgument("--output");
      options.AddArgument(sResults);
      options.m_sWorkingDirectory = sOutput;

      plStringBuilder sLog;
      plInt32 iExitCode = -1;
      if (RunDotNet(options, sLog, iExitCode).Failed() || !plOSFile::ExistsFile(sResults))
      {
        ++inout_uiFailed;
        ref_context.Error(plStringBuilder("The C# test runner did not start for ", sName, ": ", sLog), sRelativeProject, sRelativeProject);
        return;
      }
    }

    plFileReader file;
    plJSONReader json;
    if (file.Open(sResults).Failed() || json.Parse(file).Failed())
    {
      ++inout_uiFailed;
      ref_context.Error(plStringBuilder("The C# test results of ", sName, " are unreadable"), sRelativeProject, sRelativeProject);
      return;
    }

    const plVariant* pTests = json.GetTopLevelObject().GetValue("tests");
    if (pTests == nullptr || !pTests->IsA<plVariantArray>())
      return;

    for (const plVariant& entry : pTests->Get<plVariantArray>())
    {
      if (!entry.IsA<plVariantDictionary>())
        continue;

      const plVariantDictionary& test = entry.Get<plVariantDictionary>();
      auto text = [&](const char* szKey) -> plString
      {
        const plVariant* pValue = test.GetValue(szKey);
        return pValue != nullptr && pValue->IsA<plString>() ? pValue->Get<plString>() : plString();
      };

      const plVariant* pPassed = test.GetValue("passed");
      const plVariant* pLine = test.GetValue("line");
      const bool bPassed = pPassed != nullptr && pPassed->ConvertTo<bool>();
      const plUInt32 uiLine = pLine != nullptr ? pLine->ConvertTo<plUInt32>() : 0;

      plStringBuilder sTestName(sName, " \xC2\xB7 ", text("name"));
      const plString sFile = text("file").IsEmpty() ? sRelativeProject : ToRelative(text("file"));

      if (bPassed)
      {
        // A passing test has no stack trace to name its file; the project would be a misleading place.
        ++inout_uiPassed;
        ref_context.AddProblem(plScriptDiagnosticSeverity::Info, sTestName, sRelativeProject, text("file").IsEmpty() ? plString() : sFile);
      }
      else
      {
        ++inout_uiFailed;
        sTestName.Append(": ", text("message"));
        ref_context.Error(sTestName, sRelativeProject, sFile, uiLine);
      }

      ref_context.m_pProblems->PeekBack().m_sLanguage = "C#";
    }
  }

  void RunCSharpTests(plProjectChecks::Context& ref_context)
  {
    plStringBuilder sInspector = plCSharpEditorPaths::FindPayloadRoot();
    sInspector.AppendPath("CSharp/Tools/Plasma.ScriptInspector.dll");
    if (!plOSFile::ExistsFile(sInspector))
    {
      ref_context.Error(plStringBuilder("The C# test runner is missing at '", sInspector, "'. Repair or reinstall plasma.csharp."), {});
      return;
    }

    plDynamicArray<plString> projects;
    FindScriptProjects(projects);

    plUInt32 uiPassed = 0;
    plUInt32 uiFailed = 0;
    for (const plString& sProject : projects)
      RunProject(ref_context, sProject, sInspector, uiPassed, uiFailed);

    ref_context.m_sSummary.SetFormat("C#: {} tests in {} projects, {} failed", uiPassed + uiFailed, projects.GetCount(), uiFailed);
  }
} // namespace

void plRegisterCSharpTestRunner()
{
  if (plProjectChecks* pChecks = plProjectChecks::GetSingleton())
    pChecks->RegisterTestRunner("C#", plProjectChecks::Runner(&RunCSharpTests));
}

void plUnregisterCSharpTestRunner()
{
  if (plProjectChecks* pChecks = plProjectChecks::GetSingleton())
    pChecks->UnregisterTestRunner("C#");
}
