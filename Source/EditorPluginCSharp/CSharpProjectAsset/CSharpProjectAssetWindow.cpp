#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAssetWindow.h>
#include <EditorPluginCSharp/CSharpEditorPaths.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Table.h>
#include <EditorUi/Tabs.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Elements/UiElements.h>
#include <GuiFoundation/EngineUi/EngineCVarWidget.h>
#include <GuiFoundation/EngineUi/EngineUiControlStrip.h>

#include <regex>
#include <string>

//////////////////////////////////////////////////////////////////////////
// Problems

/// The build's errors and warnings as an engine-rendered table: severity, code, message and
/// location. Double-clicking a row opens its location in the IDE.
class plCSharpProblemsView : public plEditorUiContent
{
public:
  struct Problem
  {
    plString m_sSeverity;
    plString m_sCode;
    plString m_sMessage;
    plString m_sFile;
    plUInt32 m_uiLine = 0;
    plUInt32 m_uiColumn = 0;

    plString GetLocation() const
    {
      // Project-relative where possible: the absolute path's start is the same for every row and hides the file name.
      plStringBuilder sFile = m_sFile;
      sFile.MakeCleanPath();
      plEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sFile);

      plStringBuilder location;
      location.SetFormat("{}:{}:{}", sFile, m_uiLine, m_uiColumn);
      return location;
    }
  };

  explicit plCSharpProblemsView(plCSharpProjectAssetDocument* pDocument)
    : plEditorUiContent("CSharp.Problems")
    , m_pDocument(pDocument)
    , m_Adapter(*this)
  {
    m_pTable = PL_DEFAULT_NEW(plEditorUiTable, *GetSurface(), "Problems", m_Adapter);

    const plEditorUiTableColumn columns[] = {{"Severity", 80.0f}, {"Code", 80.0f}, {"Message", 420.0f}, {"Location", 260.0f}};
    m_pTable->SetColumns(columns);
    m_pTable->SetPersistKey("CSharp/ProblemsColumns");
    m_pTable->m_Events.AddEventHandler([this](const plEditorUiTableEvent& e)
      {
      if (e.m_Type == plEditorUiTableEvent::Type::LayoutChanged)
        m_bRebuild = true;

      const plUInt32 uiRow = m_pTable->GetCurrentRow();
      if (e.m_Type == plEditorUiTableEvent::Type::SelectionChanged && uiRow < m_Problems.GetCount() && m_OnSelected.IsValid())
        m_OnSelected(m_Problems[uiRow]); });

    GetSurface()->m_PointerEvents.AddEventHandler([this](const plEditorUiPointerEvent& e)
      {
      if (e.m_Type == plEditorUiPointerEvent::Type::DoubleClick)
        OpenCurrent(); });
  }

  ~plCSharpProblemsView() { m_pTable.Clear(); }

  void SetProblems(plDynamicArray<Problem>&& problems)
  {
    m_Problems = std::move(problems);
    m_pTable->Refresh();
    m_bRebuild = true;
  }

  plUInt32 GetCount() const { return m_Problems.GetCount(); }
  plArrayPtr<const Problem> GetAll() const { return m_Problems; }

  /// Called when a row is selected, e.g. to show its place in the code view.
  plDelegate<void(const Problem&)> m_OnSelected;

protected:
  void OnUiUpdate() override
  {
    if (!m_bRebuild)
      return;

    m_bRebuild = false;
    plUiPanel* root = plEditorUi::Panel();
    root->m_Color = plEditorUi::Background();
    m_pTable->Build(root);
    SetDocument(plEditorUi::Finish(root));
  }

