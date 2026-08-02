#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <Core/Scripting/ScriptRTTI.h>
#include <Foundation/Communication/Message.h>

/// Opaque synchronous envelope used to route user-defined C# messages through
/// the native world. The managed registry owns the actual message object for
/// the duration of SendMessage(), so reference-type mutations are visible to
/// the sender without serializing arbitrary managed object graphs.
struct PL_CSHARPPLUGIN_DLL plMsgDeliverCSharpMsg : public plMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgDeliverCSharpMsg, plMessage);

  plUInt64 m_uiMessageId = 0;
  plUInt64 m_uiPayloadToken = 0;
};

class PL_CSHARPPLUGIN_DLL plCSharpFunctionProperty : public plScriptFunctionProperty
{
public:
  plCSharpFunctionProperty(plStringView sName, plUInt64 uiMethodId);

  virtual plFunctionType::Enum GetFunctionType() const override { return plFunctionType::Member; }
  virtual const plRTTI* GetReturnType() const override { return nullptr; }
  virtual plBitflags<plPropertyFlags> GetReturnFlags() const override { return plPropertyFlags::Void; }
  virtual plUInt32 GetArgumentCount() const override;
  virtual const plRTTI* GetArgumentType(plUInt32 uiParamIndex) const override;
  virtual plBitflags<plPropertyFlags> GetArgumentFlags(plUInt32 uiParamIndex) const override;
  virtual void Execute(
    void* pInstance, plArrayPtr<plVariant> arguments, plVariant& out_returnValue) const override;

private:
  plUInt64 m_uiMethodId = 0;
};

class PL_CSHARPPLUGIN_DLL plCSharpMessageHandler : public plScriptMessageHandler
{
public:
  plCSharpMessageHandler(const plScriptMessageDesc& desc, plUInt64 uiMessageHandlerId);

  static void Dispatch(
    plAbstractMessageHandler* pSelf, void* pInstance, plMessage& ref_msg);

private:
  plUInt64 m_uiMessageHandlerId = 0;
};
