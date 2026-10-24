#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetProcessor.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/EngineShell/WindowContent.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorFramework/Scripting/ScriptTools.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/Process.h>
#include <EditorPluginCSharp/Actions/CSharpActions.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpSourceDependencyTracker.h>
#include <EditorPluginCSharp/Preferences/CSharpPreferences.h>
#include <EditorPluginCSharp/Tests/CSharpTestRunner.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Configuration/Startup.h>
#include <GuiFoundation/Action/ActionMapManager.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/EditActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <EditorUi/Runtime.h>
#include <EditorUi/Widgets.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ToolsFoundation/FileSystem/FileSystemModel.h>
#include <ToolsFoundation/Project/ToolsProject.h>

#include <atomic>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plCSharpIDE, 1)
  PL_ENUM_CONSTANT(plCSharpIDE::VisualStudio),
  PL_ENUM_CONSTANT(plCSharpIDE::Rider),
  PL_ENUM_CONSTANT(plCSharpIDE::VisualStudioCode),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpPreferences, 1, plRTTIDefaultAllocator<plCSharpPreferences>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("IDE", plCSharpIDE, m_IDE),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plCSharpPreferences::plCSharpPreferences()
  : plPreferences(Domain::Application, "C# Scripting")
{
}

namespace
{
  std::atomic_bool s_bRefreshBindingSchema = false;
  std::atomic_bool s_bRefreshSourceAssets = false;

  /// The project whose background build finished and whose metadata the editor is still importing (main thread only).
  plUuid s_ImportingProject;
  plUInt8 s_uiBindingSchemaDeferTicks = 0;
  plEventSubscriptionID s_IdleSubscription = 0;
  plString s_sIconFolder;

  void RequestBindingSchemaRefresh()
  {
    s_bRefreshBindingSchema.store(true, std::memory_order_relaxed);
    // Interactive startup gets one frame before paying for reflected bindings.
    // The headless processor must refresh before its first TransformAllAssets
    // callback, otherwise it can incorrectly accept a stale schema stamp.
    s_uiBindingSchemaDeferTicks =
      plStartup::HasApplicationTag("editorprocessor") ? 0 : 1;
  }

  void InvalidateCSharpAssets()
  {
    plAssetCurator* pCurator = plAssetCurator::GetSingleton();
    if (pCurator == nullptr)
      return;

    plDynamicArray<plUuid> assets;
    {
      const auto knownAssets = pCurator->GetKnownAssets();
      for (auto it = knownAssets->GetIterator(); it.IsValid(); ++it)
      {
        const plAssetInfo* pInfo = it.Value();
        if (pInfo == nullptr)
          continue;

        if (pInfo->m_Info->m_sAssetsDocumentTypeName == plTempHashedString("C# Script Project"))
        {
          assets.PushBack(it.Key());
        }
      }
    }

    for (const plUuid& asset : assets)
      pCurator->NotifyOfAssetChange(asset);
  }

