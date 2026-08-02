#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Runtime/CSharpProjectRuntime.h>
#include <Core/Scripting/ScriptRTTI.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/HashedString.h>
#include <Foundation/Types/RefCounted.h>

struct plCSharpFieldInfo
{
  plUInt64 m_uiId = 0;
  plHashedString m_sName;
  plString m_sManagedType;
};

/// Keeps a project generation alive while any resource or live script instance still references it.
class PL_CSHARPPLUGIN_DLL plCSharpGenerationContext : public plRefCountingImpl
{
public:
  explicit plCSharpGenerationContext(plCSharpGenerationLease&& lease);
  ~plCSharpGenerationContext();

  plUInt64 GetGeneration() const { return m_Lease.m_uiGeneration; }

private:
  plCSharpGenerationLease m_Lease;
};

class PL_CSHARPPLUGIN_DLL plCSharpClassData : public plRefCountingImpl
{
public:
  plSharedPtr<plCSharpGenerationContext> m_pGeneration;
  plUInt64 m_uiTypeId = 0;
  plString m_sManagedName;
  plDynamicArray<plCSharpFieldInfo> m_Fields;
};

class PL_CSHARPPLUGIN_DLL plCSharpInstance : public plScriptInstance
{
public:
  plCSharpInstance(plReflectedClass& inout_owner, plWorld* pWorld,
    const plSharedPtr<const plCSharpClassData>& pClassData);
  ~plCSharpInstance();

  bool IsValid() const { return m_uiInstance != 0; }

  plResult InvokeLifecycle(plUInt64 uiMethodId, plTime deltaTime = plTime::MakeZero());
  plResult DispatchMessage(plUInt64 uiMessageHandlerId, plStringView sPayloadJson);
  plResult DispatchManagedMessage(plUInt64 uiMessageId, plUInt64 uiPayloadToken);

  virtual void SetInstanceVariable(const plHashedString& sName, const plVariant& value) override;
  virtual plVariant GetInstanceVariable(const plHashedString& sName) override;
  virtual void GetInstanceVariables(plArrayMap<plHashedString, plVariant>& out_parameters) const override;
  virtual bool SupportsTransactionalReload() const override { return true; }
  virtual plResult PrepareForReload(
    const plArrayMap<plHashedString, plVariant>& parameters,
    const plArrayMap<plHashedString, plVariant>& reloadState) override;
  virtual void CancelReloadPreparation() override;

private:
  const plCSharpFieldInfo* FindField(const plHashedString& sName) const;
  plResult SetField(const plCSharpFieldInfo& field, const plVariant& value);
  plVariant GetField(const plCSharpFieldInfo& field) const;

  plSharedPtr<const plCSharpClassData> m_pClassData;
  plUInt64 m_uiInstance = 0;
  plCSharpObjectHandle m_OwnerHandle;
  plCSharpObjectHandle m_WorldHandle;
  plCSharpObjectHandle m_ComponentHandle;
  mutable plDynamicArray<plCSharpObjectHandle> m_BorrowedHandles;
  plString m_sOwnerName;
  plUInt8 m_uiLoggedLifecycleFailures = 0;
  bool m_bSimulationStartLogged = false;
};
