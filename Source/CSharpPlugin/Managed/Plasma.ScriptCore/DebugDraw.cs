using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;

namespace Plasma;

/// <summary>Where a debug line is drawn.</summary>
public enum DebugLineMode : uint
{
    /// <summary>In the world, depth tested.</summary>
    World = 0,
    /// <summary>On top of all geometry, with distance fade-out.</summary>
    Occluded = 1,
    /// <summary>In screen space; coordinates are pixels.</summary>
    Screen2D = 2,
}

public enum DebugBoxStyle : uint
{
    Lines = 0,
    Solid = 1,
    /// <summary>Only the box corners, sized by a fraction of each edge.</summary>
    Corners = 2,
}

public enum TextHAlign : uint { Left = 0, Center = 1, Right = 2 }

public enum TextVAlign : uint { Top = 0, Center = 1, Bottom = 2 }

public enum TextPlacement : uint
{
    TopLeft = 0,
    TopCenter = 1,
    TopRight = 2,
    BottomLeft = 3,
    BottomCenter = 4,
    BottomRight = 5,
}

public enum DebugAxis : uint
{
    PositiveX = 0,
    PositiveY = 1,
    PositiveZ = 2,
    NegativeX = 3,
    NegativeY = 4,
    NegativeZ = 5,
}

/// <summary>One debug line. Layout matches <c>plDebugRenderer::Line</c>, so arrays cross to native
/// code as raw memory rather than being converted element by element.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct DebugLine
{
    public DebugLine(Vec3 start, Vec3 end, Color color)
    {
        Start = start;
        End = end;
        StartColor = color;
        EndColor = color;
    }

    public DebugLine(Vec3 start, Vec3 end, Color startColor, Color endColor)
    {
        Start = start;
        End = end;
        StartColor = startColor;
        EndColor = endColor;
    }

    public Vec3 Start;
    public Vec3 End;
    public Color StartColor;
    public Color EndColor;
}

/// <summary>One filled debug triangle. Layout matches <c>plDebugRenderer::Triangle</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct DebugTriangle
{
    public DebugTriangle(Vec3 a, Vec3 b, Vec3 c, Color color)
    {
        A = a;
        B = b;
        C = c;
        Color = color;
    }

    public Vec3 A;
    public Vec3 B;
    public Vec3 C;
    public Color Color;
}

/// <summary>
/// Debug rendering. Everything draws into the world of the script that is currently executing and
/// lasts one frame, except the <c>AddPersistent*</c> calls, which last for a given duration.
/// </summary>
/// <remarks>
/// <para>
/// These call the engine through a fixed function table: no boxing, no argument arrays, nothing
/// allocated per call. That matters because debug drawing is the API most likely to be called inside
/// a loop - and for real loops, <see cref="Lines(int)"/> submits a whole batch in one crossing.
/// </para>
/// <para>
/// This sits beside the generated <c>Debug</c> class rather than replacing it. <c>Debug</c> exposes
/// 13 of <c>plDebugRenderer</c>'s entry points through the reflected binding path, which allocates
/// roughly six times per call; <c>DebugDraw</c> covers the whole surface and allocates nothing.
/// Existing <c>Debug.*</c> calls keep working unchanged.
/// </para>
/// </remarks>
public static unsafe class DebugDraw
{
    /// <summary>UTF-8 bytes below this length are encoded on the stack.</summary>
    private const int StackTextBytes = 512;

