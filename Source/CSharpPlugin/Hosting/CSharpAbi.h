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

// Debug-renderer value types. These mirror plVec3/plColor/plTransform and plDebugRenderer's Line and
// Triangle exactly, so arrays cross the boundary as raw memory and are reinterpreted rather than
// converted. CSharpDebugApi.cpp static_asserts every one of these against the engine type it mirrors.

struct plCSharpDebugVec2
{
  float m_fX = 0.0f;
  float m_fY = 0.0f;
};

struct plCSharpDebugVec3
{
  float m_fX = 0.0f;
  float m_fY = 0.0f;
  float m_fZ = 0.0f;
};

struct plCSharpDebugColor
{
  float m_fR = 0.0f;
  float m_fG = 0.0f;
  float m_fB = 0.0f;
  float m_fA = 1.0f;
};

struct plCSharpDebugTransform
{
  plCSharpDebugVec3 m_Position;
  float m_Rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  plCSharpDebugVec3 m_Scale = {1.0f, 1.0f, 1.0f};
};

struct plCSharpDebugLine
{
  plCSharpDebugVec3 m_Start;
  plCSharpDebugVec3 m_End;
  plCSharpDebugColor m_StartColor;
  plCSharpDebugColor m_EndColor;
};

struct plCSharpDebugTriangle
{
  plCSharpDebugVec3 m_Positions[3];
  plCSharpDebugColor m_Color;
};

enum class plCSharpDebugLineMode : plUInt32
{
  World = 0,
  /// Always on top of scene geometry, with distance fade-out.
  Occluded = 1,
  /// Screen space; coordinates are pixels.
  Screen2D = 2,
};

enum class plCSharpDebugBoxStyle : plUInt32
{
  Lines = 0,
  Solid = 1,
  Corners = 2,
};

/// Direct debug-rendering entry points, kept behind QueryExtension like plCSharpWorldApiV1.
///
/// These exist next to the reflected plScriptExtensionClass_Debug bindings rather than replacing
/// them: the reflected path boxes every argument and allocates per call, which is the wrong shape for
/// something invoked once per drawn line. The world is taken from the active script execution scope,
/// so it is never passed across the boundary.
struct plCSharpDebugApiV1
{
  plUInt32 m_uiSize = sizeof(plCSharpDebugApiV1);
  plUInt32 m_uiVersion = 1;

