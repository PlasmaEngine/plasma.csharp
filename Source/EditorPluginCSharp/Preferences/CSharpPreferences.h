#pragma once

#include <EditorFramework/Preferences/Preferences.h>
#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>

struct PL_EDITORPLUGINCSHARP_DLL plCSharpIDE
{
  using StorageType = plUInt8;

  enum Enum
  {
    VisualStudio,
    Rider,
    VisualStudioCode,

    Default = VisualStudioCode
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINCSHARP_DLL, plCSharpIDE);

class PL_EDITORPLUGINCSHARP_DLL plCSharpPreferences : public plPreferences
{
  PL_ADD_DYNAMIC_REFLECTION(plCSharpPreferences, plPreferences);

public:
  plCSharpPreferences();

  plEnum<plCSharpIDE> m_IDE;
};