    /// <summary>
    /// Accumulates lines and submits them in one native call when disposed. A ref struct on purpose:
    /// the batch is only valid inside the script call that created it, because the world it draws
    /// into is the one on the current execution scope.
    /// </summary>
    public ref struct LineBatch
    {
        private DebugLine[]? _buffer;
        private int _count;
        private readonly DebugLineMode _mode;
        private Transform _transform;

        internal LineBatch(int capacity, DebugLineMode mode, Transform transform)
        {
            _buffer = ArrayPool<DebugLine>.Shared.Rent(Math.Max(capacity, 16));
            _count = 0;
            _mode = mode;
            _transform = transform;
        }

        public readonly int Count => _count;

        public void Add(Vec3 start, Vec3 end, Color color) =>
            Add(new DebugLine(start, end, color));

        public void Add(Vec3 start, Vec3 end, Color startColor, Color endColor) =>
            Add(new DebugLine(start, end, startColor, endColor));

        public void Add(in DebugLine line)
        {
            DebugLine[] buffer = _buffer ?? throw new ObjectDisposedException(nameof(LineBatch));

            if (_count == buffer.Length)
            {
                // Submitting early rather than growing keeps the peak buffer bounded when a caller
                // underestimates its capacity, and the drawn result is identical either way.
                Flush();
            }

            _buffer![_count++] = line;
        }

        /// <summary>Submits what has accumulated so far and keeps the batch usable.</summary>
        public void Flush()
        {
            if (_buffer is null || _count == 0)
            {
                return;
            }

            Transform transform = _transform;
            fixed (DebugLine* lines = _buffer)
            {
                DebugApi.Require().DrawLines(lines, (uint)_count, Color.White, &transform, _mode);
            }

            _count = 0;
        }

        public void Dispose()
        {
            Flush();

            if (_buffer is not null)
            {
                ArrayPool<DebugLine>.Shared.Return(_buffer);
                _buffer = null;
            }
        }
    }

    /// <summary>Starts a batch of lines submitted together when it is disposed.</summary>
    public static LineBatch Lines(int capacity = 256) =>
        new(capacity, DebugLineMode.World, Transform.Identity);

    /// <summary>Starts a batch drawn in the given mode.</summary>
    public static LineBatch Lines(int capacity, DebugLineMode mode) =>
        new(capacity, mode, Transform.Identity);

    /// <summary>Starts a batch drawn in the given mode and local space.</summary>
    public static LineBatch Lines(int capacity, DebugLineMode mode, Transform transform) =>
        new(capacity, mode, transform);

    public static void DrawLines(
        ReadOnlySpan<DebugLine> lines,
        DebugLineMode mode = DebugLineMode.World)
    {
        if (lines.IsEmpty)
        {
            return;
        }

        fixed (DebugLine* data = lines)
        {
            DebugApi.Require().DrawLines(data, (uint)lines.Length, Color.White, null, mode);
        }
    }

    public static void DrawLines(
        ReadOnlySpan<DebugLine> lines,
        Transform transform,
        DebugLineMode mode = DebugLineMode.World)
    {
        if (lines.IsEmpty)
        {
            return;
        }

        fixed (DebugLine* data = lines)
        {
            DebugApi.Require().DrawLines(data, (uint)lines.Length, Color.White, &transform, mode);
        }
    }

    public static void AddPersistentLines(ReadOnlySpan<DebugLine> lines, Time duration)
    {
        if (lines.IsEmpty)
        {
            return;
        }

        fixed (DebugLine* data = lines)
        {
            DebugApi.Require().AddPersistentLines(
                data, (uint)lines.Length, Color.White, null, duration.Seconds);
        }
    }

    public static void DrawTriangles(ReadOnlySpan<DebugTriangle> triangles)
    {
        if (triangles.IsEmpty)
        {
            return;
        }

        fixed (DebugTriangle* data = triangles)
        {
            DebugApi.Require().DrawTriangles(data, (uint)triangles.Length, Color.White);
        }
    }

    public static void Line(Vec3 start, Vec3 end, Color color, DebugLineMode mode = DebugLineMode.World)
    {
        DebugLine line = new(start, end, color);
        DebugApi.Require().DrawLines(&line, 1, Color.White, null, mode);
    }

    public static void Line(Vec3 start, Vec3 end, Color startColor, Color endColor)
    {
        DebugLine line = new(start, end, startColor, endColor);
        DebugApi.Require().DrawLines(&line, 1, Color.White, null, DebugLineMode.World);
    }

    public static void Cross(Vec3 position, float size, Color color) =>
        DebugApi.Require().DrawCross(position, size, color, null);

    public static void Cross(Vec3 position, float size, Color color, Transform transform) =>
        DebugApi.Require().DrawCross(position, size, color, &transform);

