#pragma once

#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>
#include <GuiFoundation/Action/BaseActions.h>

class plCSharpProjectAssetDocument;

class PL_EDITORPLUGINCSHARP_DLL plCSharpActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();
  static void MapActions(plStringView sMapping, plStringView sPath = {});

  static plActionDescriptorHandle s_hCategory;
  static plActionDescriptorHandle s_hBuild;
  static plActionDescriptorHandle s_hOpenProject;
  static plActionDescriptorHandle s_hOpenScript;
  static plActionDescriptorHandle s_hCreateScript;
};

class PL_EDITORPLUGINCSHARP_DLL plCSharpAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpAction, plButtonAction);

public:
  enum class ActionType
  {
    Build,
    OpenProject,
    OpenScript,
    CreateScript,
  };

  plCSharpAction(const plActionContext& context, const char* szName, ActionType type);
  virtual void Execute(const plVariant& value) override;

private:
  plCSharpProjectAssetDocument* m_pDocument = nullptr;
  ActionType m_Type;
};