private:
  class Adapter : public plEditorUiTableAdapter
  {
  public:
    explicit Adapter(plCSharpProblemsView& ref_view)
      : m_View(ref_view)
    {
    }

    plUInt32 GetRowCount() const override { return m_View.m_Problems.GetCount(); }

    plString GetCell(plUInt32 uiRow, plUInt32 uiColumn) const override
    {
      const Problem& problem = m_View.m_Problems[uiRow];
      switch (uiColumn)
      {
        case 0:
          return problem.m_sSeverity;
        case 1:
          return problem.m_sCode;
        case 2:
          return problem.m_sMessage;
        default:
          return problem.GetLocation();
      }
    }

    void Sort(plUInt32 uiColumn, bool bAscending) override
    {
      m_View.m_Problems.Sort([&](const Problem& a, const Problem& b)
        {
        const plInt32 order = GetCellOf(a, uiColumn).Compare(GetCellOf(b, uiColumn));
        return bAscending ? order < 0 : order > 0; });
    }

  private:
    static plString GetCellOf(const Problem& problem, plUInt32 uiColumn)
    {
      return uiColumn == 0 ? problem.m_sSeverity : uiColumn == 1 ? problem.m_sCode : uiColumn == 2 ? problem.m_sMessage : problem.GetLocation();
    }

    plCSharpProblemsView& m_View;
  };

  void OpenCurrent()
  {
    const plUInt32 uiRow = m_pTable->GetCurrentRow();
    if (uiRow >= m_Problems.GetCount())
      return;

    const Problem& problem = m_Problems[uiRow];
    const plStatus status = m_pDocument->OpenScriptInIDE(problem.m_sFile, problem.m_uiLine, problem.m_uiColumn);
    if (status.Failed())
      plLog::Warning("Could not open C# diagnostic: {}", status.m_sMessage);
  }

  plCSharpProjectAssetDocument* m_pDocument = nullptr;
  plDynamicArray<Problem> m_Problems;
  Adapter m_Adapter;
  plUniquePtr<plEditorUiTable> m_pTable;
  bool m_bRebuild = true;
};

//////////////////////////////////////////////////////////////////////////
// Code

/// The source file of the selected problem, read-only, with every problem of that file marked and the selected one's
/// line in view. Edits happen in the IDE; this shows where a problem is without leaving the editor.
class plCSharpCodeView : public plEditorUiContent
{
public:
  static constexpr const char* s_szLanguage = "C#";

  plCSharpCodeView()
    : plEditorUiContent("CSharp.Code")
  {
    RegisterLanguage();
  }

  ~plCSharpCodeView() { plUiCodeArea::ClearDecorations(s_szArea); }

  void ShowFile(plStringView sFile, plUInt32 uiLine, plArrayPtr<const plCSharpProblemsView::Problem> problems)
  {
    plStringBuilder sCode;
    plFileReader file;
    if (file.Open(sFile).Succeeded())
      sCode.ReadAll(file);
    else
      sCode.SetFormat("// '{}' could not be read.", sFile);

    m_sCode = sCode;
    m_sFile = sFile;
    if (IsReady())
      GetSurface()->GetBlackboard().SetEntryValue(s_szBinding, m_sCode);

    plUiCodeArea::Decorations decorations;
    for (const plCSharpProblemsView::Problem& problem : problems)
    {
      if (!problem.m_sFile.IsEqual_NoCase(sFile) || problem.m_uiLine == 0)
        continue;

      plUiCodeArea::Decorations::Marker& marker = decorations.m_Markers.ExpandAndGetRef();
      marker.m_uiLine = problem.m_uiLine;
      marker.m_uiColumn = problem.m_uiColumn;
      marker.m_uiSeverity = problem.m_sSeverity.IsEqual_NoCase("error") ? 2 : (problem.m_sSeverity.IsEqual_NoCase("warning") ? 1 : 0);
    }
    decorations.m_uiHighlightLine = uiLine;
    decorations.m_uiRevealLine = uiLine;
    plUiCodeArea::SetDecorations(s_szArea, decorations);

    m_bRebuild = true;
    RequestRedraw();
  }

  plStringView GetFile() const { return m_sFile; }