    public static void Box(
        Vec3 center,
        Vec3 halfExtents,
        Color color,
        DebugBoxStyle style = DebugBoxStyle.Lines) =>
        DebugApi.Require().DrawBox(center, halfExtents, color, null, style, 0.25f);

    public static void Box(
        Vec3 center,
        Vec3 halfExtents,
        Color color,
        Transform transform,
        DebugBoxStyle style = DebugBoxStyle.Lines,
        float cornerFraction = 0.25f) =>
        DebugApi.Require().DrawBox(center, halfExtents, color, &transform, style, cornerFraction);

    public static void Sphere(Vec3 center, float radius, Color color) =>
        DebugApi.Require().DrawSphere(center, radius, color, null);

    public static void Sphere(Vec3 center, float radius, Color color, Transform transform) =>
        DebugApi.Require().DrawSphere(center, radius, color, &transform);

    /// <summary>An upright wireframe capsule along the transform's local Z axis.</summary>
    public static void Capsule(float length, float radius, Color color, Transform transform) =>
        DebugApi.Require().DrawCapsuleZ(length, radius, color, &transform);

    /// <summary>An upright wireframe cylinder along the transform's local Z axis.</summary>
    public static void CylinderZ(float length, float radius, Color color, Transform transform) =>
        DebugApi.Require().DrawCylinderZ(length, radius, color, &transform);

    /// <summary>A wireframe view frustum looking along the transform's local +X axis.</summary>
    public static void Frustum(
        Transform transform,
        float fovXDegrees,
        float fovYDegrees,
        float near,
        float far,
        Color color,
        bool drawPlaneNormals = false) =>
        DebugApi.Require().DrawFrustum(
            &transform, fovXDegrees, fovYDegrees, near, far, color, drawPlaneNormals ? 1u : 0u);

    /// <summary>
    /// A cylinder starting at the transform's origin. Different start and end radii give a cone or
    /// an arrow head.
    /// </summary>
    public static void Cylinder(
        float radiusStart,
        float radiusEnd,
        float length,
        Color solidColor,
        Color lineColor,
        Transform transform,
        bool capStart = false,
        bool capEnd = false,
        DebugAxis axis = DebugAxis.PositiveX) =>
        DebugApi.Require().DrawCylinder(radiusStart, radiusEnd, length, solidColor, lineColor,
            &transform, capStart ? 1u : 0u, capEnd ? 1u : 0u, (uint)axis);

    public static void Arrow(float size, Color color, Transform transform) =>
        Arrow(size, color, transform, Vec3.AxisX);

    public static void Arrow(float size, Color color, Transform transform, Vec3 forwardAxis) =>
        DebugApi.Require().DrawArrow(size, color, &transform, forwardAxis);

    /// <summary>A filled 2D wedge between two angles, in the plane around <paramref name="rotationAxis"/>.</summary>
    public static void Angle(
        float startDegrees,
        float endDegrees,
        Color solidColor,
        Color lineColor,
        Transform transform,
        Vec3 forwardAxis = default,
        Vec3 rotationAxis = default)
    {
        Vec3 forward = forwardAxis == default ? Vec3.AxisX : forwardAxis;
        Vec3 rotation = rotationAxis == default ? Vec3.AxisZ : rotationAxis;
        DebugApi.Require().DrawAngle(
            startDegrees, endDegrees, solidColor, lineColor, &transform, forward, rotation);
    }

    /// <summary>A cone with its tip at the transform's origin, opening along the forward axis.</summary>
    public static void OpeningCone(
        float halfAngleDegrees,
        Color colorInside,
        Color colorOutside,
        Transform transform,
        Vec3 forwardAxis = default)
    {
        Vec3 forward = forwardAxis == default ? Vec3.AxisX : forwardAxis;
        DebugApi.Require().DrawOpeningCone(
            halfAngleDegrees, colorInside, colorOutside, &transform, forward);
    }

