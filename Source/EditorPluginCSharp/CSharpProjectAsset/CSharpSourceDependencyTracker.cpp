#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpSourceDependencyTracker.h>
#include <Foundation/IO/FileSystem/DeferredFileWriter.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Strings/PathUtils.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Types/RefCounted.h>
#include <ToolsFoundation/Project/ToolsProject.h>

namespace
{
  struct SourceEntry
  {
    plString m_sAbsolutePath;
    plUInt64 m_uiHash = 0;
  };

  struct SourceInput
  {
    plString m_sIdentity;
    plString m_sAbsolutePath;
    plFileStatus m_Status;
  };

  struct SourceTaskResult : public plRefCounted
  {
    struct HashedInput
    {
      plString m_sIdentity;
      SourceEntry m_Entry;
    };

    plUInt64 m_uiGeneration = 0;
    bool m_bFullRefresh = false;
    plDynamicArray<HashedInput> m_HashedInputs;
    plSet<plString> m_RemovedInputs;
    plString m_sError;
    plUInt32 m_uiCacheHits = 0;
    plUInt32 m_uiCacheMisses = 0;
  };

  static plCSharpSourceDependencyTracker* s_pTracker = nullptr;

  static bool IsIgnoredSourceDirectory(plStringView sPath)
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

  static plString MakeIdentity(const plDataDirPath& path)
  {
    plStringBuilder identity;
    identity.SetFormat("{}:{}", path.GetDataDirIndex(), path.GetDataDirRelativePath());
    return identity;
  }

  static plStatus WriteStamp(
    const plMap<plString, SourceEntry>& entries, bool* out_pWasWritten)
  {
    PL_PROFILE_SCOPE("CSharp.WriteSourceStamp");

    if (out_pWasWritten != nullptr)
      *out_pWasWritten = false;

    plStringBuilder stampContent("pl-csharp-source-inputs/v1\n");
    for (const auto& entry : entries)
    {
      stampContent.AppendFormat(
        "{}\t{}\n", plArgU(entry.Value().m_uiHash, 16, true, 16, false), entry.Key());
    }

    plStringBuilder stampPath;
    PL_SUCCEED_OR_RETURN(
      plCSharpProjectAssetDocument::GetSourceDependencyStampPath(stampPath));

    plStringBuilder stampDirectory = stampPath;
    stampDirectory.PathParentDirectory();
    if (plOSFile::CreateDirectoryStructure(stampDirectory).Failed())
      return plStatus(plFmt(
        "Could not create C# source-dependency stamp directory '{}'.",
        stampDirectory));

    plDeferredFileWriter writer;
    writer.SetOutput(stampPath, true);
    if (writer.WriteBytes(
          stampContent.GetData(), stampContent.GetElementCount())
          .Failed())
    {
      writer.Discard();
      return plStatus("Could not buffer the C# source-dependency stamp.");
    }

    bool bWasWritten = false;
    if (writer.CloseAtomically(&bWasWritten).Failed())
      return plStatus(plFmt(
        "Could not write C# source-dependency stamp '{}'.", stampPath));

    if (out_pWasWritten != nullptr)
      *out_pWasWritten = bWasWritten;

    if (bWasWritten && plAssetCurator::GetSingleton() != nullptr)
      plAssetCurator::GetSingleton()->NotifyOfFileChange(stampPath);

    return plStatus(PL_SUCCESS);
  }
} // namespace

struct plCSharpSourceDependencyTracker::Impl
{
  plMap<plString, SourceEntry> m_Entries;
  plMap<plString, SourceInput> m_PendingUpserts;
  plSet<plString> m_PendingRemovals;
  plSharedPtr<plDelegateTask<void>> m_pTask;
  plSharedPtr<SourceTaskResult> m_pResult;
  plTaskGroupID m_TaskGroup;
  plUInt64 m_uiGeneration = 1;
  bool m_bCacheInitialized = false;
  bool m_bFullRefreshPending = false;
  bool m_bLastWriteChanged = false;

  void CancelTask()
  {
    if (m_pTask != nullptr)
      plTaskSystem::CancelTask(
        m_pTask, plOnTaskRunning::WaitTillFinished).IgnoreResult();

    m_pTask = nullptr;
    m_pResult = nullptr;
    m_TaskGroup.Invalidate();
  }

  void ClearState()
  {
    CancelTask();
    m_Entries.Clear();
    m_PendingUpserts.Clear();
    m_PendingRemovals.Clear();
    m_bCacheInitialized = false;
    m_bFullRefreshPending = false;
    m_bLastWriteChanged = false;
  }

