#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>
#include <Core/World/Component.h>
#include <Core/World/GameObject.h>
#include <Core/World/World.h>

namespace
{
  constexpr plUInt32 s_uiMaxBorrowedHandlesPerInstance = 4096;

  thread_local plWorld* s_pCurrentScriptWorld = nullptr;
  thread_local plDynamicArray<plCSharpObjectHandle>* s_pBorrowedHandles = nullptr;
} // namespace

plCSharpObjectRegistry& plCSharpObjectRegistry::GetSingleton()
{
  static plCSharpObjectRegistry registry;
  return registry;
}

plCSharpObjectHandle plCSharpObjectRegistry::RegisterObject(
  void* pObject, const plRTTI* pType, plCSharpNativeObjectKind kind)
{
  if (pObject == nullptr || kind == plCSharpNativeObjectKind::Invalid)
    return {};

  if (kind == plCSharpNativeObjectKind::Component)
    pType = static_cast<plComponent*>(pObject)->GetDynamicRTTI();

  PL_LOCK(m_Mutex);

  for (auto it = m_Entries.GetIterator(); it.IsValid(); ++it)
  {
    Entry& existing = it.Value();
    if (existing.m_pObject != pObject || existing.m_pType != pType ||
        existing.m_Kind != kind)
    {
      continue;
    }

    // Resolve only the matching identity. Scanning and resolving every global
    // entry here could touch unrelated worlds without their access marker and
    // made registration O(all live handles plus cross-world lookups).
    void* pExistingObject = nullptr;
    if (TryResolveEntry(existing, pExistingObject) &&
        pExistingObject == pObject)
    {
      ++existing.m_uiReferenceCount;
      plCSharpObjectHandle handle;
      handle.m_uiValue = it.Key();
      handle.m_uiGeneration = existing.m_uiGeneration;
      handle.m_uiKind = static_cast<plUInt32>(kind);
      return handle;
    }
  }

  const plUInt64 uiId = m_uiNextId++;
  Entry& entry = m_Entries[uiId];
  entry.m_pObject = pObject;
  entry.m_pType = pType;
  entry.m_uiReferenceCount = 1;
  entry.m_uiGeneration = m_uiNextGeneration++;
  entry.m_Kind = kind;
  if (kind == plCSharpNativeObjectKind::World)
  {
    auto* pWorld = static_cast<plWorld*>(pObject);
    entry.m_pWorld = pWorld;
    entry.m_uiWorldIndex = pWorld->GetIndex();
  }
  else if (kind == plCSharpNativeObjectKind::GameObject)
  {
    auto* pGameObject = static_cast<plGameObject*>(pObject);
    entry.m_pWorld = pGameObject->GetWorld();
    entry.m_hGameObject = pGameObject->GetHandle();
  }
  else if (kind == plCSharpNativeObjectKind::Component)
  {
    auto* pComponent = static_cast<plComponent*>(pObject);
    entry.m_pWorld = pComponent->GetWorld();
    entry.m_hComponent = pComponent->GetHandle();
  }

  plCSharpObjectHandle handle;
  handle.m_uiValue = uiId;
  handle.m_uiGeneration = entry.m_uiGeneration;
  handle.m_uiKind = static_cast<plUInt32>(kind);
  return handle;
}

void plCSharpObjectRegistry::UnregisterObject(plCSharpObjectHandle handle)
{
  if (handle.m_uiValue == 0)
    return;

  PL_LOCK(m_Mutex);
  Entry* pEntry = nullptr;
  if (!m_Entries.TryGetValue(handle.m_uiValue, pEntry) || pEntry == nullptr ||
      pEntry->m_uiGeneration != handle.m_uiGeneration ||
      static_cast<plUInt32>(pEntry->m_Kind) != handle.m_uiKind)
  {
    return;
  }

  if (pEntry->m_uiReferenceCount > 0)
    --pEntry->m_uiReferenceCount;

  // Every managed instance tracks the native handles it receives. Remove the
  // identity as soon as its last tracked reference is released; retaining
  // zero-reference world-backed entries would allow world slot and allocator
  // address reuse to make an old managed handle resolve to a new object.
  if (pEntry->m_uiReferenceCount == 0)
    m_Entries.Remove(handle.m_uiValue);
}

plCSharpStatus plCSharpObjectRegistry::ValidateObject(
  plCSharpObjectHandle handle, plCSharpNativeObjectKind expectedKind) const
{
  void* pObject = nullptr;
  const plRTTI* pType = nullptr;
  const plCSharpStatus status = ResolveObject(handle, pObject, pType);
  if (status != plCSharpStatus::Success)
    return status;

  if (expectedKind != plCSharpNativeObjectKind::Invalid &&
      handle.m_uiKind != static_cast<plUInt32>(expectedKind))
  {
    return plCSharpStatus::InvalidArgument;
  }

  return plCSharpStatus::Success;
}

