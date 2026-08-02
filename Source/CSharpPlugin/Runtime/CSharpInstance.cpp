#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Hosting/CSharpHost.h>
#include <CSharpPlugin/Runtime/CSharpInstance.h>
#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>
#include <Core/Scripting/ScriptComponent.h>
#include <Core/World/GameObject.h>
#include <Core/World/World.h>
#include <Foundation/Types/TypedPointer.h>

namespace
{
  static plCSharpValueFlags EncodeObjectKind(plCSharpNativeObjectKind kind)
  {
    return static_cast<plCSharpValueFlags>(
      static_cast<plUInt32>(kind) << 8);
  }

  static plCSharpNativeObjectKind DecodeObjectKind(plCSharpValueFlags flags)
  {
    const plUInt32 uiKind = (static_cast<plUInt32>(flags) >> 8) & 0xFFu;
    return uiKind >= static_cast<plUInt32>(plCSharpNativeObjectKind::World) &&
               uiKind <= static_cast<plUInt32>(plCSharpNativeObjectKind::ReflectedObject)
             ? static_cast<plCSharpNativeObjectKind>(uiKind)
             : plCSharpNativeObjectKind::Invalid;
  }

  static plStringView GetLifecycleName(plUInt64 uiMethodId)
  {
    switch (uiMethodId)
    {
      case plComponent_ScriptBaseClassFunctions::Initialize:
        return "Initialize";
      case plComponent_ScriptBaseClassFunctions::Deinitialize:
        return "Deinitialize";
      case plComponent_ScriptBaseClassFunctions::OnActivated:
        return "OnActivated";
      case plComponent_ScriptBaseClassFunctions::OnDeactivated:
        return "OnDeactivated";
      case plComponent_ScriptBaseClassFunctions::OnSimulationStarted:
        return "OnSimulationStarted";
      case plComponent_ScriptBaseClassFunctions::Update:
        return "Update";
      default:
        return "Unknown lifecycle method";
    }
  }

  static plCSharpValue MakeDoubleValue(double value)
  {
    plCSharpValue result;
    result.m_Kind = plCSharpValueKind::Double;
    plMemoryUtils::RawByteCopy(&result.m_uiPayload0, &value, sizeof(value));
    return result;
  }

