using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Plasma;

/// <summary>
/// The native "Plasma.Debug" extension table. Infrastructure for <see cref="DebugDraw"/>, not an
/// authoring API.
/// </summary>
[EditorBrowsable(EditorBrowsableState.Never)]
[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal unsafe struct DebugApi
{
    private const uint RequiredVersion = 1;

    public uint Size;
    public uint Version;

    public delegate* unmanaged[Cdecl]<DebugLine*, uint, Color, Transform*, DebugLineMode, NativeCallStatus> DrawLines;
    public delegate* unmanaged[Cdecl]<DebugLine*, uint, Color, Transform*, double, NativeCallStatus> AddPersistentLines;
    public delegate* unmanaged[Cdecl]<DebugTriangle*, uint, Color, NativeCallStatus> DrawTriangles;

    public delegate* unmanaged[Cdecl]<Vec3, float, Color, Transform*, NativeCallStatus> DrawCross;
    public delegate* unmanaged[Cdecl]<Vec3, Vec3, Color, Transform*, DebugBoxStyle, float, NativeCallStatus> DrawBox;
    public delegate* unmanaged[Cdecl]<Vec3, float, Color, Transform*, NativeCallStatus> DrawSphere;
    public delegate* unmanaged[Cdecl]<float, float, Color, Transform*, NativeCallStatus> DrawCapsuleZ;
    public delegate* unmanaged[Cdecl]<float, float, Color, Transform*, NativeCallStatus> DrawCylinderZ;
    public delegate* unmanaged[Cdecl]<Transform*, float, float, float, float, Color, uint, NativeCallStatus> DrawFrustum;
    public delegate* unmanaged[Cdecl]<float, float, float, Color, Color, Transform*, uint, uint, uint, NativeCallStatus> DrawCylinder;
    public delegate* unmanaged[Cdecl]<float, Color, Transform*, Vec3, NativeCallStatus> DrawArrow;
    public delegate* unmanaged[Cdecl]<float, float, Color, Color, Transform*, Vec3, Vec3, NativeCallStatus> DrawAngle;
    public delegate* unmanaged[Cdecl]<float, Color, Color, Transform*, Vec3, NativeCallStatus> DrawOpeningCone;
    public delegate* unmanaged[Cdecl]<float, float, Color, Color, Transform*, NativeCallStatus> DrawLimitCone;
    public delegate* unmanaged[Cdecl]<float, float, float, float, float, Color, uint, NativeCallStatus> DrawRectangle2D;

    public delegate* unmanaged[Cdecl]<Utf8Span, int, int, uint, uint, uint, Color, uint*, NativeCallStatus> DrawText2D;
    public delegate* unmanaged[Cdecl]<Utf8Span, Vec3, uint, uint, uint, uint, Color, uint*, NativeCallStatus> DrawText3D;
    public delegate* unmanaged[Cdecl]<Utf8Span, Transform*, float, uint, uint, Color, uint*, NativeCallStatus> DrawText3DInWorld;
    public delegate* unmanaged[Cdecl]<uint, Utf8Span, Utf8Span, Color, NativeCallStatus> DrawInfoText;
    public delegate* unmanaged[Cdecl]<uint, Utf8Span, double, Color, NativeCallStatus> AddPersistentInfoText;

    public delegate* unmanaged[Cdecl]<float, Color, Transform*, double, NativeCallStatus> AddPersistentCross;
    public delegate* unmanaged[Cdecl]<float, Color, Transform*, double, NativeCallStatus> AddPersistentSphere;
    public delegate* unmanaged[Cdecl]<Vec3, Color, Transform*, double, NativeCallStatus> AddPersistentBox;

    public delegate* unmanaged[Cdecl]<uint, float*, float*, float*, NativeCallStatus> GetTextMetrics;
    public delegate* unmanaged[Cdecl]<float, NativeCallStatus> SetTextScale;
    public delegate* unmanaged[Cdecl]<Vec2*, NativeCallStatus> GetResolution;

    private static DebugApi s_api;
    private static bool s_resolved;
    private static readonly object s_lock = new();

    /// <summary>
    /// Returns the resolved table, or throws when the host does not provide one. Debug drawing that
    /// silently does nothing is worse than an exception, because the usual reason to call it is that
    /// something else is already wrong.
    /// </summary>
    internal static DebugApi Require()
    {
        lock (s_lock)
        {
            if (!s_resolved)
            {
                s_resolved = true;
                s_api = NativeBridge.QueryExtension<DebugApi>("Plasma.Debug", RequiredVersion) is { } api &&
                        api.Size >= (uint)sizeof(DebugApi) && api.Version >= RequiredVersion
                    ? api
                    : default;
            }

            if (s_api.DrawLines is null)
            {
                throw new PlasmaException(
                    "Debug rendering is not available in this host. It requires the plCSharpPlugin runtime, " +
                    "which the editor and the player both load, and a script call in progress.");
            }

            return s_api;
        }
    }

    /// <summary>Drops the cached table when the host is reconfigured.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    internal static void Reset()
    {
        lock (s_lock)
        {
            s_resolved = false;
            s_api = default;
        }
    }
}
