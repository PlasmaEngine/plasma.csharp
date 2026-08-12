#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Runtime/CSharpDebugApi.h>
#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>

#include <Core/World/World.h>
#include <Foundation/Math/Frustum.h>
#include <Foundation/Math/Rect.h>
#include <RendererCore/Debug/DebugRenderer.h>

// The managed structs are reinterpreted rather than converted, so their layout has to match the
// engine types exactly. A mismatch here is silent memory corruption at the boundary, not a compile
// error, which is why every one of them is pinned.
static_assert(sizeof(plCSharpDebugVec2) == sizeof(plVec2));
static_assert(sizeof(plCSharpDebugVec3) == sizeof(plVec3));
static_assert(sizeof(plCSharpDebugColor) == sizeof(plColor));
static_assert(sizeof(plCSharpDebugTransform) == sizeof(plTransform));
static_assert(sizeof(plCSharpDebugLine) == sizeof(plDebugRenderer::Line));
static_assert(sizeof(plCSharpDebugTriangle) == sizeof(plDebugRenderer::Triangle));
static_assert(offsetof(plCSharpDebugLine, m_StartColor) == offsetof(plDebugRenderer::Line, m_startColor));
static_assert(offsetof(plCSharpDebugTriangle, m_Color) == offsetof(plDebugRenderer::Triangle, m_color));

namespace
{
  PL_ALWAYS_INLINE const plVec3& ToVec3(const plCSharpDebugVec3& value)
  {
    return *reinterpret_cast<const plVec3*>(&value);
  }

  PL_ALWAYS_INLINE const plColor& ToColor(const plCSharpDebugColor& value)
  {
    return *reinterpret_cast<const plColor*>(&value);
  }

  /// A null transform means identity, which is what every plDebugRenderer overload defaults to.
  PL_ALWAYS_INLINE plTransform ToTransform(const plCSharpDebugTransform* pValue)
  {
    if (pValue == nullptr)
      return plTransform::MakeIdentity();

    return *reinterpret_cast<const plTransform*>(pValue);
  }

  PL_ALWAYS_INLINE plStringView ToStringView(plCSharpUtf8Span span)
  {
    if (span.m_pData == nullptr || span.m_uiLength == 0)
      return plStringView();

    return plStringView(span.m_pData, span.m_uiLength);
  }

  template <typename T>
  PL_ALWAYS_INLINE plEnum<T> ToEnum(plUInt32 uiValue, typename T::Enum fallback)
  {
    return (uiValue <= static_cast<plUInt32>(T::Default) || uiValue < 16u)
             ? static_cast<typename T::Enum>(uiValue)
             : fallback;
  }

  /// Resolves the world every draw goes into. Debug geometry is scoped to a world, and the only
  /// world a script legitimately draws into is the one it is executing in.
  plResult GetContext(plDebugRendererContext& out_context)
  {
    plWorld* pWorld = plCSharpExecutionScope::GetCurrentWorld();
    if (pWorld == nullptr)
      return PL_FAILURE;

    out_context = plDebugRendererContext(pWorld);
    return PL_SUCCESS;
  }

#define PL_CSHARP_DEBUG_CONTEXT()             \
  plDebugRendererContext context;             \
  if (GetContext(context).Failed())           \
    return plCSharpStatus::NotInitialized;

  plCSharpStatus PL_CSHARP_CALL DrawLines(const plCSharpDebugLine* pLines, plUInt32 uiCount,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform, plCSharpDebugLineMode mode)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    if (uiCount == 0)
      return plCSharpStatus::Success;
    if (pLines == nullptr)
      return plCSharpStatus::InvalidArgument;

    const plArrayPtr<const plDebugRenderer::Line> lines(
      reinterpret_cast<const plDebugRenderer::Line*>(pLines), uiCount);

    switch (mode)
    {
      case plCSharpDebugLineMode::Occluded:
        plDebugRenderer::DrawLinesOccluded(context, lines, ToColor(color), ToTransform(pTransform));
        break;
      case plCSharpDebugLineMode::Screen2D:
        plDebugRenderer::Draw2DLines(context, lines, ToColor(color));
        break;
      default:
        plDebugRenderer::DrawLines(context, lines, ToColor(color), ToTransform(pTransform));
        break;
    }

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL AddPersistentLines(const plCSharpDebugLine* pLines, plUInt32 uiCount,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform, double fDurationSeconds)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    if (uiCount == 0)
      return plCSharpStatus::Success;
    if (pLines == nullptr)
      return plCSharpStatus::InvalidArgument;

