#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>
#include <Core/Scripting/Bindings/ScriptBindingRegistry.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>

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
  /// What a binding id calls, resolved once per snapshot so a call is one hash lookup.
  struct CallPlan
  {
    const plScriptBindingFunctionDesc* m_pFunction = nullptr;
    const plScriptBindingPropertyDesc* m_pProperty = nullptr;
    const plRTTI* m_pDeclaringType = nullptr;
  };

  /// A snapshot and its call plans. Calls hold a reference while they run, so a plugin change can
  /// replace the current snapshot without waiting for them and without invalidating their plan.
  struct SnapshotData : public plRefCounted
  {
    plScriptBindingSnapshot m_Snapshot;
    plHashTable<plUInt64, CallPlan> m_Plans;
  };

  void PluginEventHandler(const plPluginEvent& eventData);
  bool EnsureSnapshotLocked();

  plCSharpStatus InvokeFunction(const plScriptBindingFunctionDesc& function, const plRTTI* pDeclaringType,
    plCSharpObjectHandle target, const plCSharpValue* pArguments,
    plUInt32 uiArgumentCount, plCSharpValue* out_pResult);
  plCSharpStatus InvokeProperty(const plScriptBindingPropertyDesc& property, const plRTTI* pDeclaringType,
    plCSharpObjectHandle target, const plCSharpValue* pArguments,
    plUInt32 uiArgumentCount, plCSharpValue* out_pResult);

  mutable plMutex m_Mutex;
  plSharedPtr<SnapshotData> m_pSnapshot;
  plUInt64 m_uiSchemaHash = 0;
  bool m_bSnapshotValid = false;
  bool m_bPluginChangesInProgress = false;
  bool m_bSubscribedToPluginEvents = false;
};