plCSharpStatus plCSharpObjectRegistry::ResolveObject(
  plCSharpObjectHandle handle, void*& out_pObject, const plRTTI*& out_pType) const
{
  out_pObject = nullptr;
  out_pType = nullptr;

  if (handle.m_uiValue == 0)
    return plCSharpStatus::InvalidArgument;

  PL_LOCK(m_Mutex);

  const Entry* pEntry = nullptr;
  if (!m_Entries.TryGetValue(handle.m_uiValue, pEntry) || pEntry == nullptr ||
      pEntry->m_uiGeneration != handle.m_uiGeneration ||
      static_cast<plUInt32>(pEntry->m_Kind) != handle.m_uiKind)
  {
    return plCSharpStatus::InvalidArgument;
  }

  if (!TryResolveEntry(*pEntry, out_pObject))
    return plCSharpStatus::InvalidArgument;

  out_pType = pEntry->m_pType;
  return plCSharpStatus::Success;
}

plCSharpStatus plCSharpObjectRegistry::ResolveObject(
  plUInt64 uiValue, plUInt64 uiGeneration, void*& out_pObject,
  const plRTTI*& out_pType, plCSharpNativeObjectKind& out_kind) const
{
  out_pObject = nullptr;
  out_pType = nullptr;
  out_kind = plCSharpNativeObjectKind::Invalid;

  if (uiValue == 0)
    return plCSharpStatus::InvalidArgument;

  PL_LOCK(m_Mutex);

  const Entry* pEntry = nullptr;
  if (!m_Entries.TryGetValue(uiValue, pEntry) || pEntry == nullptr ||
      pEntry->m_uiGeneration != uiGeneration)
  {
    return plCSharpStatus::InvalidArgument;
  }

  if (!TryResolveEntry(*pEntry, out_pObject))
    return plCSharpStatus::InvalidArgument;

  out_pType = pEntry->m_pType;
  out_kind = pEntry->m_Kind;
  return plCSharpStatus::Success;
}

bool plCSharpObjectRegistry::TryResolveEntry(
  const Entry& entry, void*& out_pObject) const
{
  out_pObject = nullptr;

  if (entry.m_Kind == plCSharpNativeObjectKind::World)
  {
    if (entry.m_uiWorldIndex >= plWorld::GetWorldCount())
      return false;

    plWorld* pWorld = plWorld::GetWorld(entry.m_uiWorldIndex);
    if (pWorld == nullptr || pWorld != entry.m_pWorld)
      return false;

    out_pObject = pWorld;
    return true;
  }

  if (entry.m_Kind == plCSharpNativeObjectKind::GameObject)
  {
    plWorld* pWorld = plWorld::GetWorld(entry.m_hGameObject);
    plGameObject* pGameObject = nullptr;
    if (pWorld == nullptr || pWorld != entry.m_pWorld ||
        !pWorld->TryGetObject(entry.m_hGameObject, pGameObject))
    {
      return false;
    }
    out_pObject = pGameObject;
    return true;
  }

  if (entry.m_Kind == plCSharpNativeObjectKind::Component)
  {
    plWorld* pWorld = plWorld::GetWorld(entry.m_hComponent);
    plComponent* pComponent = nullptr;
    if (pWorld == nullptr || pWorld != entry.m_pWorld ||
        !pWorld->TryGetComponent(entry.m_hComponent, pComponent))
    {
      return false;
    }
    out_pObject = pComponent;
    return true;
  }

  out_pObject = entry.m_pObject;
  return out_pObject != nullptr;
}

plCSharpExecutionScope::plCSharpExecutionScope(plWorld* pWorld,
  plDynamicArray<plCSharpObjectHandle>* pBorrowedHandles)
  : m_pPreviousWorld(s_pCurrentScriptWorld)
  , m_pPreviousBorrowedHandles(s_pBorrowedHandles)
{
  s_pCurrentScriptWorld = pWorld;
  s_pBorrowedHandles = pBorrowedHandles;
}

plCSharpExecutionScope::~plCSharpExecutionScope()
{
  s_pCurrentScriptWorld = m_pPreviousWorld;
  s_pBorrowedHandles = m_pPreviousBorrowedHandles;
}

plWorld* plCSharpExecutionScope::GetCurrentWorld()
{
  return s_pCurrentScriptWorld;
}

bool plCSharpExecutionScope::TrackBorrowedHandle(plCSharpObjectHandle handle)
{
  if (s_pBorrowedHandles == nullptr || handle.m_uiValue == 0)
    return false;

  for (const plCSharpObjectHandle& existing : *s_pBorrowedHandles)
  {
    if (existing.m_uiValue == handle.m_uiValue &&
        existing.m_uiGeneration == handle.m_uiGeneration &&
        existing.m_uiKind == handle.m_uiKind)
    {
      // RegisterObject added one reference; keep only the instance's original reference.
      plCSharpObjectRegistry::GetSingleton().UnregisterObject(handle);
      return true;
    }
  }

  if (s_pBorrowedHandles->GetCount() >= s_uiMaxBorrowedHandlesPerInstance)
  {
    plLog::Error(
      "A C# script instance exceeded the limit of {} retained native object handles.",
      s_uiMaxBorrowedHandlesPerInstance);
    return false;
  }

  s_pBorrowedHandles->PushBack(handle);
  return true;
}
