#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Runtime/CSharpFunctionProperty.h>
#include <CSharpPlugin/Runtime/CSharpInstance.h>
#include <Core/Scripting/ScriptAttributes.h>
#include <Core/Scripting/ScriptComponent.h>
#include <Foundation/IO/JSONWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Reflection/ReflectionUtils.h>

namespace
{
  static void WriteVec2(
    plStandardJSONWriter& writer, plStringView sName, const plVec2& value)
  {
    writer.BeginObject(sName);
    writer.AddVariableFloat("X", value.x);
    writer.AddVariableFloat("Y", value.y);
    writer.EndObject();
  }

  static void WriteVec2U32(
    plStandardJSONWriter& writer, plStringView sName, const plVec2U32& value)
  {
    writer.BeginObject(sName);
    writer.AddVariableUInt32("X", value.x);
    writer.AddVariableUInt32("Y", value.y);
    writer.EndObject();
  }

  static void WriteVec3(
    plStandardJSONWriter& writer, plStringView sName, const plVec3& value)
  {
    writer.BeginObject(sName);
    writer.AddVariableFloat("X", value.x);
    writer.AddVariableFloat("Y", value.y);
    writer.AddVariableFloat("Z", value.z);
    writer.EndObject();
  }

  static void WriteVec4(
    plStandardJSONWriter& writer, plStringView sName, const plVec4& value)
  {
    writer.BeginObject(sName);
    writer.AddVariableFloat("X", value.x);
    writer.AddVariableFloat("Y", value.y);
    writer.AddVariableFloat("Z", value.z);
    writer.AddVariableFloat("W", value.w);
    writer.EndObject();
  }

  static void WriteQuat(
    plStandardJSONWriter& writer, plStringView sName, const plQuat& value)
  {
    writer.BeginObject(sName);
    writer.AddVariableFloat("X", value.x);
    writer.AddVariableFloat("Y", value.y);
    writer.AddVariableFloat("Z", value.z);
    writer.AddVariableFloat("W", value.w);
    writer.EndObject();
  }

  static bool WriteManagedJsonValue(
    plStandardJSONWriter& writer, plStringView sName, const plVariant& value)
  {
    if (!value.IsValid())
    {
      writer.AddVariableNULL(sName);
      return true;
    }

    switch (value.GetType())
    {
      case plVariantType::Bool:
        writer.AddVariableBool(sName, value.Get<bool>());
        return true;
      case plVariantType::Int8:
        writer.AddVariableInt64(sName, value.Get<plInt8>());
        return true;
      case plVariantType::UInt8:
        writer.AddVariableUInt64(sName, value.Get<plUInt8>());
        return true;
      case plVariantType::Int16:
        writer.AddVariableInt64(sName, value.Get<plInt16>());
        return true;
      case plVariantType::UInt16:
        writer.AddVariableUInt64(sName, value.Get<plUInt16>());
        return true;
      case plVariantType::Int32:
        writer.AddVariableInt64(sName, value.Get<plInt32>());
        return true;
      case plVariantType::UInt32:
        writer.AddVariableUInt64(sName, value.Get<plUInt32>());
        return true;
      case plVariantType::Int64:
        writer.AddVariableInt64(sName, value.Get<plInt64>());
        return true;
      case plVariantType::UInt64:
        writer.AddVariableUInt64(sName, value.Get<plUInt64>());
        return true;
      case plVariantType::Float:
        writer.AddVariableFloat(sName, value.Get<float>());
        return true;
      case plVariantType::Double:
        writer.AddVariableDouble(sName, value.Get<double>());
        return true;
      case plVariantType::String:
        writer.AddVariableString(sName, value.Get<plString>());
        return true;
      case plVariantType::StringView:
        writer.AddVariableString(sName, value.Get<plStringView>());
        return true;
      case plVariantType::HashedString:
        writer.AddVariableString(sName, value.Get<plHashedString>().GetView());
        return true;
      case plVariantType::TempHashedString:
        writer.BeginObject(sName);
        writer.AddVariableUInt64(
          "Hash", value.Get<plTempHashedString>().GetHash());
        writer.EndObject();
        return true;
      case plVariantType::Time:
        writer.BeginObject(sName);
        writer.AddVariableDouble("Seconds", value.Get<plTime>().GetSeconds());
        writer.EndObject();
        return true;
      case plVariantType::Angle:
        writer.BeginObject(sName);
        writer.AddVariableFloat("Radians", value.Get<plAngle>().GetRadian());
        writer.EndObject();
        return true;
      case plVariantType::Vector2:
        WriteVec2(writer, sName, value.Get<plVec2>());
        return true;
      case plVariantType::Vector2U:
        WriteVec2U32(writer, sName, value.Get<plVec2U32>());
        return true;
      case plVariantType::Vector3:
        WriteVec3(writer, sName, value.Get<plVec3>());
        return true;
      case plVariantType::Vector4:
        WriteVec4(writer, sName, value.Get<plVec4>());
        return true;
      case plVariantType::Quaternion:
        WriteQuat(writer, sName, value.Get<plQuat>());
        return true;
      case plVariantType::Color:
      {
        const plColor& color = value.Get<plColor>();
        writer.BeginObject(sName);
        writer.AddVariableFloat("R", color.r);
        writer.AddVariableFloat("G", color.g);
        writer.AddVariableFloat("B", color.b);
        writer.AddVariableFloat("A", color.a);
        writer.EndObject();
        return true;
      }
      case plVariantType::Transform:
      {
        const plTransform& transform = value.Get<plTransform>();
        writer.BeginObject(sName);
        WriteVec3(writer, "Position", transform.m_vPosition);
        WriteQuat(writer, "Rotation", transform.m_qRotation);
        WriteVec3(writer, "Scale", transform.m_vScale);
        writer.EndObject();
        return true;
      }
      default:
        writer.AddVariableNULL(sName);
        return false;
    }
  }