  static void RegisterLanguage()
  {
    if (plUiCodeLanguage::Find(s_szLanguage) != nullptr)
      return;

    plUiCodeLanguage language;
    language.m_sName = s_szLanguage;
    language.AddKeywords(plColorGammaUB(0xC5, 0x86, 0xC0), "break case continue default do for foreach return switch while if else goto yield await throw try catch finally when");
    language.AddKeywords(plColorGammaUB(0x56, 0x9C, 0xCA),
      "abstract as base class const delegate enum event explicit extern false fixed implicit in interface internal is lock namespace new null "
      "operator out override params partial private protected public readonly record ref sealed sizeof stackalloc static struct this true "
      "typeof unchecked unsafe using var virtual void volatile get set init value async nameof");
    language.AddKeywords(plColorGammaUB(0x4E, 0xC9, 0xB0), "bool byte sbyte char decimal double float int uint long ulong short ushort object string");
    plUiCodeLanguage::Register(language);
  }

protected:
  void OnUiUpdate() override
  {
    if (!m_bRebuild)
      return;

    using namespace plEditorUi;
    m_bRebuild = false;

    plUiPanel* root = Panel();
    root->m_Color = Background();

    plUiCodeArea* area = CodeArea(root, s_szArea, s_szBinding, s_szLanguage);
    Fill(area);
    area->m_fBorderWidth = 0;
    area->m_bReadOnly = true;

    if (m_sCode.IsEmpty())
      EmptyState(root, Icon::Source, "No file shown", "Select a problem to see its place in the code.");

    GetSurface()->GetBlackboard().SetEntryValue(s_szBinding, m_sCode);
    SetDocument(Finish(root));
  }

private:
  static constexpr const char* s_szArea = "CSharp.Code.area";
  static constexpr const char* s_szBinding = "CSharp.Code.Text";

  plString m_sCode;
  plString m_sFile;
  bool m_bRebuild = true;
};

//////////////////////////////////////////////////////////////////////////
// Build view

/// The Build Diagnostics panel: the status line, a tab row (Problems, Build Output, Setup) and the current tab's page.
/// The status line and the pages are child contents and the tab row is the view's own document, so the whole view is
/// one content: the window's "Build Diagnostics" panel.
class plCSharpBuildView : public plEditorUiContent
{
public:
  enum Page : plUInt32
  {
    ProblemsPage,
    BuildOutputPage,
    SetupPage,
    PageCount,
  };

  explicit plCSharpBuildView(plCSharpProjectAssetDocument* pDocument)
    : plEditorUiContent("CSharp.Build")
  {
    if (!IsReady())
      return;

    m_pStatus = AddPart<plEditorUiControlStrip>();
    m_pProblems = AddPart<plCSharpProblemsView>(pDocument);
    m_pCode = AddPart<plCSharpCodeView>();
    m_pProblems->m_OnSelected = [this](const plCSharpProblemsView::Problem& problem)
    { m_pCode->ShowFile(problem.m_sFile, problem.m_uiLine, m_pProblems->GetAll()); };
    m_pDiagnostics = AddPart<plEditorUiConsoleOutput>();
    m_pSetupControls = AddPart<plEditorUiControlStrip>();
    m_pSetupOutput = AddPart<plEditorUiConsoleOutput>();

    plEditorUiTab tabs[PageCount];
    tabs[ProblemsPage].m_sLabel = "Problems";
    tabs[BuildOutputPage].m_sLabel = "Build Output";
    tabs[SetupPage].m_sLabel = "Setup";

    m_pTabs = PL_DEFAULT_NEW(plEditorUiTabs, *GetSurface(), "CSharp.Tabs");
    m_pTabs->SetTabs(tabs);
    m_pTabs->m_CurrentChanged.AddEventHandler([this](plUInt32)
      { m_bRebuild = true; });
  }

  ~plCSharpBuildView() { m_pTabs.Clear(); }

  plEditorUiControlStrip& GetStatus() const { return *m_pStatus; }
  plCSharpProblemsView& GetProblems() const { return *m_pProblems; }
  plEditorUiConsoleOutput& GetDiagnostics() const { return *m_pDiagnostics; }
  plEditorUiControlStrip& GetSetupControls() const { return *m_pSetupControls; }
  plEditorUiConsoleOutput& GetSetupOutput() const { return *m_pSetupOutput; }

