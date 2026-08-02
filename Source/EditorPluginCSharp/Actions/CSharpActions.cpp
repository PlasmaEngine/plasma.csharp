#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorPluginCSharp/Actions/CSharpActions.h>
#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectAsset.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plActionDescriptorHandle plCSharpActions::s_hCategory;
plActionDescriptorHandle plCSharpActions::s_hBuild;
plActionDescriptorHandle plCSharpActions::s_hOpenProject;
plActionDescriptorHandle plCSharpActions::s_hOpenScript;
plActionDescriptorHandle plCSharpActions::s_hCreateScript;

void plCSharpActions::RegisterActions()
{
  s_hCategory = PL_REGISTER_CATEGORY("CSharpCategory");
  s_hBuild = PL_REGISTER_ACTION_1(
    "CSharp.BuildProject", plActionScope::Document, "C#", "Ctrl+B", plCSharpAction, plCSharpAction::ActionType::Build);
  s_hOpenProject = PL_REGISTER_ACTION_1(
    "CSharp.OpenProject", plActionScope::Document, "C#", "", plCSharpAction, plCSharpAction::ActionType::OpenProject);
  s_hOpenScript = PL_REGISTER_ACTION_1(
    "CSharp.OpenScript", plActionScope::Document, "C#", "", plCSharpAction, plCSharpAction::ActionType::OpenScript);
  s_hCreateScript = PL_REGISTER_ACTION_1(
    "CSharp.CreateScript", plActionScope::Document, "C#", "", plCSharpAction, plCSharpAction::ActionType::CreateScript);
}

void plCSharpActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategory);
  plActionManager::UnregisterAction(s_hBuild);
  plActionManager::UnregisterAction(s_hOpenProject);
  plActionManager::UnregisterAction(s_hOpenScript);
  plActionManager::UnregisterAction(s_hCreateScript);
}

void plCSharpActions::MapActions(plStringView sMapping, plStringView sPath)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The C# action map '{}' does not exist.", sMapping);

  pMap->MapAction(s_hCategory, sPath, 11.0f);
  pMap->MapAction(s_hBuild, sPath, "CSharpCategory", 1.0f);
  pMap->MapAction(s_hCreateScript, sPath, "CSharpCategory", 2.0f);
  pMap->MapAction(s_hOpenScript, sPath, "CSharpCategory", 3.0f);
  pMap->MapAction(s_hOpenProject, sPath, "CSharpCategory", 4.0f);
}

plCSharpAction::plCSharpAction(const plActionContext& context, const char* szName, ActionType type)
  : plButtonAction(context, szName, false, "")
  , m_pDocument(static_cast<plCSharpProjectAssetDocument*>(context.m_pDocument))
  , m_Type(type)
{
  switch (m_Type)
  {
    case ActionType::Build:
      SetIconPath(":/GuiFoundation/Icons/ReloadResources.svg");
      break;
    case ActionType::OpenProject:
    case ActionType::OpenScript:
      SetIconPath(":/GuiFoundation/Icons/vscode.svg");
      break;
    case ActionType::CreateScript:
      SetIconPath(":/GuiFoundation/Icons/Add.svg");
      break;
  }
}

void plCSharpAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);

  switch (m_Type)
  {
    case ActionType::Build:
    {
      const plTransformStatus status = m_pDocument->BuildProject();
      if (status.Failed())
        plLog::Error("C# project build failed: {}", status.m_sMessage);
      break;
    }
    case ActionType::OpenProject:
      m_pDocument->OpenProject();
      break;
    case ActionType::OpenScript:
      m_pDocument->OpenScript();
      break;
    case ActionType::CreateScript:
      m_pDocument->CreateScript();
      break;
  }
}