  void RefreshCSharpSourceAssetMetadata()
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
            pInfo->m_Info->m_sAssetsDocumentTypeName == plTempHashedString("C# Script"))
        {
          scriptFiles.PushBack(pInfo->m_Path.GetAbsolutePath());
        }
      }
    }

    for (const plString& scriptFile : scriptFiles)
      pCurator->NotifyOfFileChange(scriptFile);
  }

  void RefreshBindingSchemaStamp()
  {
    const plStatus status = plCSharpProjectAssetDocument::UpdateBindingSchemaStamp();
    if (status.Failed())
      plLog::Warning("Could not update the C# binding-schema dependency: {}", status.m_sMessage);
    else
    {
      // Re-evaluate the project asset even when the reflected schema is
      // unchanged. Generator and ScriptCore revisions can require a rebuild
      // through ComputeTransformSettingsHash without changing that schema.
      InvalidateCSharpAssets();
    }
  }

  void FlushPendingStampUpdates()
  {
    if (!plToolsProject::IsProjectOpen())
    {
      s_bRefreshBindingSchema.store(false, std::memory_order_relaxed);
      return;
    }

    if (plCSharpSourceDependencyTracker::GetSingleton()->MainThreadTick())
      InvalidateCSharpAssets();
    if (s_bRefreshSourceAssets.exchange(false, std::memory_order_relaxed))
      RefreshCSharpSourceAssetMetadata();
    if (s_bRefreshBindingSchema.load(std::memory_order_relaxed))
    {
      if (s_uiBindingSchemaDeferTicks > 0)
      {
        --s_uiBindingSchemaDeferTicks;
      }
      else if (s_bRefreshBindingSchema.exchange(false, std::memory_order_relaxed))
      {
        RefreshBindingSchemaStamp();
      }
    }
  }

  /// The import after a background build happens in the editor and sends no progress event; its end shows here.
  void ReportFinishedImport()
  {
    if (!s_ImportingProject.IsValid())
      return;

    plAssetInfo::TransformState state = plAssetInfo::TransformState::Unknown;
    {
      auto pAsset = plAssetCurator::GetSingleton()->GetSubAsset(s_ImportingProject);
      if (!pAsset.isValid())
      {
        s_ImportingProject = plUuid();
        return;
      }
      state = pAsset->m_pAssetInfo->m_TransformState;
    }

    if (state == plAssetInfo::TransformState::UpToDate)
    {
      s_ImportingProject = plUuid();
      plEditorUiServices::ShowGlobalStatusBarMessage("C# compilation succeeded. Script assets are up to date.", plEditorUiServices::Event::TextType::Success);
    }
    else if (state == plAssetInfo::TransformState::TransformError)
    {
      // The failure has its own message.
      s_ImportingProject = plUuid();
    }
  }

  void EditorIdleEventHandler(plEditorApp*)
  {
    FlushPendingStampUpdates();
    ReportFinishedImport();
  }

  void PluginEventHandler(const plPluginEvent& event)
  {
    if (event.m_EventType == plPluginEvent::BeforePluginChanges)
    {
      plCSharpProjectAssetDocument::InvalidateBindingSchemaCache();
    }
    else if (event.m_EventType == plPluginEvent::AfterPluginChanges)
    {
      RequestBindingSchemaRefresh();
    }
  }

  void ProjectEventHandler(const plToolsProjectEvent& event)
  {
    if (event.m_Type == plToolsProjectEvent::Type::ProjectOpened ||
        event.m_Type == plToolsProjectEvent::Type::ProjectFirstSetup)
    {
      plCSharpProjectAssetDocument::InvalidateBindingSchemaCache();
      RequestBindingSchemaRefresh();
      plCSharpSourceDependencyTracker::GetSingleton()->ResetProject();
      s_bRefreshSourceAssets.store(true, std::memory_order_relaxed);
    }
    else if (event.m_Type == plToolsProjectEvent::Type::ProjectClosed)
    {
      s_bRefreshBindingSchema.store(false, std::memory_order_relaxed);
      s_bRefreshSourceAssets.store(false, std::memory_order_relaxed);
      plCSharpSourceDependencyTracker::GetSingleton()->CloseProject();
    }
  }

  void EngineProcessEventHandler(const plEditorEngineProcessConnection::Event& event)
  {
    if (event.m_Type == plEditorEngineProcessConnection::Event::Type::ProcessMessage &&
        event.m_pMsg != nullptr &&
        plDynamicCast<const plProjectReadyMsgToEditor*>(event.m_pMsg) != nullptr)
    {
      // ProjectReady is sent after the engine process has delivered its complete reflected type list.
      plCSharpProjectAssetDocument::InvalidateBindingSchemaCache();
      RequestBindingSchemaRefresh();
    }
  }

  void FileSystemEventHandler(const plFileChangedEvent& event)
  {
    if (event.m_Type == plFileChangedEvent::Type::ModelReset)
    {
      RequestBindingSchemaRefresh();
      plCSharpSourceDependencyTracker::GetSingleton()->ResetProject();
      s_bRefreshSourceAssets.store(true, std::memory_order_relaxed);
      return;
    }

    if (event.m_Type != plFileChangedEvent::Type::FileAdded &&
        event.m_Type != plFileChangedEvent::Type::FileChanged &&
        event.m_Type != plFileChangedEvent::Type::FileRemoved)
    {
      return;
    }

    plCSharpSourceDependencyTracker::GetSingleton()->NotifyFileChange(event);

    if (plPathUtils::GetFileExtension(event.m_Path.GetAbsolutePath())
          .IsEqual_NoCase("plCSharpProjectAsset"))
    {
      s_bRefreshSourceAssets.store(true, std::memory_order_relaxed);
    }

    if (event.m_Type == plFileChangedEvent::Type::FileRemoved)
    {
      const plStringView path = event.m_Path.GetAbsolutePath();
      const plStringView fileName = plPathUtils::GetFileNameAndExtension(path);
      if (fileName.IsEqual_NoCase("BindingSchema.stamp"))
        RequestBindingSchemaRefresh();
      else if (fileName.IsEqual_NoCase("CSharpSourceInputs.stamp"))
        plCSharpSourceDependencyTracker::GetSingleton()->RequestFullRefresh();
    }
  }

  void AssetProcessorProgressEventHandler(const plAssetProcessorProgressEvent& event)
  {
    if (!plPathUtils::GetFileExtension(event.m_sAssetPath).IsEqual_NoCase("plCSharpProjectAsset"))
      return;

    plString message;
    plEditorUiServices::Event::TextType messageType = plEditorUiServices::Event::TextType::Info;

    if (event.m_Type == plAssetProcessorProgressEvent::Type::ProcessingStarted)
    {
      message = "Compiling C# scripts in the background...";
    }
    else if (event.m_Result.m_Result == plTransformResult::NeedsImport)
    {
      message = "C# compilation finished; updating script metadata...";
    }
    else if (event.m_Result.Succeeded())
    {
      message = "C# compilation succeeded. Script assets are up to date.";
      messageType = plEditorUiServices::Event::TextType::Success;
    }
    else
    {
      messageType = plEditorUiServices::Event::TextType::Error;
      message = "C# compilation failed. See the log for diagnostics.";

      // A background transform runs in the asset processor, so everything it logged went to that
      // process's log. Reprint it here, in the editor, where the person who pressed build is looking.
      plStringBuilder details = event.m_Result.m_sMessage;
      details.ReplaceAll("\r", "");

      plHybridArray<plStringView, 32> lines;
      details.Split(false, lines, "\n");

      plLog::Error("C# compilation failed for '{}'.", event.m_sAssetPath);
      for (plStringView line : lines)
      {
        line.Trim(" \t");
        if (!line.IsEmpty())
          plLog::Error("  {}", line);
      }

      // An open project shows the errors on its Problems page; a background build cannot reach that page itself.
      plEditorUiRuntime::RunAfterFrame([guid = event.m_AssetGuid, sDetails = plString(details)]()
        {
          if (auto* pDoc = plDynamicCast<plCSharpProjectAssetDocument*>(plDocumentManager::GetDocumentByGuid(guid)))
            pDoc->ShowBackgroundBuildDiagnostics(sDetails); });
    }

    const bool bImporting = event.m_Type != plAssetProcessorProgressEvent::Type::ProcessingStarted && event.m_Result.m_Result == plTransformResult::NeedsImport;

    // the event may come from another thread: the status bar is shown on the main thread
    plEditorUiRuntime::RunAfterFrame([message = std::move(message), messageType, bImporting, guid = event.m_AssetGuid]()
      {
        plEditorUiServices::ShowGlobalStatusBarMessage(plFmt("{}", message), messageType);
        s_ImportingProject = bImporting ? guid : plUuid(); });
  }

  /// The package's editor icons (":/AssetIcons/..." are files under EditorIcons): the plugin binary sits in
  /// <package>/Bin/<config>, both in the source tree and in an installed copy.
  plString GetIconFolder()
  {
    plStringBuilder sBinary, sCopy;
    plPlugin::GetPluginPaths("plEditorPluginCSharp", sBinary, sCopy, 0);

    plStringBuilder sFolder = sBinary.GetFileDirectory();
    sFolder.PathParentDirectory(2);
    sFolder.AppendPath("EditorIcons");
    return sFolder;
  }

  /// Attaches the configured IDE's debugger to the engine process, or puts the process id on the clipboard.
  void AttachCSharpDebugger(plUInt64 uiProcessId)
  {
    plStringBuilder sPid;
    sPid.SetFormat("{}", uiProcessId);

    const plCSharpPreferences* pPreferences = plPreferences::QueryPreferences<plCSharpPreferences>();
    if (pPreferences->m_IDE.GetValue() == plCSharpIDE::VisualStudio)
    {
      // The JIT debugger dialog lists the open Visual Studio instances and attaches the chosen one.
      if (plOSFile::ExistsFile("C:/Windows/System32/vsjitdebugger.exe"))
      {
        plProcessOptions options;
        options.m_sProcess = "C:/Windows/System32/vsjitdebugger.exe";
        options.m_Arguments.PushBack("-p");
        options.m_Arguments.PushBack(sPid);
        plProcess process;
        if (process.Launch(options, plProcessLaunchFlags::Detached).Succeeded())
        {
          plEditorUiServices::ShowGlobalStatusBarMessage(plFmt("Attaching Visual Studio to the engine process ({}). While attached, C# edits reload fully.", sPid));
          return;
        }
      }
    }

    plEditorUi::WriteClipboard(sPid);
    plEditorUiServices::ShowGlobalStatusBarMessage(plFmt("Engine process id {} copied. Attach your IDE's .NET debugger to it; while attached, C# edits reload fully.", sPid));
  }

  // The C# project in the engine shell: the build diagnostics get the width, the properties sit on their right.
  void ArrangeCSharpProject(plEditorUiDockManager& ref_panels)
  {
    ref_panels.DockPanel("CSharpProjectAssetProperties", "CSharpBuildDiagnostics", plEditorUiDockSide::Right, 0.3f);
  }

  void OnLoadPlugin()
  {
    s_sIconFolder = GetIconFolder();
    plEditorUi::AddIconFolder(s_sIconFolder);

    plPreferences::QueryPreferences<plCSharpPreferences>();

    auto& buildConfigurations =
      plDynamicStringEnum::CreateDynamicEnum("CSharpBuildConfiguration");
    buildConfigurations.Clear();
    buildConfigurations.AddValidValue("Development");

    plCSharpSourceDependencyTracker::Startup();
    plPlugin::Events().AddEventHandler(PluginEventHandler);
    plToolsProject::s_Events.AddEventHandler(ProjectEventHandler);
    plEditorEngineProcessConnection::s_Events.AddEventHandler(EngineProcessEventHandler);
    plFileSystemModel::GetSingleton()->m_FileChangedEvents.AddEventHandler(FileSystemEventHandler);
    plAssetProcessor::GetSingleton()->m_ProgressEvents.AddEventHandler(AssetProcessorProgressEventHandler);
    s_IdleSubscription = plEditorApp::GetSingleton()->m_IdleEvent.AddEventHandler(EditorIdleEventHandler);

    if (plToolsProject::IsProjectOpen())
    {
      plCSharpProjectAssetDocument::InvalidateBindingSchemaCache();
      RequestBindingSchemaRefresh();
      plCSharpSourceDependencyTracker::GetSingleton()->ResetProject();
      s_bRefreshSourceAssets.store(true, std::memory_order_relaxed);
    }

    plCSharpActions::RegisterActions();

    {
      constexpr const char* szMenuBar = "CSharpProjectAssetMenuBar";
      plActionMapManager::RegisterActionMap(szMenuBar).IgnoreResult();
      plStandardMenus::MapActions(szMenuBar, plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
      plProjectActions::MapActions(szMenuBar);
      plDocumentActions::MapMenuActions(szMenuBar);
      plAssetActions::MapMenuActions(szMenuBar);
      plCommandHistoryActions::MapActions(szMenuBar);
      plCSharpActions::MapActions(szMenuBar, "G.Tools.Document");
    }

    {
      constexpr const char* szToolBar = "CSharpProjectAssetToolBar";
      plActionMapManager::RegisterActionMap(szToolBar).IgnoreResult();
      plCSharpActions::MapActions(szToolBar);
    }

    plWindowContentProvider::SetDefaultArrangement("C# Script Project", &ArrangeCSharpProject);

    if (plScriptTools* pTools = plScriptTools::GetSingleton())
    {
      pTools->RegisterLocationOpener("C#", plScriptTools::LocationOpener(&plCSharpProjectAssetDocument::OpenLocation));

      plScriptTools::ExternalDebugger debugger;
      debugger.m_sLanguage = "C#";
      debugger.m_sLabel = "Attach C# debugger";
      debugger.m_Attach = plMakeDelegate(&AttachCSharpDebugger);
      pTools->RegisterExternalDebugger(debugger);
    }

    plRegisterCSharpTestRunner();
  }

  void OnUnloadPlugin()
  {
    plUnregisterCSharpTestRunner();

    if (plScriptTools* pTools = plScriptTools::GetSingleton())
    {
      pTools->UnregisterLocationOpener("C#");
      pTools->UnregisterExternalDebugger("C#");
    }

    plWindowContentProvider::SetDefaultArrangement("C# Script Project", nullptr);

    plEditorApp::GetSingleton()->m_IdleEvent.RemoveEventHandler(s_IdleSubscription);
    plAssetProcessor::GetSingleton()->m_ProgressEvents.RemoveEventHandler(AssetProcessorProgressEventHandler);
    plFileSystemModel::GetSingleton()->m_FileChangedEvents.RemoveEventHandler(FileSystemEventHandler);
    plEditorEngineProcessConnection::s_Events.RemoveEventHandler(EngineProcessEventHandler);
    plToolsProject::s_Events.RemoveEventHandler(ProjectEventHandler);
    plPlugin::Events().RemoveEventHandler(PluginEventHandler);
    plCSharpSourceDependencyTracker::Shutdown();
    plCSharpActions::UnregisterActions();
    plDynamicStringEnum::RemoveEnum("CSharpBuildConfiguration");

    plEditorUi::RemoveIconFolder(s_sIconFolder);
    s_sIconFolder.Clear();
  }
} // namespace

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}