  /// The Problems tab's count pill: empty hides it, bProblem tints it as an error count.
  void SetProblemCount(plStringView sCount, bool bProblem)
  {
    if (m_pTabs == nullptr)
      return;

    plHybridArray<plEditorUiTab, PageCount> tabs;
    tabs = m_pTabs->GetTabs();
    tabs[ProblemsPage].m_sCount = sCount;
    tabs[ProblemsPage].m_bCountIsProblem = bProblem;
    m_pTabs->SetTabs(tabs);
    m_bRebuild = true;
  }

  /// Shows the text in an output from its start, unless the output shows it already.
  static void SetOutputText(plEditorUiConsoleOutput& ref_output, plStringView sText)
  {
    if (ref_output.FullText() == sText)
      return;

    plConsoleString message;
    message.m_sText = sText;

    ref_output.Clear();
    ref_output.Append(message);
    ref_output.ScrollToStart();
  }

protected:
  void OnUiUpdate() override
  {
    if (m_pTabs == nullptr)
      return;

    const plVec2 vSize = GetSurface()->GetLogicalSize();
    const float fStatusHeight = m_pStatus->GetHeight();

    if (m_bRebuild || vSize != m_vBuiltSize || fStatusHeight != m_fBuiltStatusHeight)
      BuildDocument(vSize, fStatusHeight);

    PlaceParts(vSize, fStatusHeight);
  }

private:
  template <typename Part, typename... Args>
  Part* AddPart(Args&&... args)
  {
    return static_cast<Part*>(AddChild(plUniquePtr<plEditorUiContent>(PL_DEFAULT_NEW(Part, std::forward<Args>(args)...))));
  }

  /// The tab row under the status line; the parts are child contents and draw themselves.
  void BuildDocument(const plVec2& vSize, float fStatusHeight)
  {
    using namespace plEditorUi;

    m_bRebuild = false;
    m_vBuiltSize = vSize;
    m_fBuiltStatusHeight = fStatusHeight;

    plUiPanel* pRoot = Panel();
    pRoot->m_Color = Background();

    plUiPanel* pRow = Panel(pRoot);
    pRow->m_vAnchorMin = pRow->m_vAnchorMax = plVec2::MakeZero();
    pRow->m_vOffsets = plVec4(0, fStatusHeight, vSize.x, plEditorUiTabs::Height);
    pRow->m_Color = Surface();
    m_pTabs->Build(pRow, vSize.x);

    SetDocument(Finish(pRoot));
  }

  /// The status line on top; below the tab row, the current tab's page. The other pages hide.
  void PlaceParts(const plVec2& vSize, float fStatusHeight)
  {
    const plRectFloat hidden(0, 0, 0, 0);
    const float fTop = fStatusHeight + plEditorUiTabs::Height;
    const plRectFloat page(0, fTop, vSize.x, plMath::Max(vSize.y - fTop, 0.0f));
    const plUInt32 uiPage = m_pTabs->GetCurrent();

    SetChildRect(m_pStatus, plRectFloat(0, 0, vSize.x, fStatusHeight));

    // the problem list on top, the selected problem's file below
    if (uiPage == ProblemsPage)
    {
      const float fList = plMath::Floor(page.height * 0.45f);
      SetChildRect(m_pProblems, plRectFloat(page.x, page.y, page.width, fList));
      SetChildRect(m_pCode, plRectFloat(page.x, page.y + fList + 1.0f, page.width, plMath::Max(page.height - fList - 1.0f, 0.0f)));
    }
    else
    {
      SetChildRect(m_pProblems, hidden);
      SetChildRect(m_pCode, hidden);
    }

    SetChildRect(m_pDiagnostics, uiPage == BuildOutputPage ? page : hidden);

    if (uiPage != SetupPage)
    {
      SetChildRect(m_pSetupControls, hidden);
      SetChildRect(m_pSetupOutput, hidden);
      return;
    }

    // the Check button's row above the setup output
    const float fControls = plMath::Min(m_pSetupControls->GetHeight(), page.height);
    SetChildRect(m_pSetupControls, plRectFloat(page.x, page.y, page.width, fControls));
    SetChildRect(m_pSetupOutput, plRectFloat(page.x, page.y + fControls, page.width, page.height - fControls));
  }