    /// <summary>A bent cone opening by a different half angle along the local Y and Z axes.</summary>
    public static void LimitCone(
        float halfAngle1Degrees,
        float halfAngle2Degrees,
        Color solidColor,
        Color lineColor,
        Transform transform) =>
        DebugApi.Require().DrawLimitCone(
            halfAngle1Degrees, halfAngle2Degrees, solidColor, lineColor, &transform);

    /// <summary>A screen-space rectangle, in pixels.</summary>
    public static void Rectangle2D(
        float x,
        float y,
        float width,
        float height,
        Color color,
        float depth = 0.0f,
        bool linesOnly = false) =>
        DebugApi.Require().DrawRectangle2D(x, y, width, height, depth, color, linesOnly ? 1u : 0u);

    /// <summary>
    /// Screen-space text, positioned in pixels. Newlines split lines and tabs separate columns of a
    /// table. Returns the number of lines drawn.
    /// </summary>
    public static uint Text2D(
        string text,
        int x,
        int y,
        Color color,
        uint sizeInPixel = 16,
        TextHAlign horizontalAlignment = TextHAlign.Left,
        TextVAlign verticalAlignment = TextVAlign.Top)
    {
        uint lineCount = 0;
        Span<byte> scratch = stackalloc byte[StackTextBytes];
        byte[]? rented = null;
        Span<byte> encoded = EncodeUtf8(text, scratch, ref rented);

        fixed (byte* data = encoded)
        {
            DebugApi.Require().DrawText2D(MakeSpan(data, encoded.Length), x, y, sizeInPixel,
                (uint)horizontalAlignment, (uint)verticalAlignment, color, &lineCount);
        }

        Return(rented);
        return lineCount;
    }

    /// <summary>World-space text that always faces the camera and keeps a constant pixel size.</summary>
    public static uint Text3D(
        string text,
        Vec3 position,
        Color color,
        uint sizeInPixel = 16,
        TextHAlign horizontalAlignment = TextHAlign.Center,
        TextVAlign verticalAlignment = TextVAlign.Bottom,
        bool depthTest = false)
    {
        uint lineCount = 0;
        Span<byte> scratch = stackalloc byte[StackTextBytes];
        byte[]? rented = null;
        Span<byte> encoded = EncodeUtf8(text, scratch, ref rented);

        fixed (byte* data = encoded)
        {
            DebugApi.Require().DrawText3D(MakeSpan(data, encoded.Length), position, sizeInPixel,
                (uint)horizontalAlignment, (uint)verticalAlignment, depthTest ? 1u : 0u, color, &lineCount);
        }

        Return(rented);
        return lineCount;
    }

    /// <summary>
    /// Text that sits on a fixed plane in the world rather than facing the camera, so it takes
    /// perspective and is occluded by geometry. Glyph size is in world units.
    /// </summary>
    public static uint Text3DInWorld(
        string text,
        Transform transform,
        float glyphSize,
        Color color,
        TextHAlign horizontalAlignment = TextHAlign.Center,
        TextVAlign verticalAlignment = TextVAlign.Bottom)
    {
        uint lineCount = 0;
        Span<byte> scratch = stackalloc byte[StackTextBytes];
        byte[]? rented = null;
        Span<byte> encoded = EncodeUtf8(text, scratch, ref rented);

        fixed (byte* data = encoded)
        {
            DebugApi.Require().DrawText3DInWorld(MakeSpan(data, encoded.Length), &transform, glyphSize,
                (uint)horizontalAlignment, (uint)verticalAlignment, color, &lineCount);
        }

        Return(rented);
        return lineCount;
    }

    /// <summary>
    /// Text placed automatically in a screen corner, stacked so lines from the same corner never
    /// overlap. The group name only inserts whitespace between unrelated blocks; it is not drawn.
    /// </summary>
    public static void InfoText(
        string text,
        TextPlacement placement = TextPlacement.TopLeft,
        string group = "",
        Color color = default)
    {
        Span<byte> textScratch = stackalloc byte[StackTextBytes];
        Span<byte> groupScratch = stackalloc byte[128];
        byte[]? textRented = null;
        byte[]? groupRented = null;
        Span<byte> encodedText = EncodeUtf8(text, textScratch, ref textRented);
        Span<byte> encodedGroup = EncodeUtf8(group, groupScratch, ref groupRented);

        fixed (byte* textData = encodedText)
        fixed (byte* groupData = encodedGroup)
        {
            DebugApi.Require().DrawInfoText((uint)placement,
                MakeSpan(groupData, encodedGroup.Length), MakeSpan(textData, encodedText.Length),
                color == default ? Color.White : color);
        }

        Return(textRented);
        Return(groupRented);
    }

