#pragma once

#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>
#include <EditorUi/Runtime.h>
#include <Foundation/System/Process.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/UniquePtr.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>

class plCSharpProjectAssetDocument;
class plEditorUiConsoleOutput;
class plEditorUiControlStrip;
class plCSharpBuildView;
class plCSharpProblemsView;
struct plDocumentObjectPropertyEvent;

class PL_EDITORPLUGINCSHARP_DLL plCSharpProjectAssetDocumentWindow : public plDocumentWindow
{
public:
  explicit plCSharpProjectAssetDocumentWindow(plCSharpProjectAssetDocument* pDocument);
  ~plCSharpProjectAssetDocumentWindow();

private:
  void CreateBuildView();
  void CreateSetupPage();
  void CheckSetup();
  void PollSdkProbe();
  void AppendSetupOutput(plStringView sText);
  void UpdateBuildStatus();
  void DocumentObjectEventHandler(const plDocumentObjectPropertyEvent& event);
  void UpdateDiagnostics();

  plCSharpProjectAssetDocument* m_pAssetDocument = nullptr;

  // Engine-rendered build view: a status line above the Problems, Build Output and Setup tabs. One content holds them
  // all: the window's "Build Diagnostics" panel.
  plCSharpBuildView* m_pBuildView = nullptr;
  plEditorUiControlStrip* m_pStatus = nullptr;
  plUInt32 m_uiStatusLabel = 0;
  plCSharpProblemsView* m_pProblems = nullptr;
  plEditorUiConsoleOutput* m_pDiagnostics = nullptr;

  plEditorUiControlStrip* m_pSetupControls = nullptr;
  plUInt32 m_uiCheckSetupButton = 0;
  plEditorUiConsoleOutput* m_pSetupOutput = nullptr;
  plStringBuilder m_sSetupOutput;

  // "dotnet --version" of the setup check: its output arrives on the process' reader threads, the poll timer reports
  // the result once the process has finished. The process is terminated with the window, before the text it writes to
  // is gone (declared last of the four).
  plMutex m_SdkProbeMutex;
  plStringBuilder m_sSdkProbeOutput;
  plStringBuilder m_sSdkProbeErrors;
  plUniquePtr<plProcess> m_pSdkProbe; ///< one per check: a process instance cannot be launched twice
  plUInt32 m_uiSdkProbeTimer = 0;

  plEditorUiTimerGroup m_Timers;
};