  plEditorUiControlStrip* m_pStatus = nullptr;
  plCSharpProblemsView* m_pProblems = nullptr;
  plCSharpCodeView* m_pCode = nullptr;
  plEditorUiConsoleOutput* m_pDiagnostics = nullptr;
  plEditorUiControlStrip* m_pSetupControls = nullptr;
  plEditorUiConsoleOutput* m_pSetupOutput = nullptr;
  plUniquePtr<plEditorUiTabs> m_pTabs;

  plVec2 m_vBuiltSize = plVec2::MakeZero();
  float m_fBuiltStatusHeight = -1.0f;
  bool m_bRebuild = true;
};

//////////////////////////////////////////////////////////////////////////
// Window

plCSharpProjectAssetDocumentWindow::plCSharpProjectAssetDocumentWindow(plCSharpProjectAssetDocument* pDocument)
  : plDocumentWindow(pDocument)
  , m_pAssetDocument(pDocument)
{
  SetMenuBarActions("CSharpProjectAssetMenuBar");
  SetToolBarActions("CSharpProjectAssetToolBar");

  CreateBuildView();

  // the diagnostics get the width, the properties sit on their right (the plugin's default arrangement for the type)
  AddPropertiesPanel({"CSharpProjectAssetProperties", "C# Project Properties", plEditorUiDockRole::Right});
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(
    plMakeDelegate(&plCSharpProjectAssetDocumentWindow::DocumentObjectEventHandler, this));

  UpdateDiagnostics();
}

plCSharpProjectAssetDocumentWindow::~plCSharpProjectAssetDocumentWindow()
{
  // a running probe is killed with the window; its result is never reported, the views may be gone
  m_Timers.StopAll();
  m_pSdkProbe.Clear();

  // the setup controls report clicks to this window
  DestroyPanels();

  m_pAssetDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(
    plMakeDelegate(&plCSharpProjectAssetDocumentWindow::DocumentObjectEventHandler, this));
}

void plCSharpProjectAssetDocumentWindow::CreateBuildView()
{
  m_pBuildView = AddPanel<plCSharpBuildView>({"CSharpBuildDiagnostics", "Build Diagnostics"}, m_pAssetDocument);

  m_pStatus = &m_pBuildView->GetStatus();
  m_uiStatusLabel = m_pStatus->AddLabel("Checking C# build status...");
  m_pProblems = &m_pBuildView->GetProblems();
  m_pDiagnostics = &m_pBuildView->GetDiagnostics();
  CreateSetupPage();

  m_Timers.Start(plTime::MakeFromMilliseconds(250), [this]()
    { UpdateBuildStatus(); }, true);
}

void plCSharpProjectAssetDocumentWindow::UpdateBuildStatus()
{
  const auto asset = plAssetCurator::GetSingleton()->GetSubAsset(m_pAssetDocument->GetGuid());
  if (!asset || asset->m_pAssetInfo == nullptr)
    return;

  switch (asset->m_pAssetInfo->m_TransformState)
  {
    case plAssetInfo::UpToDate:
      m_pStatus->SetText(m_uiStatusLabel, "Build up to date. Simulation reload is handled by the engine.");
      break;
    case plAssetInfo::TransformError:
      m_pStatus->SetText(m_uiStatusLabel, "Build failed. Previous successful output was retained. See Problems.");
      break;
    case plAssetInfo::NeedsImport:
      m_pStatus->SetText(m_uiStatusLabel, "Compiled. Importing script descriptors...");
      break;
    case plAssetInfo::NeedsTransform:
      m_pStatus->SetText(m_uiStatusLabel, "Changes pending compilation...");
      break;
    default:
      m_pStatus->SetText(m_uiStatusLabel, "Checking project inputs and build status...");
      break;
  }
}

// Setup check.

