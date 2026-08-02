#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>
#include <Core/World/Declarations.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Threading/Mutex.h>

class plRTTI;
class plWorld;

enum class plCSharpNativeObjectKind : plUInt32
{
  Invalid,
  World,
  GameObject,
  Component,
  Resource,
  ReflectedObject,
};

/// Generation-checked handles prevent collectible managed code from retaining native pointers.
class PL_CSHARPPLUGIN_DLL plCSharpObjectRegistry
{
public:
  static plCSharpObjectRegistry& GetSingleton();

  plCSharpObjectHandle RegisterObject(
    void* pObject, const plRTTI* pType, plCSharpNativeObjectKind kind);
  void UnregisterObject(plCSharpObjectHandle handle);

  plCSharpStatus ValidateObject(
    plCSharpObjectHandle handle, plCSharpNativeObjectKind expectedKind) const;
  plCSharpStatus ResolveObject(
    plCSharpObjectHandle handle, void*& out_pObject, const plRTTI*& out_pType) const;
  /// Resolves a generation-checked value when the value transport did not carry
  /// the wrapper kind (for example a generic Plasma.NativeObject field).
  plCSharpStatus ResolveObject(
    plUInt64 uiValue, plUInt64 uiGeneration, void*& out_pObject,
    const plRTTI*& out_pType, plCSharpNativeObjectKind& out_kind) const;

private:
  struct Entry
  {
    void* m_pObject = nullptr;
    const plRTTI* m_pType = nullptr;
    plWorld* m_pWorld = nullptr;
    plUInt32 m_uiWorldIndex = plInvalidIndex;
    plGameObjectHandle m_hGameObject;
    plComponentHandle m_hComponent;
    plUInt64 m_uiGeneration = 0;
    plUInt32 m_uiReferenceCount = 0;
    plCSharpNativeObjectKind m_Kind = plCSharpNativeObjectKind::Invalid;
  };

  mutable plMutex m_Mutex;
  plHashTable<plUInt64, Entry> m_Entries;
  plUInt64 m_uiNextId = 1;
  plUInt64 m_uiNextGeneration = 1;

  bool TryResolveEntry(const Entry& entry, void*& out_pObject) const;
};

/// Scoped native context used for plWorld injection while managed script code calls back into RTTI.
class PL_CSHARPPLUGIN_DLL plCSharpExecutionScope
{
public:
  explicit plCSharpExecutionScope(plWorld* pWorld,
    plDynamicArray<plCSharpObjectHandle>* pBorrowedHandles = nullptr);
  ~plCSharpExecutionScope();

  static plWorld* GetCurrentWorld();
  /// Tracks a returned native object for the current script instance.
  /// Returns false when there is no active managed-script execution scope.
  static bool TrackBorrowedHandle(plCSharpObjectHandle handle);

private:
  plWorld* m_pPreviousWorld = nullptr;
  plDynamicArray<plCSharpObjectHandle>* m_pPreviousBorrowedHandles = nullptr;
};