    /// <summary>Info text that stays on screen for a while.</summary>
    public static void AddPersistentInfoText(
        string text,
        Time duration,
        TextPlacement placement = TextPlacement.TopLeft,
        Color color = default)
    {
        Span<byte> scratch = stackalloc byte[StackTextBytes];
        byte[]? rented = null;
        Span<byte> encoded = EncodeUtf8(text, scratch, ref rented);

        fixed (byte* data = encoded)
        {
            DebugApi.Require().AddPersistentInfoText((uint)placement, MakeSpan(data, encoded.Length),
                duration.Seconds, color == default ? Color.White : color);
        }

        Return(rented);
    }

    public static void AddPersistentCross(float size, Color color, Transform transform, Time duration) =>
        DebugApi.Require().AddPersistentCross(size, color, &transform, duration.Seconds);

    public static void AddPersistentSphere(float radius, Color color, Transform transform, Time duration) =>
        DebugApi.Require().AddPersistentSphere(radius, color, &transform, duration.Seconds);

    public static void AddPersistentBox(Vec3 halfExtents, Color color, Transform transform, Time duration) =>
        DebugApi.Require().AddPersistentBox(halfExtents, color, &transform, duration.Seconds);

    /// <summary>Width in pixels of one glyph at the given text size.</summary>
    public static float GetTextGlyphWidth(uint sizeInPixel = 16)
    {
        GetTextMetrics(sizeInPixel, out float glyphWidth, out _, out _);
        return glyphWidth;
    }

    /// <summary>Line height in pixels at the given text size.</summary>
    public static float GetTextLineHeight(uint sizeInPixel = 16)
    {
        GetTextMetrics(sizeInPixel, out _, out float lineHeight, out _);
        return lineHeight;
    }

    /// <summary>The global debug text scale.</summary>
    public static float TextScale
    {
        get
        {
            GetTextMetrics(16, out _, out _, out float scale);
            return scale;
        }
        set => DebugApi.Require().SetTextScale(value);
    }

    /// <summary>Resolution of the first main view that can be found, in pixels.</summary>
    public static Vec2 GetResolution()
    {
        Vec2 resolution = default;
        DebugApi.Require().GetResolution(&resolution);
        return resolution;
    }

    private static void GetTextMetrics(
        uint sizeInPixel, out float glyphWidth, out float lineHeight, out float scale)
    {
        float width = 0, height = 0, textScale = 0;
        DebugApi.Require().GetTextMetrics(sizeInPixel, &width, &height, &textScale);
        glyphWidth = width;
        lineHeight = height;
        scale = textScale;
    }

    private static Utf8Span MakeSpan(byte* data, int length) => new()
    {
        Data = data,
        Length = checked((uint)length),
    };

    /// <summary>
    /// Encodes into <paramref name="scratch"/> when it fits, and only rents when it does not. Debug
    /// text is drawn every frame, so the common short label must not touch the heap.
    /// </summary>
    private static Span<byte> EncodeUtf8(string? value, Span<byte> scratch, ref byte[]? rented)
    {
        if (string.IsNullOrEmpty(value))
        {
            return Span<byte>.Empty;
        }

        int maximum = Encoding.UTF8.GetMaxByteCount(value.Length);
        Span<byte> destination = scratch;
        if (maximum > scratch.Length)
        {
            rented = ArrayPool<byte>.Shared.Rent(maximum);
            destination = rented;
        }

        int written = Encoding.UTF8.GetBytes(value, destination);
        return destination[..written];
    }

    private static void Return(byte[]? rented)
    {
        if (rented is not null)
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }
}