void plCSharpProjectAssetDocumentWindow::CreateSetupPage()
{
  m_pSetupControls = &m_pBuildView->GetSetupControls();
  m_uiCheckSetupButton = m_pSetupControls->AddButton("Check C# setup", {}, 160);
  m_pSetupControls->m_OnChanged = [this](plUInt32, plInt32)
  { CheckSetup(); };
  m_pSetupOutput = &m_pBuildView->GetSetupOutput();
}

void plCSharpProjectAssetDocumentWindow::AppendSetupOutput(plStringView sText)
{
  if (!m_sSetupOutput.IsEmpty())
    m_sSetupOutput.Append("\n");

  m_sSetupOutput.Append(sText);
  plCSharpBuildView::SetOutputText(*m_pSetupOutput, m_sSetupOutput);
}

void plCSharpProjectAssetDocumentWindow::CheckSetup()
{
  if (m_pSdkProbe != nullptr && m_pSdkProbe->GetState() == plProcessState::Running)
    return;

  m_sSetupOutput.Clear();
  m_pSetupControls->SetEnabled(m_uiCheckSetupButton, false);

  const plStringBuilder sPayload = plCSharpEditorPaths::FindPayloadRoot();
  const char* files[] = {"CSharp/Plasma.ScriptCore.dll", "CSharp/Plasma.ManagedHost.dll",
    "CSharp/Plasma.ManagedHost.runtimeconfig.json", "CSharp/Tools/Plasma.ScriptInspector.dll",
    "CSharp/Tools/Plasma.ScriptCompiler.dll", "CSharp/M0Game/Plasma.Engine.Generator.dll"};

  bool bMissingFiles = false;
  for (const char* szFile : files)
  {
    const plStringBuilder sPath(sPayload, "/", szFile);
    const bool bExists = plOSFile::ExistsFile(sPath);
    bMissingFiles |= !bExists;
    AppendSetupOutput(plStringBuilder(bExists ? "Found: " : "Missing: ", szFile));
  }

  // The bundled runtime is an optional package component; without it the host uses an installed .NET 10.
  plDynamicArray<plFileStats> runtimeVersions;
  plOSFile::GatherAllItemsInFolder(runtimeVersions, plStringBuilder(sPayload, "/DotNet/host/fxr"), plFileSystemIteratorFlags::ReportFolders);
  if (runtimeVersions.IsEmpty())
    AppendSetupOutput("Bundled .NET runtime: not installed (optional). The engine uses a .NET 10 runtime installed on this machine.");
  else
    AppendSetupOutput(plStringBuilder("Bundled .NET runtime: ", runtimeVersions.PeekBack().m_sName));

  if (bMissingFiles)
    AppendSetupOutput("\nSome package files are missing: reinstall plasma.csharp, or rebuild the package before installing it.");

  AppendSetupOutput("\nThe engine checks native ABI compatibility when C# initializes; see the engine log for its selected runtime and any mismatch.\n\nChecking the project's .NET SDK...");

  {
    PL_LOCK(m_SdkProbeMutex);
    m_sSdkProbeOutput.Clear();
    m_sSdkProbeErrors.Clear();
  }

  plProcessOptions options;
  options.m_sProcess = "dotnet";
  options.m_Arguments.PushBack("--version");
  options.m_sWorkingDirectory = m_pAssetDocument->GetDocumentPath().GetFileDirectory();
  options.m_onStdOut = [this](plStringView sText)
  {
    PL_LOCK(m_SdkProbeMutex);
    m_sSdkProbeOutput.Append(sText);
  };
  options.m_onStdError = [this](plStringView sText)
  {
    PL_LOCK(m_SdkProbeMutex);
    m_sSdkProbeErrors.Append(sText);
  };

  m_pSdkProbe = PL_DEFAULT_NEW(plProcess);
  if (m_pSdkProbe->Launch(options).Failed())
  {
    m_pSdkProbe.Clear();
    AppendSetupOutput("Could not start dotnet. Install the .NET 10 SDK and make it available on PATH, then restart the editor.");
    m_pSetupControls->SetEnabled(m_uiCheckSetupButton, true);
    return;
  }

  m_uiSdkProbeTimer = m_Timers.Start(plTime::MakeFromMilliseconds(100), [this]()
    { PollSdkProbe(); }, true);
}

