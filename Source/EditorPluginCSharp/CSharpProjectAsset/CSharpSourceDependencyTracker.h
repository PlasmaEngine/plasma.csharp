#pragma once

#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>
#include <Foundation/Types/Status.h>
#include <ToolsFoundation/FileSystem/FileSystemModel.h>

/// Tracks the project-wide set of C# source inputs without blocking editor startup.
class PL_EDITORPLUGINCSHARP_DLL plCSharpSourceDependencyTracker
{
public:
  static void Startup();
  static void Shutdown();
  static plCSharpSourceDependencyTracker* GetSingleton();

  void ResetProject();
  void CloseProject();
  void RequestFullRefresh();
  void NotifyFileChange(const plFileChangedEvent& event);

  /// Starts or completes pending work. Returns true when the dependency stamp changed.
  bool MainThreadTick();

  /// Used by explicit builds and transforms to guarantee that the dependency stamp is current.
  plStatus EnsureCurrent(bool* out_pWasWritten = nullptr);

  ~plCSharpSourceDependencyTracker();

private:
  plCSharpSourceDependencyTracker();

  struct Impl;
  Impl* m_pImpl = nullptr;
};
