#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Runtime/CSharpBindingRuntime.h>
#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>
#include <Core/Scripting/Bindings/ScriptBindingManifest.h>
#include <Core/World/Component.h>
#include <Core/World/GameObject.h>
#include <Core/World/World.h>
#include <Core/World/WorldModule.h>
#include <Foundation/Memory/Allocator.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Types/TypedPointer.h>

static_assert(sizeof(plTime) == 8);
static_assert(sizeof(plAngle) == 4);
static_assert(sizeof(plVec2) == 8);
static_assert(sizeof(plVec2U32) == 8);
static_assert(sizeof(plVec3) == 12);
static_assert(sizeof(plVec4) == 16);
static_assert(sizeof(plQuat) == 16);
static_assert(sizeof(plColor) == 16);
static_assert(sizeof(plTransform) == 40);

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

  static bool IsObjectInCurrentWorld(
    void* pObject, const plRTTI* pObjectType)
  {
    plWorld* pCurrentWorld = plCSharpExecutionScope::GetCurrentWorld();
    if (pCurrentWorld == nullptr || pObject == nullptr || pObjectType == nullptr)
      return false;

    if (pObjectType->IsDerivedFrom<plWorld>())
      return pObject == pCurrentWorld;
    if (pObjectType->IsDerivedFrom<plGameObject>())
      return static_cast<plGameObject*>(pObject)->GetWorld() == pCurrentWorld;
    if (pObjectType->IsDerivedFrom<plComponent>())
      return static_cast<plComponent*>(pObject)->GetWorld() == pCurrentWorld;
    if (pObjectType->IsDerivedFrom<plWorldModule>())
      return static_cast<plWorldModule*>(pObject)->GetWorld() == pCurrentWorld;

    return true;
  }

  static bool IsTypeNamed(const plRTTI* pType, plStringView sName)
  {
    return pType != nullptr && pType->GetTypeName() == sName;
  }

  static plResult ConvertObjectPointerToValue(
    void* pObject, const plRTTI* pObjectType, plCSharpValue& out_value)
  {
    if (pObject == nullptr)
    {
      out_value = {};
      return PL_SUCCESS;
    }
    if (pObjectType == nullptr || !IsObjectInCurrentWorld(pObject, pObjectType))
      return PL_FAILURE;

    plCSharpNativeObjectKind kind = plCSharpNativeObjectKind::ReflectedObject;
    if (pObjectType->IsDerivedFrom<plWorld>())
      kind = plCSharpNativeObjectKind::World;
    else if (pObjectType->IsDerivedFrom<plGameObject>())
      kind = plCSharpNativeObjectKind::GameObject;
    else if (pObjectType->IsDerivedFrom<plComponent>())
      kind = plCSharpNativeObjectKind::Component;

    const plCSharpObjectHandle handle =
      plCSharpObjectRegistry::GetSingleton().RegisterObject(pObject, pObjectType, kind);
    if (handle.m_uiValue == 0)
      return PL_FAILURE;

    if (!plCSharpExecutionScope::TrackBorrowedHandle(handle))
    {
      plCSharpObjectRegistry::GetSingleton().UnregisterObject(handle);
      return PL_FAILURE;
    }

    out_value = {};
    out_value.m_Kind = plCSharpValueKind::ObjectHandle;
    out_value.m_Flags = EncodeObjectKind(kind);
    out_value.m_uiPayload0 = handle.m_uiValue;
    out_value.m_uiPayload1 = handle.m_uiGeneration;
    return PL_SUCCESS;
  }

  static plResult ConvertValueToVariant(
    const plCSharpValue& value, const plRTTI* pExpectedType,
    plBitflags<plPropertyFlags> flags, plVariant& out_value)
  {
    if (value.m_Kind == plCSharpValueKind::Null)
    {
      out_value = plReflectionUtils::GetDefaultVariantFromType(pExpectedType);
      return PL_SUCCESS;
    }

    switch (value.m_Kind)
    {
      case plCSharpValueKind::Boolean:
        out_value = value.m_uiPayload0 != 0;
        break;
      case plCSharpValueKind::Int64:
        out_value = static_cast<plInt64>(value.m_uiPayload0);
        break;
      case plCSharpValueKind::UInt64:
        if (pExpectedType != nullptr &&
            pExpectedType->GetVariantType() == plVariantType::TempHashedString)
        {
          out_value = plTempHashedString(value.m_uiPayload0);
          return PL_SUCCESS;
        }
        out_value = value.m_uiPayload0;
        break;
      case plCSharpValueKind::Double:
      {
        double number = 0.0;
        plMemoryUtils::RawByteCopy(&number, &value.m_uiPayload0, sizeof(number));
        out_value = number;
        break;
      }
      case plCSharpValueKind::Utf8String:
      {
        if (value.m_uiPayload0 == 0 && value.m_uiPayload1 != 0)
          return PL_FAILURE;

        const plStringView text(
          reinterpret_cast<const char*>(value.m_uiPayload0),
          static_cast<plUInt32>(value.m_uiPayload1));
        if (pExpectedType != nullptr &&
            pExpectedType->GetVariantType() == plVariantType::TempHashedString)
        {
          out_value = plTempHashedString(text);
          return PL_SUCCESS;
        }
        out_value = text;
        break;
      }
      case plCSharpValueKind::ObjectHandle:
      {
        plCSharpObjectHandle handle;
        handle.m_uiValue = value.m_uiPayload0;
        handle.m_uiGeneration = value.m_uiPayload1;
        handle.m_uiKind = static_cast<plUInt32>(DecodeObjectKind(value.m_Flags));

        if (handle.m_uiKind == 0 && pExpectedType != nullptr)
        {
          if (pExpectedType->IsDerivedFrom<plWorld>())
            handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::World);
          else if (pExpectedType->IsDerivedFrom<plGameObject>())
            handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::GameObject);
          else if (pExpectedType->IsDerivedFrom<plComponent>())
            handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::Component);
          else
            handle.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::ReflectedObject);
        }

        void* pObject = nullptr;
        const plRTTI* pObjectType = nullptr;
        if (plCSharpObjectRegistry::GetSingleton().ResolveObject(handle, pObject, pObjectType) != plCSharpStatus::Success)
          return PL_FAILURE;
        if (!IsObjectInCurrentWorld(pObject, pObjectType))
          return PL_FAILURE;

        if (IsTypeNamed(pExpectedType, "plGameObjectHandle"))
        {
          if (pObjectType == nullptr || !pObjectType->IsDerivedFrom<plGameObject>())
            return PL_FAILURE;
          out_value = static_cast<plGameObject*>(pObject)->GetHandle();
          return PL_SUCCESS;
        }
        if (IsTypeNamed(pExpectedType, "plComponentHandle"))
        {
          if (pObjectType == nullptr || !pObjectType->IsDerivedFrom<plComponent>())
            return PL_FAILURE;
          out_value = static_cast<plComponent*>(pObject)->GetHandle();
          return PL_SUCCESS;
        }

        if (pExpectedType != nullptr &&
            (pObjectType == nullptr || !pObjectType->IsDerivedFrom(pExpectedType)))
        {
          return PL_FAILURE;
        }

        out_value = plTypedPointer(pObject, pObjectType);
        return PL_SUCCESS;
      }
      case plCSharpValueKind::ByteSpan:
      {
        if (pExpectedType == nullptr || value.m_uiPayload0 == 0)
        {
          return PL_FAILURE;
        }

        const void* pData = reinterpret_cast<const void*>(value.m_uiPayload0);
        if (IsTypeNamed(pExpectedType, "plColorGammaUB") &&
            value.m_uiPayload1 == sizeof(plColor))
        {
          out_value = plColorGammaUB(*static_cast<const plColor*>(pData));
          return PL_SUCCESS;
        }
        if (value.m_uiPayload1 != pExpectedType->GetTypeSize())
          return PL_FAILURE;

        switch (pExpectedType->GetVariantType())
        {
          case plVariantType::Time:
            out_value = *static_cast<const plTime*>(pData);
            return PL_SUCCESS;
          case plVariantType::Angle:
            out_value = *static_cast<const plAngle*>(pData);
            return PL_SUCCESS;
          case plVariantType::Vector2:
            out_value = *static_cast<const plVec2*>(pData);
            return PL_SUCCESS;
          case plVariantType::Vector2U:
            out_value = *static_cast<const plVec2U32*>(pData);
            return PL_SUCCESS;
          case plVariantType::Vector3:
            out_value = *static_cast<const plVec3*>(pData);
            return PL_SUCCESS;
          case plVariantType::Vector4:
            out_value = *static_cast<const plVec4*>(pData);
            return PL_SUCCESS;
          case plVariantType::Quaternion:
            out_value = *static_cast<const plQuat*>(pData);
            return PL_SUCCESS;
          case plVariantType::Color:
            out_value = *static_cast<const plColor*>(pData);
            return PL_SUCCESS;
          case plVariantType::Transform:
            out_value = *static_cast<const plTransform*>(pData);
            return PL_SUCCESS;
          default:
            out_value.CopyTypedObject(pData, pExpectedType);
            return PL_SUCCESS;
        }
      }
      default:
        return PL_FAILURE;
    }

    if (pExpectedType == nullptr || flags.IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags))
      return PL_SUCCESS;

    const plVariantType::Enum targetType = pExpectedType->GetVariantType();
    if (targetType == plVariantType::Invalid || targetType == plVariantType::TypedPointer ||
        targetType == plVariantType::TypedObject)
    {
      return PL_SUCCESS;
    }

    plResult conversionResult = PL_FAILURE;
    plVariant converted = out_value.ConvertTo(targetType, &conversionResult);
    if (conversionResult.Failed())
      return PL_FAILURE;

    out_value = std::move(converted);
    return PL_SUCCESS;
  }

  static plResult AllocateResultBytes(
    plCSharpValueKind kind, const void* pData, plUInt32 uiSize, plCSharpValue& out_value)
  {
    void* pCopy = nullptr;
    if (uiSize > 0)
    {
      // plAllocator requires a non-zero power-of-two alignment. These buffers
      // cross the native/managed ABI as opaque bytes, so the platform minimum is
      // sufficient and keeps strings and value-type return data safely aligned.
      pCopy = plFoundation::GetDefaultAllocator()->Allocate(
        uiSize, PL_ALIGNMENT_MINIMUM);
      if (pCopy == nullptr)
        return PL_FAILURE;
      plMemoryUtils::RawByteCopy(pCopy, pData, uiSize);
    }

    out_value.m_Kind = kind;
    out_value.m_Flags = plCSharpValueFlags::ManagedOwned;
    out_value.m_uiPayload0 = reinterpret_cast<plUInt64>(pCopy);
    out_value.m_uiPayload1 = uiSize;
    return PL_SUCCESS;
  }

  static plResult ConvertVariantToValue(const plVariant& value, plCSharpValue& out_value)
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
      {
        const double number = value.ConvertTo<double>();
        out_value.m_Kind = plCSharpValueKind::Double;
        plMemoryUtils::RawByteCopy(&out_value.m_uiPayload0, &number, sizeof(number));
        return PL_SUCCESS;
      }
      case plVariantType::String:
      {
        const plString& string = value.Get<plString>();
        return AllocateResultBytes(plCSharpValueKind::Utf8String, string.GetData(),
          string.GetElementCount(), out_value);
      }
      case plVariantType::StringView:
      {
        const plStringView string = value.Get<plStringView>();
        return AllocateResultBytes(plCSharpValueKind::Utf8String, string.GetStartPointer(),
          string.GetElementCount(), out_value);
      }
      case plVariantType::HashedString:
      {
        const plStringView string = value.Get<plHashedString>().GetView();
        return AllocateResultBytes(plCSharpValueKind::Utf8String,
          string.GetStartPointer(), string.GetElementCount(), out_value);
      }
      case plVariantType::TempHashedString:
        out_value.m_Kind = plCSharpValueKind::UInt64;
        out_value.m_uiPayload0 = value.Get<plTempHashedString>().GetHash();
        return PL_SUCCESS;
      case plVariantType::Time:
      {
        const plTime& data = value.Get<plTime>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Angle:
      {
        const plAngle& data = value.Get<plAngle>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Vector2:
      {
        const plVec2& data = value.Get<plVec2>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Vector2U:
      {
        const plVec2U32& data = value.Get<plVec2U32>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Vector3:
      {
        const plVec3& data = value.Get<plVec3>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Vector4:
      {
        const plVec4& data = value.Get<plVec4>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Quaternion:
      {
        const plQuat& data = value.Get<plQuat>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Color:
      {
        const plColor& data = value.Get<plColor>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::ColorGamma:
      {
        const plColor data = value.Get<plColorGammaUB>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::Transform:
      {
        const plTransform& data = value.Get<plTransform>();
        return AllocateResultBytes(plCSharpValueKind::ByteSpan, &data, sizeof(data), out_value);
      }
      case plVariantType::TypedPointer:
      {
        const plTypedPointer pointer = value.Get<plTypedPointer>();
        return ConvertObjectPointerToValue(
          pointer.m_pObject, pointer.m_pType, out_value);
      }
      case plVariantType::TypedObject:
      {
        const plTypedObject object = value.Get<plTypedObject>();
        plWorld* pWorld = plCSharpExecutionScope::GetCurrentWorld();
        if (pWorld == nullptr || object.m_pObject == nullptr || object.m_pType == nullptr)
          return PL_FAILURE;

        if (IsTypeNamed(object.m_pType, "plGameObjectHandle"))
        {
          plGameObject* pGameObject = nullptr;
          if (!pWorld->TryGetObject(
                *static_cast<const plGameObjectHandle*>(object.m_pObject), pGameObject))
            return PL_SUCCESS;
          return ConvertObjectPointerToValue(
            pGameObject, plGetStaticRTTI<plGameObject>(), out_value);
        }
        if (IsTypeNamed(object.m_pType, "plComponentHandle"))
        {
          plComponent* pComponent = nullptr;
          if (!pWorld->TryGetComponent(
                *static_cast<const plComponentHandle*>(object.m_pObject), pComponent))
            return PL_SUCCESS;
          return ConvertObjectPointerToValue(
            pComponent, pComponent->GetDynamicRTTI(), out_value);
        }
        return PL_FAILURE;
      }
      default:
        return PL_FAILURE;
    }
  }

  static bool IsInjectedWorldParameter(const plScriptBindingParameterDesc& parameter)
  {
    return parameter.m_sTypeName == "plWorld" &&
           parameter.m_Flags.IsAnySet(plPropertyFlags::Pointer | plPropertyFlags::Reference);
  }
} // namespace

plCSharpBindingRuntime& plCSharpBindingRuntime::GetSingleton()
{
  static plCSharpBindingRuntime runtime;
  return runtime;
}

plResult plCSharpBindingRuntime::Startup()
{
  PL_LOCK(m_Mutex);
  if (!m_bSubscribedToPluginEvents)
  {
    plPlugin::Events().AddEventHandler(
      plMakeDelegate(&plCSharpBindingRuntime::PluginEventHandler, this));
    m_bSubscribedToPluginEvents = true;
  }

  m_bPluginChangesInProgress = false;
  return PL_SUCCESS;
}

void plCSharpBindingRuntime::Shutdown()
{
  if (m_bSubscribedToPluginEvents)
  {
    plPlugin::Events().RemoveEventHandler(
      plMakeDelegate(&plCSharpBindingRuntime::PluginEventHandler, this));
  }

  PL_LOCK(m_Mutex);
  m_bSubscribedToPluginEvents = false;
  m_bPluginChangesInProgress = true;
  m_bSnapshotValid = false;
  m_uiSchemaHash = 0;
  m_Snapshot = {};
}

plResult plCSharpBindingRuntime::Refresh()
{
  PL_LOCK(m_Mutex);
  if (m_bPluginChangesInProgress)
    return PL_FAILURE;

  m_bSnapshotValid = false;
  m_uiSchemaHash = 0;
  return EnsureSnapshotLocked() ? PL_SUCCESS : PL_FAILURE;
}

plResult plCSharpBindingRuntime::GetSchemaHash(plUInt64& out_uiSchemaHash)
{
  out_uiSchemaHash = 0;
  if (Startup().Failed())
    return PL_FAILURE;

  PL_LOCK(m_Mutex);
  if (m_bPluginChangesInProgress || !EnsureSnapshotLocked())
    return PL_FAILURE;

  out_uiSchemaHash = m_uiSchemaHash;
  return out_uiSchemaHash != 0 ? PL_SUCCESS : PL_FAILURE;
}

plCSharpStatus plCSharpBindingRuntime::ValidateObject(
  plCSharpObjectHandle object, plUInt32 uiExpectedKind) const
{
  if (plCSharpExecutionScope::GetCurrentWorld() == nullptr)
    return plCSharpStatus::InvalidArgument;

  auto& registry = plCSharpObjectRegistry::GetSingleton();
  const plCSharpStatus validationStatus = registry.ValidateObject(
    object, static_cast<plCSharpNativeObjectKind>(uiExpectedKind));
  if (validationStatus != plCSharpStatus::Success)
    return validationStatus;

  void* pObject = nullptr;
  const plRTTI* pObjectType = nullptr;
  if (registry.ResolveObject(object, pObject, pObjectType) != plCSharpStatus::Success ||
      !IsObjectInCurrentWorld(pObject, pObjectType))
  {
    return plCSharpStatus::InvalidArgument;
  }
  return plCSharpStatus::Success;
}

plCSharpStatus plCSharpBindingRuntime::InvokeReflected(plUInt64 uiBindingId,
  plCSharpObjectHandle target, const plCSharpValue* pArguments,
  plUInt32 uiArgumentCount, plCSharpValue* out_pResult)
{
  if (out_pResult == nullptr || (uiArgumentCount > 0 && pArguments == nullptr))
    return plCSharpStatus::InvalidArgument;
  if (plCSharpExecutionScope::GetCurrentWorld() == nullptr)
    return plCSharpStatus::InvalidArgument;

  *out_pResult = {};

  // Hold the recursive mutex through invocation. BeforePluginChanges takes the
  // same lock and marks the cache unavailable before any owning DLL can unload,
  // so the raw RTTI/property pointers remain callable for this entire scope.
  PL_LOCK(m_Mutex);
  if (m_bPluginChangesInProgress || !EnsureSnapshotLocked())
    return plCSharpStatus::NotInitialized;

  if (const auto* pFunction = m_Snapshot.FindFunction(uiBindingId))
  {
    return InvokeFunction(
      *pFunction, target, pArguments, uiArgumentCount, out_pResult);
  }

  if (const auto* pProperty = m_Snapshot.FindProperty(uiBindingId))
  {
    return InvokeProperty(
      *pProperty, target, pArguments, uiArgumentCount, out_pResult);
  }

  return plCSharpStatus::MemberNotFound;
}

plCSharpStatus plCSharpBindingRuntime::ReleaseValue(plCSharpValue* pValue) const
{
  if (pValue == nullptr)
    return plCSharpStatus::InvalidArgument;

  if ((static_cast<plUInt32>(pValue->m_Flags) &
        static_cast<plUInt32>(plCSharpValueFlags::ManagedOwned)) != 0 &&
      pValue->m_uiPayload0 != 0)
  {
    plFoundation::GetDefaultAllocator()->Deallocate(
      reinterpret_cast<void*>(pValue->m_uiPayload0));
  }

  *pValue = {};
  return plCSharpStatus::Success;
}

void plCSharpBindingRuntime::PluginEventHandler(
  const plPluginEvent& eventData)
{
  PL_LOCK(m_Mutex);
  switch (eventData.m_EventType)
  {
    case plPluginEvent::BeforePluginChanges:
      m_bPluginChangesInProgress = true;
      m_bSnapshotValid = false;
      m_uiSchemaHash = 0;
      m_Snapshot = {};
      break;
    case plPluginEvent::AfterPluginChanges:
      m_bPluginChangesInProgress = false;
      m_bSnapshotValid = false;
      m_uiSchemaHash = 0;
      break;
    default:
      break;
  }
}

bool plCSharpBindingRuntime::EnsureSnapshotLocked()
{
  if (m_bPluginChangesInProgress)
    return false;

  if (!m_bSnapshotValid)
  {
    PL_PROFILE_SCOPE("CSharp.CreateBindingSnapshot");
    m_bSnapshotValid = plScriptBindingRegistry::CreateSnapshot(m_Snapshot).Succeeded();
    if (m_bSnapshotValid)
    {
      m_uiSchemaHash = plScriptBindingManifest::ComputeSchemaHash(m_Snapshot);
      m_bSnapshotValid = m_uiSchemaHash != 0;
    }

    if (!m_bSnapshotValid)
    {
      m_uiSchemaHash = 0;
      m_Snapshot.Clear();
    }
  }
  return m_bSnapshotValid;
}

plCSharpStatus plCSharpBindingRuntime::InvokeFunction(
  const plScriptBindingFunctionDesc& function, plCSharpObjectHandle target,
  const plCSharpValue* pArguments, plUInt32 uiArgumentCount, plCSharpValue* out_pResult)
{
  void* pInstance = nullptr;
  if (function.m_FunctionType == plFunctionType::Member)
  {
    const plRTTI* pTargetType = nullptr;
    if (plCSharpObjectRegistry::GetSingleton().ResolveObject(target, pInstance, pTargetType) != plCSharpStatus::Success)
      return plCSharpStatus::InvalidArgument;
    if (!IsObjectInCurrentWorld(pInstance, pTargetType))
      return plCSharpStatus::InvalidArgument;

    const plRTTI* pDeclaringType = plRTTI::FindTypeByName(function.m_sDeclaringTypeName);
    if (pTargetType == nullptr || pDeclaringType == nullptr || !pTargetType->IsDerivedFrom(pDeclaringType))
    {
      return plCSharpStatus::InvalidArgument;
    }
  }

  plDynamicArray<plVariant> arguments;
  arguments.SetCount(function.m_Parameters.GetCount());
  plHybridArray<plInt32, 16> nativeArgumentIndices;
  nativeArgumentIndices.SetCount(function.m_Parameters.GetCount(), -1);

  plUInt32 uiInputIndex = 0;
  for (plUInt32 uiParameter = 0; uiParameter < function.m_Parameters.GetCount(); ++uiParameter)
  {
    const auto& parameter = function.m_Parameters[uiParameter];
    if (IsInjectedWorldParameter(parameter))
    {
      plWorld* pWorld = plCSharpExecutionScope::GetCurrentWorld();
      if (pWorld == nullptr)
        return plCSharpStatus::InvalidArgument;
      arguments[uiParameter] = plTypedPointer(pWorld, parameter.m_pType);
      continue;
    }

    if (uiInputIndex >= uiArgumentCount)
      return plCSharpStatus::InvalidArgument;

    nativeArgumentIndices[uiParameter] = static_cast<plInt32>(uiInputIndex);
    if (ConvertValueToVariant(pArguments[uiInputIndex], parameter.m_pType,
          parameter.m_Flags, arguments[uiParameter])
          .Failed())
    {
      return plCSharpStatus::InvalidValue;
    }
    ++uiInputIndex;
  }

  if (uiInputIndex != uiArgumentCount)
    return plCSharpStatus::InvalidArgument;

  plVariant result;
  function.m_pFunction->Execute(pInstance, arguments, result);

  for (plUInt32 uiParameter = 0; uiParameter < function.m_Parameters.GetCount(); ++uiParameter)
  {
    const auto& parameter = function.m_Parameters[uiParameter];
    if (parameter.m_Direction == plScriptBindingParameterDirection::In ||
        nativeArgumentIndices[uiParameter] < 0)
    {
      continue;
    }

    plCSharpValue& outputArgument =
      const_cast<plCSharpValue*>(pArguments)[nativeArgumentIndices[uiParameter]];
    if (ConvertVariantToValue(arguments[uiParameter], outputArgument).Failed())
      return plCSharpStatus::InvalidValue;
  }

  return ConvertVariantToValue(result, *out_pResult).Succeeded()
           ? plCSharpStatus::Success
           : plCSharpStatus::InvalidValue;
}

plCSharpStatus plCSharpBindingRuntime::InvokeProperty(
  const plScriptBindingPropertyDesc& property, plCSharpObjectHandle target,
  const plCSharpValue* pArguments, plUInt32 uiArgumentCount, plCSharpValue* out_pResult)
{
  if (property.m_Category != plPropertyCategory::Member)
    return plCSharpStatus::Unsupported;

  void* pInstance = nullptr;
  const plRTTI* pTargetType = nullptr;
  if (plCSharpObjectRegistry::GetSingleton().ResolveObject(target, pInstance, pTargetType) != plCSharpStatus::Success)
    return plCSharpStatus::InvalidArgument;
  if (!IsObjectInCurrentWorld(pInstance, pTargetType))
    return plCSharpStatus::InvalidArgument;

  const plRTTI* pDeclaringType = plRTTI::FindTypeByName(property.m_sDeclaringTypeName);
  if (pDeclaringType == nullptr || pTargetType == nullptr || !pTargetType->IsDerivedFrom(pDeclaringType))
    return plCSharpStatus::InvalidArgument;

  const auto* pMember = static_cast<const plAbstractMemberProperty*>(property.m_pProperty);
  if (uiArgumentCount == 0)
  {
    const plVariant value = plReflectionUtils::GetMemberPropertyValue(pMember, pInstance);
    return ConvertVariantToValue(value, *out_pResult).Succeeded()
             ? plCSharpStatus::Success
             : plCSharpStatus::InvalidValue;
  }

  if (uiArgumentCount != 1 || property.m_Flags.IsSet(plPropertyFlags::ReadOnly))
    return plCSharpStatus::InvalidArgument;

  plVariant value;
  if (ConvertValueToVariant(*pArguments,
        property.m_pProperty->GetSpecificType(), property.m_Flags, value)
        .Failed())
  {
    return plCSharpStatus::InvalidValue;
  }

  plReflectionUtils::SetMemberPropertyValue(pMember, pInstance, value);
  return plCSharpStatus::Success;
}