void plCSharpProjectAssetDocumentWindow::PollSdkProbe()
{
  if (m_pSdkProbe == nullptr || m_pSdkProbe->GetState() == plProcessState::Running)
    return;

  m_Timers.Stop(m_uiSdkProbeTimer);

  const plInt32 iExitCode = m_pSdkProbe->GetExitCode();
  m_pSdkProbe.Clear();

  plStringBuilder sOutput, sErrors;
  {
    PL_LOCK(m_SdkProbeMutex);
    sOutput = m_sSdkProbeOutput;
    sErrors = m_sSdkProbeErrors;
  }

  sOutput.Trim();
  sErrors.Trim();

  if (iExitCode == 0 && sOutput.StartsWith("10."))
    AppendSetupOutput(plStringBuilder("SDK ready: ", sOutput));
  else
    AppendSetupOutput(plStringBuilder("SDK unavailable or incompatible. Install a .NET 10 SDK accepted by the project's global.json.\n", sOutput, "\n", sErrors));

  m_pSetupControls->SetEnabled(m_uiCheckSetupButton, true);
}

// Diagnostics.

void plCSharpProjectAssetDocumentWindow::DocumentObjectEventHandler(
  const plDocumentObjectPropertyEvent& event)
{
  if (event.m_sProperty == "BuildDiagnostics" ||
      event.m_sProperty == "LastGoodDescriptorJson")
  {
    UpdateDiagnostics();
  }
}

void plCSharpProjectAssetDocumentWindow::UpdateDiagnostics()
{
  const plString& diagnostics = m_pAssetDocument->GetProperties()->m_sBuildDiagnostics;

  // One row per distinct file, line, column and code.
  plDynamicArray<plCSharpProblemsView::Problem> problems;
  static const std::regex diagnosticPattern(
    R"(^\s*(.+?)\((\d+),(\d+)(?:,\d+,\d+)?\):\s*(error|warning)\s+(\w+):\s*(.*?)(?:\s+\[.*\])?$)", std::regex::ECMAScript);
  plSet<plString> seen;

  plHybridArray<plStringView, 64> lines;
  diagnostics.GetView().Split(true, lines, "\n");

  for (plStringView line : lines)
  {
    line.Trim();

    const std::string text(line.GetStartPointer(), line.GetElementCount());
    std::smatch match;
    if (!std::regex_match(text, match, diagnosticPattern))
      continue;

    plStringBuilder identity;
    identity.SetFormat("{}:{}:{}:{}", match[1].str().c_str(), match[2].str().c_str(), match[3].str().c_str(), match[5].str().c_str());
    if (seen.Contains(identity))
      continue;
    seen.Insert(identity);

    auto& problem = problems.ExpandAndGetRef();
    problem.m_sSeverity = match[4].str().c_str();
    problem.m_sCode = match[5].str().c_str();
    problem.m_sMessage = match[6].str().c_str();
    problem.m_sFile = match[1].str().c_str();
    plConversionUtils::StringToUInt(match[2].str().c_str(), problem.m_uiLine).IgnoreResult();
    plConversionUtils::StringToUInt(match[3].str().c_str(), problem.m_uiColumn).IgnoreResult();
  }

  bool bErrors = false;
  for (const auto& problem : problems)
    bErrors |= problem.m_sSeverity == "error";

  plStringBuilder sCount;
  if (!problems.IsEmpty())
    sCount.SetFormat("{}", problems.GetCount());

  m_pBuildView->SetProblemCount(sCount, bErrors);
  m_pProblems->SetProblems(std::move(problems));

  if (diagnostics.IsEmpty())
    plCSharpBuildView::SetOutputText(*m_pDiagnostics, "No build diagnostics yet.\n\nUse Build Project to compile and discover C# script classes.");
  else
    plCSharpBuildView::SetOutputText(*m_pDiagnostics, diagnostics);
}