    plDebugRenderer::AddPersistentLines(context,
      plArrayPtr<const plDebugRenderer::Line>(reinterpret_cast<const plDebugRenderer::Line*>(pLines), uiCount),
      ToColor(color), ToTransform(pTransform), plTime::MakeFromSeconds(fDurationSeconds));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawTriangles(
    const plCSharpDebugTriangle* pTriangles, plUInt32 uiCount, plCSharpDebugColor color)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    if (uiCount == 0)
      return plCSharpStatus::Success;
    if (pTriangles == nullptr)
      return plCSharpStatus::InvalidArgument;

    // DrawSolidTriangles takes a mutable array; the renderer transforms the vertices in place before
    // batching them, so the managed buffer is copied rather than handed over.
    plHybridArray<plDebugRenderer::Triangle, 64> triangles;
    triangles.SetCountUninitialized(uiCount);
    plMemoryUtils::RawByteCopy(triangles.GetData(), pTriangles, uiCount * sizeof(plCSharpDebugTriangle));

    plDebugRenderer::DrawSolidTriangles(context, triangles, ToColor(color));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawCross(plCSharpDebugVec3 vPosition, float fLineLength,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawCross(context, ToVec3(vPosition), fLineLength, ToColor(color), ToTransform(pTransform));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawBox(plCSharpDebugVec3 vCenter, plCSharpDebugVec3 vHalfExtents,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform, plCSharpDebugBoxStyle style,
    float fCornerFraction)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(ToVec3(vCenter), ToVec3(vHalfExtents));
    const plTransform transform = ToTransform(pTransform);

    switch (style)
    {
      case plCSharpDebugBoxStyle::Solid:
        plDebugRenderer::DrawSolidBox(context, box, ToColor(color), transform);
        break;
      case plCSharpDebugBoxStyle::Corners:
        plDebugRenderer::DrawLineBoxCorners(context, box, fCornerFraction, ToColor(color), transform);
        break;
      default:
        plDebugRenderer::DrawLineBox(context, box, ToColor(color), transform);
        break;
    }

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawSphere(plCSharpDebugVec3 vCenter, float fRadius,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawLineSphere(context,
      plBoundingSphere::MakeFromCenterAndRadius(ToVec3(vCenter), fRadius), ToColor(color), ToTransform(pTransform));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawCapsuleZ(float fLength, float fRadius,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawLineCapsuleZ(context, fLength, fRadius, ToColor(color), ToTransform(pTransform));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawCylinderZ(float fLength, float fRadius,
    plCSharpDebugColor color, const plCSharpDebugTransform* pTransform)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawLineCylinderZ(context, fLength, fRadius, ToColor(color), ToTransform(pTransform));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawFrustum(const plCSharpDebugTransform* pTransform,
    float fFovXDegrees, float fFovYDegrees, float fNear, float fFar, plCSharpDebugColor color,
    plUInt32 bDrawPlaneNormals)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plTransform transform = ToTransform(pTransform);
    const plFrustum frustum = plFrustum::MakeFromFOV(transform.m_vPosition,
      transform.m_qRotation * plVec3::MakeAxisX(), transform.m_qRotation * plVec3::MakeAxisZ(),
      plAngle::MakeFromDegree(fFovXDegrees), plAngle::MakeFromDegree(fFovYDegrees), fNear, fFar);

    plDebugRenderer::DrawLineFrustum(context, frustum, ToColor(color), bDrawPlaneNormals != 0);
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawCylinder(float fRadiusStart, float fRadiusEnd, float fLength,
    plCSharpDebugColor solidColor, plCSharpDebugColor lineColor, const plCSharpDebugTransform* pTransform,
    plUInt32 bCapStart, plUInt32 bCapEnd, plUInt32 uiCylinderAxis)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plBasisAxis::Enum axis = (uiCylinderAxis <= static_cast<plUInt32>(plBasisAxis::NegativeZ))
                                     ? static_cast<plBasisAxis::Enum>(uiCylinderAxis)
                                     : plBasisAxis::PositiveX;

    plDebugRenderer::DrawCylinder(context, fRadiusStart, fRadiusEnd, fLength, ToColor(solidColor),
      ToColor(lineColor), ToTransform(pTransform), bCapStart != 0, bCapEnd != 0, axis);
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawArrow(float fSize, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, plCSharpDebugVec3 vForwardAxis)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawArrow(context, fSize, ToColor(color), ToTransform(pTransform), ToVec3(vForwardAxis));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawAngle(float fStartDegrees, float fEndDegrees,
    plCSharpDebugColor solidColor, plCSharpDebugColor lineColor, const plCSharpDebugTransform* pTransform,
    plCSharpDebugVec3 vForwardAxis, plCSharpDebugVec3 vRotationAxis)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawAngle(context, plAngle::MakeFromDegree(fStartDegrees), plAngle::MakeFromDegree(fEndDegrees),
      ToColor(solidColor), ToColor(lineColor), ToTransform(pTransform), ToVec3(vForwardAxis), ToVec3(vRotationAxis));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawOpeningCone(float fHalfAngleDegrees, plCSharpDebugColor colorInside,
    plCSharpDebugColor colorOutside, const plCSharpDebugTransform* pTransform, plCSharpDebugVec3 vForwardAxis)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawOpeningCone(context, plAngle::MakeFromDegree(fHalfAngleDegrees), ToColor(colorInside),
      ToColor(colorOutside), ToTransform(pTransform), ToVec3(vForwardAxis));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawLimitCone(float fHalfAngle1Degrees, float fHalfAngle2Degrees,
    plCSharpDebugColor solidColor, plCSharpDebugColor lineColor, const plCSharpDebugTransform* pTransform)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawLimitCone(context, plAngle::MakeFromDegree(fHalfAngle1Degrees),
      plAngle::MakeFromDegree(fHalfAngle2Degrees), ToColor(solidColor), ToColor(lineColor), ToTransform(pTransform));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawRectangle2D(float fX, float fY, float fWidth, float fHeight,
    float fDepth, plCSharpDebugColor color, plUInt32 bLinesOnly)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plRectFloat rect(fX, fY, fWidth, fHeight);
    if (bLinesOnly != 0)
      plDebugRenderer::Draw2DLineRectangle(context, rect, fDepth, ToColor(color));
    else
      plDebugRenderer::Draw2DRectangle(context, rect, fDepth, ToColor(color));

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawText2D(plCSharpUtf8Span text, plInt32 iPositionX, plInt32 iPositionY,
    plUInt32 uiSizeInPixel, plUInt32 uiHorizontalAlignment, plUInt32 uiVerticalAlignment,
    plCSharpDebugColor color, plUInt32* out_pLineCount)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plUInt32 uiLines = plDebugRenderer::Draw2DText(context, ToStringView(text),
      plVec2I32(iPositionX, iPositionY), ToColor(color), uiSizeInPixel,
      ToEnum<plDebugTextHAlign>(uiHorizontalAlignment, plDebugTextHAlign::Left),
      ToEnum<plDebugTextVAlign>(uiVerticalAlignment, plDebugTextVAlign::Top));

    if (out_pLineCount != nullptr)
      *out_pLineCount = uiLines;

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawText3D(plCSharpUtf8Span text, plCSharpDebugVec3 vPosition,
    plUInt32 uiSizeInPixel, plUInt32 uiHorizontalAlignment, plUInt32 uiVerticalAlignment,
    plUInt32 bDepthTest, plCSharpDebugColor color, plUInt32* out_pLineCount)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plUInt32 uiLines = plDebugRenderer::Draw3DText(context, ToStringView(text), ToVec3(vPosition),
      ToColor(color), uiSizeInPixel,
      ToEnum<plDebugTextHAlign>(uiHorizontalAlignment, plDebugTextHAlign::Center),
      ToEnum<plDebugTextVAlign>(uiVerticalAlignment, plDebugTextVAlign::Bottom), bDepthTest != 0);

    if (out_pLineCount != nullptr)
      *out_pLineCount = uiLines;

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawText3DInWorld(plCSharpUtf8Span text,
    const plCSharpDebugTransform* pTransform, float fGlyphSize, plUInt32 uiHorizontalAlignment,
    plUInt32 uiVerticalAlignment, plCSharpDebugColor color, plUInt32* out_pLineCount)
  {
    PL_CSHARP_DEBUG_CONTEXT();

    const plUInt32 uiLines = plDebugRenderer::Draw3DTextInWorld(context, ToStringView(text),
      ToTransform(pTransform), fGlyphSize, ToColor(color),
      ToEnum<plDebugTextHAlign>(uiHorizontalAlignment, plDebugTextHAlign::Center),
      ToEnum<plDebugTextVAlign>(uiVerticalAlignment, plDebugTextVAlign::Bottom));

    if (out_pLineCount != nullptr)
      *out_pLineCount = uiLines;

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL DrawInfoText(plUInt32 uiPlacement, plCSharpUtf8Span groupName,
    plCSharpUtf8Span text, plCSharpDebugColor color)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::DrawInfoText(context,
      ToEnum<plDebugTextPlacement>(uiPlacement, plDebugTextPlacement::TopLeft),
      ToStringView(groupName), ToStringView(text), ToColor(color));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL AddPersistentInfoText(plUInt32 uiPlacement, plCSharpUtf8Span text,
    double fDurationSeconds, plCSharpDebugColor color)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::AddPersistentInfoText(context,
      ToEnum<plDebugTextPlacement>(uiPlacement, plDebugTextPlacement::TopLeft),
      ToStringView(text), plTime::MakeFromSeconds(fDurationSeconds), ToColor(color));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL AddPersistentCross(float fSize, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, double fDurationSeconds)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::AddPersistentCross(
      context, fSize, ToColor(color), ToTransform(pTransform), plTime::MakeFromSeconds(fDurationSeconds));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL AddPersistentSphere(float fRadius, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, double fDurationSeconds)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::AddPersistentLineSphere(
      context, fRadius, ToColor(color), ToTransform(pTransform), plTime::MakeFromSeconds(fDurationSeconds));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL AddPersistentBox(plCSharpDebugVec3 vHalfExtents, plCSharpDebugColor color,
    const plCSharpDebugTransform* pTransform, double fDurationSeconds)
  {
    PL_CSHARP_DEBUG_CONTEXT();
    plDebugRenderer::AddPersistentLineBox(context, ToVec3(vHalfExtents), ToColor(color),
      ToTransform(pTransform), plTime::MakeFromSeconds(fDurationSeconds));
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL GetTextMetrics(
    plUInt32 uiSizeInPixel, float* out_pGlyphWidth, float* out_pLineHeight, float* out_pTextScale)
  {
    if (out_pGlyphWidth != nullptr)
      *out_pGlyphWidth = plDebugRenderer::GetTextGlyphWidth(uiSizeInPixel);
    if (out_pLineHeight != nullptr)
      *out_pLineHeight = plDebugRenderer::GetTextLineHeight(uiSizeInPixel);
    if (out_pTextScale != nullptr)
      *out_pTextScale = plDebugRenderer::GetTextScale();

    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL SetTextScale(float fScale)
  {
    plDebugRenderer::SetTextScale(fScale);
    return plCSharpStatus::Success;
  }

  plCSharpStatus PL_CSHARP_CALL GetResolution(plCSharpDebugVec2* out_pResolution)
  {
    if (out_pResolution == nullptr)
      return plCSharpStatus::InvalidArgument;

    const plVec2 resolution = plScriptExtensionClass_Debug::GetResolution();
    out_pResolution->m_fX = resolution.x;
    out_pResolution->m_fY = resolution.y;
    return plCSharpStatus::Success;
  }

#undef PL_CSHARP_DEBUG_CONTEXT

  plCSharpDebugApiV1 BuildApi()
  {
    plCSharpDebugApiV1 api;
    api.m_DrawLines = &DrawLines;
    api.m_AddPersistentLines = &AddPersistentLines;
    api.m_DrawTriangles = &DrawTriangles;
    api.m_DrawCross = &DrawCross;
    api.m_DrawBox = &DrawBox;
    api.m_DrawSphere = &DrawSphere;
    api.m_DrawCapsuleZ = &DrawCapsuleZ;
    api.m_DrawCylinderZ = &DrawCylinderZ;
    api.m_DrawFrustum = &DrawFrustum;
    api.m_DrawCylinder = &DrawCylinder;
    api.m_DrawArrow = &DrawArrow;
    api.m_DrawAngle = &DrawAngle;
    api.m_DrawOpeningCone = &DrawOpeningCone;
    api.m_DrawLimitCone = &DrawLimitCone;
    api.m_DrawRectangle2D = &DrawRectangle2D;
    api.m_DrawText2D = &DrawText2D;
    api.m_DrawText3D = &DrawText3D;
    api.m_DrawText3DInWorld = &DrawText3DInWorld;
    api.m_DrawInfoText = &DrawInfoText;
    api.m_AddPersistentInfoText = &AddPersistentInfoText;
    api.m_AddPersistentCross = &AddPersistentCross;
    api.m_AddPersistentSphere = &AddPersistentSphere;
    api.m_AddPersistentBox = &AddPersistentBox;
    api.m_GetTextMetrics = &GetTextMetrics;
    api.m_SetTextScale = &SetTextScale;
    api.m_GetResolution = &GetResolution;
    return api;
  }
} // namespace

const plCSharpDebugApiV1* plCSharpDebugApi::GetApi()
{
  static const plCSharpDebugApiV1 s_Api = BuildApi();
  return &s_Api;
}
