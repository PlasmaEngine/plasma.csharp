#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>
#include <Core/Scripting/Bindings/ScriptBindingRegistry.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Threading/Mutex.h>

/// Runtime projection of the shared reflection-driven binding schema.
class PL_CSHARPPLUGIN_DLL plCSharpBindingRuntime
{
public:
  static plCSharpBindingRuntime& GetSingleton();

  plResult Startup();
  void Shutdown();
  plResult Refresh();
  plResult GetSchemaHash(plUInt64& out_uiSchemaHash);

  plCSharpStatus ValidateObject(
    plCSharpObjectHandle object, plUInt32 uiExpectedKind) const;
  plCSharpStatus InvokeReflected(plUInt64 uiBindingId, plCSharpObjectHandle target,
    const plCSharpValue* pArguments, plUInt32 uiArgumentCount, plCSharpValue* out_pResult);
  plCSharpStatus ReleaseValue(plCSharpValue* pValue) const;

private:
  void PluginEventHandler(const plPluginEvent& eventData);
  bool EnsureSnapshotLocked();

  plCSharpStatus InvokeFunction(const plScriptBindingFunctionDesc& function,
    plCSharpObjectHandle target, const plCSharpValue* pArguments,
    plUInt32 uiArgumentCount, plCSharpValue* out_pResult);
  plCSharpStatus InvokeProperty(const plScriptBindingPropertyDesc& property,
    plCSharpObjectHandle target, const plCSharpValue* pArguments,
    plUInt32 uiArgumentCount, plCSharpValue* out_pResult);

  mutable plMutex m_Mutex;
  plScriptBindingSnapshot m_Snapshot;
  plUInt64 m_uiSchemaHash = 0;
  bool m_bSnapshotValid = false;
  bool m_bPluginChangesInProgress = false;
  bool m_bSubscribedToPluginEvents = false;
};