  static plResult SerializeMessage(
    const plMessage& msg, plString& out_sJson)
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter stream(&storage);
    plStandardJSONWriter writer;
    writer.SetOutputStream(&stream);
    writer.SetWhitespaceMode(plJSONWriter::WhitespaceMode::None);
    writer.BeginObject();

    plDynamicArray<const plAbstractProperty*> properties;
    msg.GetDynamicRTTI()->GetAllProperties(properties);
    for (const plAbstractProperty* pProperty : properties)
    {
      if (pProperty->GetCategory() != plPropertyCategory::Member)
        continue;

      const auto* pMember = static_cast<const plAbstractMemberProperty*>(pProperty);
      plStringView sName = pProperty->GetPropertyName();
      sName.TrimWordStart("m_");
      const plVariant value = plReflectionUtils::GetMemberPropertyValue(pMember, &msg);
      if (!WriteManagedJsonValue(writer, sName, value))
      {
        plLog::Warning(
          "C# message property '{}.{}' has unsupported value type '{}'; null was sent.",
          msg.GetDynamicRTTI()->GetTypeName(), pProperty->GetPropertyName(), value.GetType());
      }
    }

    writer.EndObject();
    const plArrayPtr<const plUInt8> bytes = storage.GetContiguousMemoryRange(0);
    out_sJson = plStringView(
      reinterpret_cast<const char*>(bytes.GetPtr()), bytes.GetCount());
    return writer.HadWriteError() ? PL_FAILURE : PL_SUCCESS;
  }
} // namespace

// clang-format off
PL_IMPLEMENT_MESSAGE_TYPE(plMsgDeliverCSharpMsg);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgDeliverCSharpMsg, 1, plRTTIDefaultAllocator<plMsgDeliverCSharpMsg>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plExcludeFromScript()
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plCSharpFunctionProperty::plCSharpFunctionProperty(
  plStringView sName, plUInt64 uiMethodId)
  : plScriptFunctionProperty(sName)
  , m_uiMethodId(uiMethodId)
{
}

plUInt32 plCSharpFunctionProperty::GetArgumentCount() const
{
  return m_uiMethodId == plComponent_ScriptBaseClassFunctions::Update ? 1 : 0;
}

const plRTTI* plCSharpFunctionProperty::GetArgumentType(
  plUInt32 uiParamIndex) const
{
  return m_uiMethodId == plComponent_ScriptBaseClassFunctions::Update &&
             uiParamIndex == 0
           ? plGetStaticRTTI<plTime>()
           : nullptr;
}

plBitflags<plPropertyFlags> plCSharpFunctionProperty::GetArgumentFlags(
  plUInt32 uiParamIndex) const
{
  return m_uiMethodId == plComponent_ScriptBaseClassFunctions::Update &&
             uiParamIndex == 0
           ? plPropertyFlags::StandardType
           : plPropertyFlags::Void;
}

void plCSharpFunctionProperty::Execute(
  void* pInstance, plArrayPtr<plVariant> arguments, plVariant& out_returnValue) const
{
  PL_IGNORE_UNUSED(out_returnValue);

  auto* pCSharpInstance = static_cast<plCSharpInstance*>(pInstance);
  plTime deltaTime = plTime::MakeZero();
  if (m_uiMethodId == plComponent_ScriptBaseClassFunctions::Update &&
      !arguments.IsEmpty() && arguments[0].IsA<plTime>())
    deltaTime = arguments[0].Get<plTime>();

  pCSharpInstance->InvokeLifecycle(m_uiMethodId, deltaTime).IgnoreResult();
}

plCSharpMessageHandler::plCSharpMessageHandler(
  const plScriptMessageDesc& desc, plUInt64 uiMessageHandlerId)
  : plScriptMessageHandler(desc)
  , m_uiMessageHandlerId(uiMessageHandlerId)
{
  m_DispatchFunc = &Dispatch;
}

void plCSharpMessageHandler::Dispatch(
  plAbstractMessageHandler* pSelf, void* pInstance, plMessage& ref_msg)
{
  auto* pHandler = static_cast<plCSharpMessageHandler*>(pSelf);
  auto* pComponent = static_cast<plScriptComponent*>(pInstance);
  auto* pCSharpInstance =
    static_cast<plCSharpInstance*>(pComponent->GetScriptInstance());
  if (pCSharpInstance == nullptr)
    return;

  if (ref_msg.GetDynamicRTTI() == plGetStaticRTTI<plMsgDeliverCSharpMsg>())
  {
    const auto& message = static_cast<const plMsgDeliverCSharpMsg&>(ref_msg);
    pCSharpInstance
      ->DispatchManagedMessage(message.m_uiMessageId, message.m_uiPayloadToken)
      .IgnoreResult();
    return;
  }

  plString payloadJson;
  if (SerializeMessage(ref_msg, payloadJson).Succeeded())
  {
    pCSharpInstance
      ->DispatchMessage(pHandler->m_uiMessageHandlerId, payloadJson)
      .IgnoreResult();
  }
}
