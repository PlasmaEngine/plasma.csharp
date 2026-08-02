#pragma once

#include <Foundation/Basics.h>

#include <cstddef>

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
#  define PL_CSHARP_CALL __cdecl
#else
#  define PL_CSHARP_CALL
#endif

constexpr plUInt32 PL_CSHARP_ABI_VERSION = 1;
constexpr plUInt32 PL_CSHARP_VALUE_LAYOUT_VERSION = 2;

enum class plCSharpStatus : plInt32
{
  Success = 0,
  InvalidArgument = 1,
  AbiMismatch = 2,
  NotInitialized = 3,
  RuntimeLoadFailed = 4,
  AssemblyLoadFailed = 5,
  GenerationNotFound = 6,
  GenerationInUse = 7,
  TypeNotFound = 8,
  InstanceNotFound = 9,
  MemberNotFound = 10,
  InvalidValue = 11,
  ManagedException = 12,
  UnloadIncomplete = 13,
  BufferTooSmall = 14,
  Unsupported = 15,
};

enum class plCSharpLogLevel : plUInt32
{
  Debug = 0,
  Info = 1,
  Warning = 2,
  Error = 3,
  Dev = 4,
  Success = 5,
  SeriousWarning = 6,
};

enum class plCSharpValueKind : plUInt32
{
  Null,
  Boolean,
  Int64,
  UInt64,
  Double,
  Utf8String,
  ObjectHandle,
  ByteSpan,
};

enum class plCSharpValueFlags : plUInt32
{
  None = 0,
  ManagedOwned = PL_BIT(0),
  ObjectKindWorld = 1u << 8,
  ObjectKindGameObject = 2u << 8,
  ObjectKindComponent = 3u << 8,
  ObjectKindResource = 4u << 8,
  ObjectKindReflectedObject = 5u << 8,
};

struct plCSharpUtf8Span
{
  const char* m_pData = nullptr;
  plUInt32 m_uiLength = 0;
  plUInt32 m_uiReserved = 0;
};

struct plCSharpBuffer
{
  void* m_pData = nullptr;
  plUInt32 m_uiSize = 0;
  plUInt32 m_uiCapacity = 0;
  plUInt64 m_uiOwnerToken = 0;
};

struct plCSharpObjectHandle
{
  plUInt64 m_uiValue = 0;
  plUInt64 m_uiGeneration = 0;
  plUInt32 m_uiKind = 0;
  plUInt32 m_uiReserved = 0;
};

/// Fixed-layout value passed across the native/managed boundary.
/// For strings and byte spans, payload 0 is a pointer and payload 1 is the byte count.
struct plCSharpValue
{
  plCSharpValueKind m_Kind = plCSharpValueKind::Null;
  plCSharpValueFlags m_Flags = plCSharpValueFlags::None;
  plUInt64 m_uiPayload0 = 0;
  plUInt64 m_uiPayload1 = 0;
};

struct plCSharpGenerationLoadDesc
{
  plUInt32 m_uiSize = sizeof(plCSharpGenerationLoadDesc);
  plUInt32 m_uiVersion = 1;
  plUInt32 m_uiFlags = 0;
  plUInt32 m_uiReserved = 0;
  plCSharpUtf8Span m_AssemblyPath;
  plCSharpUtf8Span m_ShadowCopyRoot;
};

struct plCSharpInstanceCreateDesc
{
  plUInt32 m_uiSize = sizeof(plCSharpInstanceCreateDesc);
  plUInt32 m_uiVersion = 1;
  plUInt64 m_uiGeneration = 0;
  plUInt64 m_uiTypeId = 0;
  plCSharpObjectHandle m_Owner;
  plCSharpObjectHandle m_World;
  plCSharpObjectHandle m_OwnerComponent;
};

struct plCSharpUnloadReport
{
  plUInt32 m_uiSize = sizeof(plCSharpUnloadReport);
  plUInt32 m_uiVersion = 1;
  plUInt32 m_bLoadContextAlive = 0;
  plUInt32 m_uiGcCycles = 0;
  plUInt32 m_uiLiveInstances = 0;
  plUInt32 m_uiReserved = 0;
};

/// Native callbacks consumed by the permanent managed bootstrap and, later, generated engine bindings.
struct plCSharpNativeApiV1
{
  plUInt32 m_uiSize = sizeof(plCSharpNativeApiV1);
  plUInt32 m_uiVersion = PL_CSHARP_ABI_VERSION;
  plUInt32 m_uiPointerSize = sizeof(void*);
  plUInt32 m_uiValueLayoutVersion = PL_CSHARP_VALUE_LAYOUT_VERSION;