  plCSharpStatus(PL_CSHARP_CALL* m_DrawLines)(const plCSharpDebugLine* pLines, plUInt32 uiCount,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform, plCSharpDebugLineMode mode) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_AddPersistentLines)(const plCSharpDebugLine* pLines, plUInt32 uiCount,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform, double fDurationSeconds) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawTriangles)(const plCSharpDebugTriangle* pTriangles, plUInt32 uiCount,
    plCSharpDebugColor color) = nullptr;

  plCSharpStatus(PL_CSHARP_CALL* m_DrawCross)(plCSharpDebugVec3 vPosition, float fLineLength,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawBox)(plCSharpDebugVec3 vCenter, plCSharpDebugVec3 vHalfExtents,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform, plCSharpDebugBoxStyle style,
    float fCornerFraction) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawSphere)(plCSharpDebugVec3 vCenter, float fRadius,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawCapsuleZ)(float fLength, float fRadius,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawCylinderZ)(float fLength, float fRadius,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawFrustum)(const plCSharpDebugTransform* pTransform,
    float fFovXDegrees, float fFovYDegrees, float fNear, float fFar, plCSharpDebugColor color,
    plUInt32 bDrawPlaneNormals) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawCylinder)(float fRadiusStart, float fRadiusEnd, float fLength,
    plCSharpDebugColor solidColor, plCSharpDebugColor lineColor, const plCSharpDebugTransform* pTransform,
    plUInt32 bCapStart, plUInt32 bCapEnd, plUInt32 uiCylinderAxis) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawArrow)(float fSize, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, plCSharpDebugVec3 vForwardAxis) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawAngle)(float fStartDegrees, float fEndDegrees,
    plCSharpDebugColor solidColor, plCSharpDebugColor lineColor, const plCSharpDebugTransform* pTransform,
    plCSharpDebugVec3 vForwardAxis, plCSharpDebugVec3 vRotationAxis) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawOpeningCone)(float fHalfAngleDegrees, plCSharpDebugColor colorInside,
    plCSharpDebugColor colorOutside, const plCSharpDebugTransform* pTransform, plCSharpDebugVec3 vForwardAxis) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawLimitCone)(float fHalfAngle1Degrees, float fHalfAngle2Degrees,
    plCSharpDebugColor solidColor, plCSharpDebugColor lineColor, const plCSharpDebugTransform* pTransform) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawRectangle2D)(float fX, float fY, float fWidth, float fHeight,
    float fDepth, plCSharpDebugColor color, plUInt32 bLinesOnly) = nullptr;

  plCSharpStatus(PL_CSHARP_CALL* m_DrawText2D)(plCSharpUtf8Span text, plInt32 iPositionX, plInt32 iPositionY,
    plUInt32 uiSizeInPixel, plUInt32 uiHorizontalAlignment, plUInt32 uiVerticalAlignment,
    plCSharpDebugColor color, plUInt32* out_pLineCount) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawText3D)(plCSharpUtf8Span text, plCSharpDebugVec3 vPosition,
    plUInt32 uiSizeInPixel, plUInt32 uiHorizontalAlignment, plUInt32 uiVerticalAlignment,
    plUInt32 bDepthTest, plCSharpDebugColor color, plUInt32* out_pLineCount) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawText3DInWorld)(plCSharpUtf8Span text,
    const plCSharpDebugTransform* pTransform, float fGlyphSize, plUInt32 uiHorizontalAlignment,
    plUInt32 uiVerticalAlignment, plCSharpDebugColor color, plUInt32* out_pLineCount) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_DrawInfoText)(plUInt32 uiPlacement, plCSharpUtf8Span groupName,
    plCSharpUtf8Span text, plCSharpDebugColor color) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_AddPersistentInfoText)(plUInt32 uiPlacement, plCSharpUtf8Span text,
    double fDurationSeconds, plCSharpDebugColor color) = nullptr;

  plCSharpStatus(PL_CSHARP_CALL* m_AddPersistentCross)(float fSize, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, double fDurationSeconds) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_AddPersistentSphere)(float fRadius, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, double fDurationSeconds) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_AddPersistentBox)(plCSharpDebugVec3 vHalfExtents, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, double fDurationSeconds) = nullptr;

  plCSharpStatus(PL_CSHARP_CALL* m_GetTextMetrics)(plUInt32 uiSizeInPixel, float* out_pGlyphWidth,
    float* out_pLineHeight, float* out_pTextScale) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_SetTextScale)(float fScale) = nullptr;
  plCSharpStatus(PL_CSHARP_CALL* m_GetResolution)(plCSharpDebugVec2* out_pResolution) = nullptr;
};

/// Console output exposed to managed code, behind the native QueryExtension.
struct plCSharpConsoleApiV1
{
  plUInt32 m_uiSize = sizeof(plCSharpConsoleApiV1);
  plUInt32 m_uiVersion = 1;

  /// \a uiLineType is a plConsoleString::Type.
  plCSharpStatus(PL_CSHARP_CALL* m_Print)(plUInt32 uiLineType, plCSharpUtf8Span text) = nullptr;
};

/// Managed console operations, behind the managed QueryExtension.
struct plCSharpManagedConsoleApiV1
{
  plUInt32 m_uiSize = sizeof(plCSharpManagedConsoleApiV1);
  plUInt32 m_uiVersion = 1;

  plCSharpStatus(PL_CSHARP_CALL* m_InvokeCommand)(plUInt64 uiGeneration, plUInt64 uiCommandId,
    const plCSharpValue* pArguments, plUInt32 uiArgumentCount) = nullptr;

  /// Draws one console tool. Called from inside the console's ImGui frame, on the main thread.
  plCSharpStatus(PL_CSHARP_CALL* m_DrawTool)(plUInt64 uiGeneration, plUInt64 uiToolId) = nullptr;
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
static_assert(sizeof(plCSharpDebugVec2) == 8);
static_assert(sizeof(plCSharpDebugVec3) == 12);
static_assert(sizeof(plCSharpDebugColor) == 16);
static_assert(sizeof(plCSharpDebugTransform) == 40);
static_assert(sizeof(plCSharpDebugLine) == 56);
static_assert(sizeof(plCSharpDebugTriangle) == 52);