  void CaptureFullRefresh(plDynamicArray<SourceInput>& out_inputs)
  {
    const auto files = plFileSystemModel::GetSingleton()->GetFiles();
    out_inputs.Reserve(files->GetCount());

    for (const auto& file : *files)
    {
      const plStringView absolutePath = file.Key().GetAbsolutePath();
      if (!plPathUtils::GetFileExtension(absolutePath).IsEqual_NoCase("cs") ||
          IsIgnoredSourceDirectory(absolutePath))
      {
        continue;
      }

      SourceInput& input = out_inputs.ExpandAndGetRef();
      input.m_sIdentity = MakeIdentity(file.Key());
      input.m_sAbsolutePath = absolutePath;
      input.m_Status = file.Value();
    }
  }

  void StartPendingTask()
  {
    if (m_pTask != nullptr || !plToolsProject::IsProjectOpen())
      return;

    const bool bFullRefresh = m_bFullRefreshPending;
    plDynamicArray<SourceInput> inputs;
    plSet<plString> removals;

    if (bFullRefresh)
    {
      CaptureFullRefresh(inputs);
      m_bFullRefreshPending = false;
      m_PendingUpserts.Clear();
      m_PendingRemovals.Clear();
    }
    else
    {
      inputs.Reserve(m_PendingUpserts.GetCount());
      for (const auto& pending : m_PendingUpserts)
        inputs.PushBack(pending.Value());

      removals = m_PendingRemovals;
      m_PendingUpserts.Clear();
      m_PendingRemovals.Clear();
    }

    if (!bFullRefresh && inputs.IsEmpty() && removals.IsEmpty())
      return;

    const plUInt64 uiGeneration = m_uiGeneration;
    m_pResult = PL_DEFAULT_NEW(SourceTaskResult);
    m_pResult->m_uiGeneration = uiGeneration;
    m_pResult->m_bFullRefresh = bFullRefresh;
    m_pResult->m_RemovedInputs = std::move(removals);
    plSharedPtr<SourceTaskResult> pResult = m_pResult;

    m_pTask = PL_DEFAULT_NEW(
      plDelegateTask<void>, "CSharpSourceFingerprint",
      plTaskNesting::Never,
      [inputs = std::move(inputs), pResult]() mutable
      {
        PL_PROFILE_SCOPE("CSharp.HashSourceInputs");
        pResult->m_HashedInputs.Reserve(inputs.GetCount());

        for (SourceInput& input : inputs)
        {
          plFileStatus status = input.m_Status;
          bool bCacheHit = false;
          if (plFileSystemModel::GetSingleton()
                ->HashFile(input.m_sAbsolutePath, status, &bCacheHit)
                .Failed())
          {
            plStringBuilder error;
            error.SetFormat(
              "Could not hash C# source input '{}'.", input.m_sAbsolutePath);
            pResult->m_sError = error;
            return;
          }

          auto& output = pResult->m_HashedInputs.ExpandAndGetRef();
          output.m_sIdentity = std::move(input.m_sIdentity);
          output.m_Entry.m_sAbsolutePath = std::move(input.m_sAbsolutePath);
          output.m_Entry.m_uiHash = status.m_uiHash;
          if (bCacheHit)
            ++pResult->m_uiCacheHits;
          else
            ++pResult->m_uiCacheMisses;
        }
      });

    m_TaskGroup = plTaskSystem::StartSingleTask(
      m_pTask, plTaskPriority::FileAccess);
  }

  plStatus CompleteTask(bool* out_pWasWritten)
  {
    if (out_pWasWritten != nullptr)
      *out_pWasWritten = false;

    if (m_pTask == nullptr)
      return plStatus(PL_SUCCESS);

    plTaskSystem::WaitForGroup(m_TaskGroup);
    m_TaskGroup.Invalidate();

    plSharedPtr<SourceTaskResult> pResult = m_pResult;
    m_pTask = nullptr;
    m_pResult = nullptr;

    if (pResult->m_uiGeneration != m_uiGeneration ||
        !plToolsProject::IsProjectOpen())
    {
      return plStatus(PL_SUCCESS);
    }

    if (!pResult->m_sError.IsEmpty())
      return plStatus(pResult->m_sError.GetView());

    if (pResult->m_bFullRefresh)
      m_Entries.Clear();

    for (const plString& identity : pResult->m_RemovedInputs)
      m_Entries.Remove(identity);

    for (auto& input : pResult->m_HashedInputs)
      m_Entries.Insert(
        std::move(input.m_sIdentity), std::move(input.m_Entry));

    m_bCacheInitialized = true;

    bool bWasWritten = false;
    const plStatus status = WriteStamp(m_Entries, &bWasWritten);
    if (status.Succeeded())
    {
      plLog::Debug(
        "C# source fingerprint: {} cache hit(s), {} content hash(es).",
        pResult->m_uiCacheHits, pResult->m_uiCacheMisses);
      m_bLastWriteChanged |= bWasWritten;
    }

    if (out_pWasWritten != nullptr)
      *out_pWasWritten = bWasWritten;
    return status;
  }
};