  plCSharpStatus(PL_CSHARP_CALL* m_Log)(plCSharpLogLevel level, plCSharpUtf8Span message) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_ValidateObject)(plCSharpObjectHandle object, plUInt32 uiExpectedKind) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_InvokeReflected)(plUInt64 uiFunctionId, plCSharpObjectHandle target,
    const plCSharpValue* pArguments, plUInt32 uiArgumentCount, plCSharpValue* out_pResult) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_ReleaseNativeValue)(plCSharpValue* pValue) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_QueryExtension)(plCSharpUtf8Span name, plUInt32 uiMinimumVersion, void** out_ppApi) = nullptr;

  /// M0-only round-trip probe. Kept at the tail so production consumers can ignore it by table size.
  plCSharpStatus(PL_CSHARP_CALL* m_M0Probe)(plInt64 iInput, plInt64* out_pOutput) = nullptr;
};

enum class plCSharpMessageRouting : plUInt32
{
  Direct,
  Recursive,
  Event,
};

/// Optional world/message operations kept behind QueryExtension so the stable
/// host ABI does not grow whenever a gameplay convenience is added.
struct plCSharpWorldApiV1
{
  plUInt32 m_uiSize = sizeof(plCSharpWorldApiV1);
  plUInt32 m_uiVersion = 2;

  plCSharpStatus(PL_CSHARP_CALL* m_SendMessage)(
    plCSharpObjectHandle target,
    plCSharpObjectHandle senderComponent,
    plCSharpUtf8Span nativeTypeName,
    plCSharpUtf8Span payloadJson,
    plCSharpMessageRouting routing) = nullptr;

  /// Routes an object stored in the synchronous managed-message registry.
  plCSharpStatus(PL_CSHARP_CALL* m_SendManagedMessage)(
    plCSharpObjectHandle target,
    plCSharpObjectHandle senderComponent,
    plUInt64 uiMessageId,
    plUInt64 uiPayloadToken,
    plCSharpMessageRouting routing) = nullptr;
};

/// Managed operations exposed to the native script-resource backend.
struct plCSharpManagedApiV1
{
  plUInt32 m_uiSize = sizeof(plCSharpManagedApiV1);
  plUInt32 m_uiVersion = PL_CSHARP_ABI_VERSION;
  plUInt32 m_uiPointerSize = sizeof(void*);
  plUInt32 m_uiValueLayoutVersion = PL_CSHARP_VALUE_LAYOUT_VERSION;

  plCSharpStatus(PL_CSHARP_CALL* m_Shutdown)() = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_LoadGeneration)(const plCSharpGenerationLoadDesc* pDesc, plUInt64* out_pGeneration) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_GetGenerationDescriptor)(plUInt64 uiGeneration, plCSharpBuffer* out_pDescriptor) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_CreateInstance)(const plCSharpInstanceCreateDesc* pDesc, plUInt64* out_pInstance) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DestroyInstance)(plUInt64 uiInstance) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_InvokeMethod)(plUInt64 uiInstance, plUInt64 uiMethodId,
    const plCSharpValue* pArguments, plUInt32 uiArgumentCount, plCSharpValue* out_pResult) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_GetField)(plUInt64 uiInstance, plUInt64 uiFieldId, plCSharpValue* out_pValue) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_SetField)(plUInt64 uiInstance, plUInt64 uiFieldId, const plCSharpValue* pValue) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DispatchMessage)(plUInt64 uiInstance, plUInt64 uiMessageHandlerId,
    const plCSharpValue* pPayload) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_UnloadGeneration)(plUInt64 uiGeneration, plCSharpUnloadReport* out_pReport) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_GetLastError)(plCSharpBuffer* out_pError) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_FreeBuffer)(plCSharpBuffer* pBuffer) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_ReleaseValue)(plCSharpValue* pValue) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_QueryExtension)(plCSharpUtf8Span name, plUInt32 uiMinimumVersion, void** out_ppApi) = nullptr;

  /// M0-only native callback probe. Kept at the tail so production consumers can ignore it by table size.
  plCSharpStatus(PL_CSHARP_CALL* m_RunM0Probe)(plInt64 iInput, plInt64* out_pOutput) = nullptr;
};

/// Size of the production ABI prefixes. Fields after these offsets are
/// optional test or future-extension tails and must be gated by m_uiSize.
constexpr plUInt32 PL_CSHARP_NATIVE_API_V1_REQUIRED_SIZE =
  static_cast<plUInt32>(offsetof(plCSharpNativeApiV1, m_M0Probe));
constexpr plUInt32 PL_CSHARP_MANAGED_API_V1_REQUIRED_SIZE =
  static_cast<plUInt32>(offsetof(plCSharpManagedApiV1, m_RunM0Probe));

using plCSharpManagedInitializeFn =
  plCSharpStatus(PL_CSHARP_CALL*)(const plCSharpNativeApiV1* pNativeApi, plCSharpManagedApiV1* out_pManagedApi);

static_assert(sizeof(plCSharpUtf8Span) == 16);
static_assert(sizeof(plCSharpBuffer) == 24);
static_assert(sizeof(plCSharpObjectHandle) == 24);
static_assert(sizeof(plCSharpValue) == 24);
static_assert(sizeof(plCSharpGenerationLoadDesc) == 48);
static_assert(sizeof(plCSharpInstanceCreateDesc) == 96);