  static plResult VariantToManagedValue(const plVariant& value, plCSharpValue& out_value)
  {
    out_value = {};
    if (!value.IsValid())
      return PL_SUCCESS;

    switch (value.GetType())
    {
      case plVariantType::Bool:
        out_value.m_Kind = plCSharpValueKind::Boolean;
        out_value.m_uiPayload0 = value.Get<bool>() ? 1 : 0;
        return PL_SUCCESS;
      case plVariantType::Int8:
        out_value.m_Kind = plCSharpValueKind::Int64;
        out_value.m_uiPayload0 = static_cast<plUInt64>(value.Get<plInt8>());
        return PL_SUCCESS;
      case plVariantType::UInt8:
        out_value.m_Kind = plCSharpValueKind::UInt64;
        out_value.m_uiPayload0 = value.Get<plUInt8>();
        return PL_SUCCESS;
      case plVariantType::Int16:
        out_value.m_Kind = plCSharpValueKind::Int64;
        out_value.m_uiPayload0 = static_cast<plUInt64>(value.Get<plInt16>());
        return PL_SUCCESS;
      case plVariantType::UInt16:
        out_value.m_Kind = plCSharpValueKind::UInt64;
        out_value.m_uiPayload0 = value.Get<plUInt16>();
        return PL_SUCCESS;
      case plVariantType::Int32:
        out_value.m_Kind = plCSharpValueKind::Int64;
        out_value.m_uiPayload0 = static_cast<plUInt64>(value.Get<plInt32>());
        return PL_SUCCESS;
      case plVariantType::UInt32:
        out_value.m_Kind = plCSharpValueKind::UInt64;
        out_value.m_uiPayload0 = value.Get<plUInt32>();
        return PL_SUCCESS;
      case plVariantType::Int64:
        out_value.m_Kind = plCSharpValueKind::Int64;
        out_value.m_uiPayload0 = static_cast<plUInt64>(value.Get<plInt64>());
        return PL_SUCCESS;
      case plVariantType::UInt64:
        out_value.m_Kind = plCSharpValueKind::UInt64;
        out_value.m_uiPayload0 = value.Get<plUInt64>();
        return PL_SUCCESS;
      case plVariantType::Float:
      case plVariantType::Double:
        out_value = MakeDoubleValue(value.ConvertTo<double>());
        return PL_SUCCESS;
      case plVariantType::String:
      {
        const plString& text = value.Get<plString>();
        out_value.m_Kind = plCSharpValueKind::Utf8String;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(text.GetData());
        out_value.m_uiPayload1 = text.GetElementCount();
        return PL_SUCCESS;
      }
      case plVariantType::StringView:
      {
        const plStringView text = value.Get<plStringView>();
        out_value.m_Kind = plCSharpValueKind::Utf8String;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(text.GetStartPointer());
        out_value.m_uiPayload1 = text.GetElementCount();
        return PL_SUCCESS;
      }
      case plVariantType::Time:
      {
        const plTime& data = value.Get<plTime>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Angle:
      {
        const plAngle& data = value.Get<plAngle>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Vector2:
      {
        const plVec2& data = value.Get<plVec2>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Vector2U:
      {
        const plVec2U32& data = value.Get<plVec2U32>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Vector3:
      {
        const plVec3& data = value.Get<plVec3>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Vector4:
      {
        const plVec4& data = value.Get<plVec4>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Quaternion:
      {
        const plQuat& data = value.Get<plQuat>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Color:
      {
        const plColor& data = value.Get<plColor>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::Transform:
      {
        const plTransform& data = value.Get<plTransform>();
        out_value.m_Kind = plCSharpValueKind::ByteSpan;
        out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(&data);
        out_value.m_uiPayload1 = sizeof(data);
        return PL_SUCCESS;
      }
      case plVariantType::TypedPointer:
      {
        const plTypedPointer pointer = value.Get<plTypedPointer>();
        if (pointer.m_pObject == nullptr)
          return PL_SUCCESS;
        if (pointer.m_pType == nullptr)
          return PL_FAILURE;

        plCSharpNativeObjectKind kind = plCSharpNativeObjectKind::ReflectedObject;
        if (pointer.m_pType->IsDerivedFrom<plWorld>())
          kind = plCSharpNativeObjectKind::World;
        else if (pointer.m_pType->IsDerivedFrom<plGameObject>())
          kind = plCSharpNativeObjectKind::GameObject;
        else if (pointer.m_pType->IsDerivedFrom<plComponent>())
          kind = plCSharpNativeObjectKind::Component;

        const plCSharpObjectHandle handle =
          plCSharpObjectRegistry::GetSingleton().RegisterObject(
            pointer.m_pObject, pointer.m_pType, kind);
        if (handle.m_uiValue == 0)
          return PL_FAILURE;

        if (!plCSharpExecutionScope::TrackBorrowedHandle(handle))
        {
          plCSharpObjectRegistry::GetSingleton().UnregisterObject(handle);
          return PL_FAILURE;
        }

        out_value.m_Kind = plCSharpValueKind::ObjectHandle;
        out_value.m_Flags = EncodeObjectKind(kind);
        out_value.m_uiPayload0 = handle.m_uiValue;
        out_value.m_uiPayload1 = handle.m_uiGeneration;
        return PL_SUCCESS;
      }
      default:
        return PL_FAILURE;
    }
  }

  template <typename T>
  static plVariant ReadByteSpan(const plCSharpValue& value)
  {
    if (value.m_Kind != plCSharpValueKind::ByteSpan || value.m_uiPayload0 == 0 ||
        value.m_uiPayload1 != sizeof(T))
    {
      return {};
    }

    return *reinterpret_cast<const T*>(value.m_uiPayload0);
  }

  static plVariant ManagedValueToVariant(
    const plCSharpValue& value, plStringView sManagedType)
  {
    switch (value.m_Kind)
    {
      case plCSharpValueKind::Null:
        return {};
      case plCSharpValueKind::Boolean:
        return value.m_uiPayload0 != 0;
      case plCSharpValueKind::Int64:
        return static_cast<plInt64>(value.m_uiPayload0);
      case plCSharpValueKind::UInt64:
        return value.m_uiPayload0;
      case plCSharpValueKind::Double:
      {
        double number = 0.0;
        plMemoryUtils::RawByteCopy(&number, &value.m_uiPayload0, sizeof(number));
        return number;
      }
      case plCSharpValueKind::Utf8String:
        if (value.m_uiPayload0 == 0 && value.m_uiPayload1 != 0)
          return {};
        return plString(plStringView(
          reinterpret_cast<const char*>(value.m_uiPayload0),
          static_cast<plUInt32>(value.m_uiPayload1)));
      case plCSharpValueKind::ByteSpan:
        if (sManagedType == "Plasma.Time")
          return ReadByteSpan<plTime>(value);
        if (sManagedType == "Plasma.Angle")
          return ReadByteSpan<plAngle>(value);
        if (sManagedType == "Plasma.Vec2")
          return ReadByteSpan<plVec2>(value);
        if (sManagedType == "Plasma.Vec2U32")
          return ReadByteSpan<plVec2U32>(value);
        if (sManagedType == "Plasma.Vec3")
          return ReadByteSpan<plVec3>(value);
        if (sManagedType == "Plasma.Vec4")
          return ReadByteSpan<plVec4>(value);
        if (sManagedType == "Plasma.Quat")
          return ReadByteSpan<plQuat>(value);
        if (sManagedType == "Plasma.Color")
          return ReadByteSpan<plColor>(value);
        if (sManagedType == "Plasma.Transform")
          return ReadByteSpan<plTransform>(value);
        return {};
      case plCSharpValueKind::ObjectHandle:
      {
        plCSharpObjectHandle handle;
        handle.m_uiValue = value.m_uiPayload0;
        handle.m_uiGeneration = value.m_uiPayload1;
        handle.m_uiKind = static_cast<plUInt32>(DecodeObjectKind(value.m_Flags));

        if (handle.m_uiKind == 0 && sManagedType == "Plasma.World")
          handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::World);
        else if (handle.m_uiKind == 0 && sManagedType == "Plasma.GameObject")
          handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::GameObject);
        else if (handle.m_uiKind == 0 && sManagedType == "Plasma.Component")
          handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::Component);

        void* pObject = nullptr;
        const plRTTI* pType = nullptr;
        auto& registry = plCSharpObjectRegistry::GetSingleton();
        if (handle.m_uiKind != 0)
        {
          if (registry.ResolveObject(handle, pObject, pType) !=
              plCSharpStatus::Success)
          {
            return {};
          }
        }
        else
        {
          plCSharpNativeObjectKind kind = plCSharpNativeObjectKind::Invalid;
          if (registry.ResolveObject(handle.m_uiValue, handle.m_uiGeneration,
                pObject, pType, kind) != plCSharpStatus::Success)
          {
            return {};
          }
        }

        return plTypedPointer(pObject, pType);
      }
      default:
        return {};
    }
  }
} // namespace

plCSharpGenerationContext::plCSharpGenerationContext(plCSharpGenerationLease&& lease)
  : m_Lease(std::move(lease))
{
}

plCSharpGenerationContext::~plCSharpGenerationContext()
{
  plCSharpProjectRuntime::GetSingleton().ReleaseGeneration(m_Lease);
}

plCSharpInstance::plCSharpInstance(plReflectedClass& inout_owner, plWorld* pWorld,
  const plSharedPtr<const plCSharpClassData>& pClassData)
  : plScriptInstance(inout_owner, pWorld)
  , m_pClassData(pClassData)
{
  if (pWorld == nullptr || m_pClassData == nullptr || m_pClassData->m_pGeneration == nullptr)
    return;

  auto* pComponent = plDynamicCast<plScriptComponent*>(&inout_owner);
  if (pComponent == nullptr || pComponent->GetOwner() == nullptr)
    return;

  m_sOwnerName = pComponent->GetOwner()->GetName();
  if (m_sOwnerName.IsEmpty())
    m_sOwnerName = "<unnamed>";

  auto& registry = plCSharpObjectRegistry::GetSingleton();
  m_OwnerHandle = registry.RegisterObject(
    pComponent->GetOwner(), plGetStaticRTTI<plGameObject>(),
    plCSharpNativeObjectKind::GameObject);
  m_WorldHandle = registry.RegisterObject(
    pWorld, plGetStaticRTTI<plWorld>(), plCSharpNativeObjectKind::World);
  m_ComponentHandle = registry.RegisterObject(
    pComponent, pComponent->GetDynamicRTTI(), plCSharpNativeObjectKind::Component);

  const plCSharpManagedApiV1* pApi = plCSharpProjectRuntime::GetSingleton().GetManagedApi();
  if (pApi == nullptr)
    return;

  plCSharpInstanceCreateDesc desc;
  desc.m_uiGeneration = m_pClassData->m_pGeneration->GetGeneration();
  desc.m_uiTypeId = m_pClassData->m_uiTypeId;
  desc.m_Owner = m_OwnerHandle;
  desc.m_World = m_WorldHandle;
  desc.m_OwnerComponent = m_ComponentHandle;

  const plCSharpStatus status = pApi->m_CreateInstance(&desc, &m_uiInstance);
  if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("create C# script instance", status);
    m_uiInstance = 0;
    return;
  }

  plLog::Info("C# script instance '{}' created on game object '{}'.",
    m_pClassData->m_sManagedName, m_sOwnerName);
}

plCSharpInstance::~plCSharpInstance()
{
  if (m_uiInstance != 0)
  {
    if (const plCSharpManagedApiV1* pApi =
          plCSharpProjectRuntime::GetSingleton().GetManagedApi())
    {
      const plCSharpStatus status = pApi->m_DestroyInstance(m_uiInstance);
      if (status != plCSharpStatus::Success)
      {
        if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
          pHost->LogManagedError("destroy C# script instance", status);
      }
    }
  }

  auto& registry = plCSharpObjectRegistry::GetSingleton();
  for (const plCSharpObjectHandle& handle : m_BorrowedHandles)
    registry.UnregisterObject(handle);
  registry.UnregisterObject(m_ComponentHandle);
  registry.UnregisterObject(m_WorldHandle);
  registry.UnregisterObject(m_OwnerHandle);
}

plResult plCSharpInstance::InvokeLifecycle(plUInt64 uiMethodId, plTime deltaTime)
{
  const plCSharpManagedApiV1* pApi = plCSharpProjectRuntime::GetSingleton().GetManagedApi();
  if (pApi == nullptr || m_uiInstance == 0)
    return PL_FAILURE;

  plCSharpValue argument;
  const plCSharpValue* pArguments = nullptr;
  plUInt32 uiArgumentCount = 0;
  if (uiMethodId == plComponent_ScriptBaseClassFunctions::Update)
  {
    argument = MakeDoubleValue(deltaTime.GetSeconds());
    pArguments = &argument;
    uiArgumentCount = 1;
  }

  plCSharpValue result;
  plCSharpExecutionScope scope(GetWorld(), &m_BorrowedHandles);
  const plCSharpStatus status =
    pApi->m_InvokeMethod(m_uiInstance, uiMethodId, pArguments, uiArgumentCount, &result);
  pApi->m_ReleaseValue(&result);

  if (status != plCSharpStatus::Success)
  {
    const plUInt8 uiFailureBit = uiMethodId < plComponent_ScriptBaseClassFunctions::Count
                                   ? static_cast<plUInt8>(PL_BIT(uiMethodId))
                                   : 0;
    if (uiFailureBit == 0 || (m_uiLoggedLifecycleFailures & uiFailureBit) == 0)
    {
      m_uiLoggedLifecycleFailures |= uiFailureBit;
      if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      {
        plStringBuilder operation;
        operation.SetFormat("script '{}', {} on game object '{}'",
          m_pClassData->m_sManagedName, GetLifecycleName(uiMethodId), m_sOwnerName);
        pHost->LogManagedError(operation, status);
      }
    }
    return PL_FAILURE;
  }

  if (uiMethodId < plComponent_ScriptBaseClassFunctions::Count)
    m_uiLoggedLifecycleFailures &= ~static_cast<plUInt8>(PL_BIT(uiMethodId));

  if (uiMethodId == plComponent_ScriptBaseClassFunctions::OnSimulationStarted &&
      !m_bSimulationStartLogged)
  {
    m_bSimulationStartLogged = true;
    plLog::Success("C# script '{}' started simulation on game object '{}'.",
      m_pClassData->m_sManagedName, m_sOwnerName);
  }

  return PL_SUCCESS;
}

plResult plCSharpInstance::DispatchMessage(
  plUInt64 uiMessageHandlerId, plStringView sPayloadJson)
{
  const plCSharpManagedApiV1* pApi = plCSharpProjectRuntime::GetSingleton().GetManagedApi();
  if (pApi == nullptr || m_uiInstance == 0)
    return PL_FAILURE;

  plCSharpValue payload;
  payload.m_Kind = plCSharpValueKind::Utf8String;
  payload.m_uiPayload0 = reinterpret_cast<plUInt64>(sPayloadJson.GetStartPointer());
  payload.m_uiPayload1 = sPayloadJson.GetElementCount();

  plCSharpExecutionScope scope(GetWorld(), &m_BorrowedHandles);
  const plCSharpStatus status =
    pApi->m_DispatchMessage(m_uiInstance, uiMessageHandlerId, &payload);
  if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("dispatch native message to C#", status);
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

plResult plCSharpInstance::DispatchManagedMessage(
  plUInt64 uiMessageId, plUInt64 uiPayloadToken)
{
  const plCSharpManagedApiV1* pApi =
    plCSharpProjectRuntime::GetSingleton().GetManagedApi();
  if (pApi == nullptr || m_uiInstance == 0)
    return PL_FAILURE;

  plCSharpValue payload;
  payload.m_Kind = plCSharpValueKind::UInt64;
  payload.m_uiPayload0 = uiPayloadToken;

  plCSharpExecutionScope scope(GetWorld(), &m_BorrowedHandles);
  const plCSharpStatus status =
    pApi->m_DispatchMessage(m_uiInstance, uiMessageId, &payload);
  if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("dispatch custom C# message", status);
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

const plCSharpFieldInfo* plCSharpInstance::FindField(const plHashedString& sName) const
{
  if (m_pClassData == nullptr)
    return nullptr;

  for (const plCSharpFieldInfo& field : m_pClassData->m_Fields)
  {
    if (field.m_sName == sName)
      return &field;
  }

  return nullptr;
}

plResult plCSharpInstance::SetField(
  const plCSharpFieldInfo& field, const plVariant& value)
{
  const plCSharpManagedApiV1* pApi = plCSharpProjectRuntime::GetSingleton().GetManagedApi();
  if (pApi == nullptr || m_uiInstance == 0)
    return PL_FAILURE;

  plCSharpExecutionScope scope(GetWorld(), &m_BorrowedHandles);
  plCSharpValue managedValue;
  if (VariantToManagedValue(value, managedValue).Failed())
  {
    plLog::Error("C# exposed field '{}' does not support native value type '{}'.",
      field.m_sName, value.GetType());
    return PL_FAILURE;
  }

  const plCSharpStatus status =
    pApi->m_SetField(m_uiInstance, field.m_uiId, &managedValue);
  if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("set C# exposed field", status);
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

plVariant plCSharpInstance::GetField(const plCSharpFieldInfo& field) const
{
  const plCSharpManagedApiV1* pApi = plCSharpProjectRuntime::GetSingleton().GetManagedApi();
  if (pApi == nullptr || m_uiInstance == 0)
    return {};

  plCSharpValue managedValue;
  plCSharpExecutionScope scope(GetWorld(), &m_BorrowedHandles);
  const plCSharpStatus status =
    pApi->m_GetField(m_uiInstance, field.m_uiId, &managedValue);
  if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("get C# exposed field", status);
    return {};
  }

  plVariant result = ManagedValueToVariant(managedValue, field.m_sManagedType);
  pApi->m_ReleaseValue(&managedValue);
  return result;
}

void plCSharpInstance::SetInstanceVariable(
  const plHashedString& sName, const plVariant& value)
{
  if (const plCSharpFieldInfo* pField = FindField(sName))
    SetField(*pField, value).IgnoreResult();
}

plResult plCSharpInstance::PrepareForReload(
  const plArrayMap<plHashedString, plVariant>& parameters,
  const plArrayMap<plHashedString, plVariant>& reloadState)
{
  auto applyValues = [this](const plArrayMap<plHashedString, plVariant>& values) -> plResult
  {
    for (const auto& entry : values)
    {
      const plCSharpFieldInfo* pField = FindField(entry.key);
      if (pField != nullptr && SetField(*pField, entry.value).Failed())
        return PL_FAILURE;
    }
    return PL_SUCCESS;
  };

  PL_SUCCEED_OR_RETURN(applyValues(parameters));
  PL_SUCCEED_OR_RETURN(applyValues(reloadState));
  return InvokeLifecycle(plComponent_ScriptBaseClassFunctions::Initialize);
}

void plCSharpInstance::CancelReloadPreparation()
{
  // Initialize may have completed part of the script's setup before managed
  // code reported an exception. Give the candidate its matching lifecycle
  // cleanup while the component still redirects self-access to this instance.
  InvokeLifecycle(plComponent_ScriptBaseClassFunctions::Deinitialize).IgnoreResult();
}

plVariant plCSharpInstance::GetInstanceVariable(const plHashedString& sName)
{
  if (const plCSharpFieldInfo* pField = FindField(sName))
    return GetField(*pField);

  return {};
}

void plCSharpInstance::GetInstanceVariables(
  plArrayMap<plHashedString, plVariant>& out_parameters) const
{
  out_parameters.Clear();
  if (m_pClassData == nullptr)
    return;

  for (const plCSharpFieldInfo& field : m_pClassData->m_Fields)
  {
    const plVariant value = GetField(field);
    if (value.IsValid())
      out_parameters.Insert(field.m_sName, value);
  }
}