void plCSharpSourceDependencyTracker::Startup()
{
  if (s_pTracker == nullptr)
    s_pTracker = PL_DEFAULT_NEW(plCSharpSourceDependencyTracker);
}

void plCSharpSourceDependencyTracker::Shutdown()
{
  PL_DEFAULT_DELETE(s_pTracker);
}

plCSharpSourceDependencyTracker*
plCSharpSourceDependencyTracker::GetSingleton()
{
  return s_pTracker;
}

plCSharpSourceDependencyTracker::plCSharpSourceDependencyTracker()
{
  m_pImpl = PL_DEFAULT_NEW(Impl);
}

plCSharpSourceDependencyTracker::~plCSharpSourceDependencyTracker()
{
  m_pImpl->ClearState();
  PL_DEFAULT_DELETE(m_pImpl);
}

void plCSharpSourceDependencyTracker::ResetProject()
{
  ++m_pImpl->m_uiGeneration;
  m_pImpl->ClearState();
  m_pImpl->m_bFullRefreshPending = true;
}

void plCSharpSourceDependencyTracker::CloseProject()
{
  ++m_pImpl->m_uiGeneration;
  m_pImpl->ClearState();
}

void plCSharpSourceDependencyTracker::RequestFullRefresh()
{
  m_pImpl->m_bFullRefreshPending = true;
}

void plCSharpSourceDependencyTracker::NotifyFileChange(
  const plFileChangedEvent& event)
{
  const plStringView absolutePath = event.m_Path.GetAbsolutePath();
  if (!plPathUtils::GetFileExtension(absolutePath).IsEqual_NoCase("cs") ||
      IsIgnoredSourceDirectory(absolutePath))
  {
    return;
  }

  if (!m_pImpl->m_bCacheInitialized ||
      event.m_Type == plFileChangedEvent::Type::ModelReset)
  {
    RequestFullRefresh();
    return;
  }

  const plString identity = MakeIdentity(event.m_Path);
  if (event.m_Type == plFileChangedEvent::Type::FileRemoved)
  {
    m_pImpl->m_PendingUpserts.Remove(identity);
    m_pImpl->m_PendingRemovals.Insert(identity);
  }
  else if (event.m_Type == plFileChangedEvent::Type::FileAdded ||
           event.m_Type == plFileChangedEvent::Type::FileChanged)
  {
    SourceInput input;
    input.m_sIdentity = identity;
    input.m_sAbsolutePath = absolutePath;
    input.m_Status = event.m_Status;
    m_pImpl->m_PendingUpserts.Insert(identity, std::move(input));
    m_pImpl->m_PendingRemovals.Remove(identity);
  }
}

bool plCSharpSourceDependencyTracker::MainThreadTick()
{
  if (m_pImpl->m_pTask != nullptr &&
      plTaskSystem::IsTaskGroupFinished(m_pImpl->m_TaskGroup))
  {
    bool bWasWritten = false;
    const plStatus status = m_pImpl->CompleteTask(&bWasWritten);
    if (status.Failed())
      plLog::Warning(
        "Could not update the C# source dependency: {}", status.m_sMessage);
  }

  m_pImpl->StartPendingTask();

  const bool bChanged = m_pImpl->m_bLastWriteChanged;
  m_pImpl->m_bLastWriteChanged = false;
  return bChanged;
}

plStatus plCSharpSourceDependencyTracker::EnsureCurrent(
  bool* out_pWasWritten)
{
  if (out_pWasWritten != nullptr)
    *out_pWasWritten = false;

  bool bAnyWrite = false;
  if (!m_pImpl->m_bCacheInitialized)
    m_pImpl->m_bFullRefreshPending = true;

  while (true)
  {
    m_pImpl->StartPendingTask();
    if (m_pImpl->m_pTask == nullptr)
      break;

    bool bWasWritten = false;
    const plStatus status = m_pImpl->CompleteTask(&bWasWritten);
    if (status.Failed())
      return status;
    bAnyWrite |= bWasWritten;
  }

  if (out_pWasWritten != nullptr)
    *out_pWasWritten = bAnyWrite;
  return plStatus(PL_SUCCESS);
}
