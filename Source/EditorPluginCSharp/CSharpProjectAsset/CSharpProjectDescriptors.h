#pragma once

#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/Uuid.h>

struct plCSharpFieldDefaultValue
{
  plString m_sKind;
  plString m_sValue;
  plMap<plString, plString> m_Components;
};

struct plCSharpFieldDescriptor
{
  plString m_sIdHex;
  plString m_sName;
  plString m_sManagedType;
  plCSharpFieldDefaultValue m_DefaultValue;
  plMap<plString, plString> m_Metadata;
};

struct plCSharpClassDescriptor
{
  plUuid m_SubAssetGuid;
  plUuid m_PersistentGuid;
  plString m_sTypeIdHex;
  plString m_sManagedName;
  plString m_sSourceFile;
  plDynamicArray<plCSharpFieldDescriptor> m_Fields;
};

class PL_EDITORPLUGINCSHARP_DLL plCSharpProjectDescriptors
{
public:
  static plUuid ComputeSubAssetGuid(const plUuid& projectGuid, const plUuid& persistentGuid);

  static plStatus Parse(
    plStringView sJson, const plUuid& projectGuid, plString& out_sEntryAssembly, plDynamicArray<plCSharpClassDescriptor>& out_classes);
};
