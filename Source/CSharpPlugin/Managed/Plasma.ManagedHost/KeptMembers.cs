using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;

[assembly: InternalsVisibleTo("Plasma.ManagedTests")]

namespace Plasma.ManagedHost;

/// <summary>Carries [KeepOnReload] members across a full reload as a JSON object keyed by member name.</summary>
/// <remarks>
/// Runs only when a generation is replaced, so reflection is fine here. Serializer options are created per
/// call: the shared default options cache type metadata for the life of the process, which would keep a
/// retired script generation from being collected. The member lists live in a weak table for the same reason.
/// </remarks>
internal static class KeptMembers
{
    private static readonly ConditionalWeakTable<Type, MemberInfo[]> Cache = new();

    public static bool Has(Type type) => Members(type).Length > 0;

    /// <summary>The kept members as JSON, or null when the script has none.</summary>
    public static string? Capture(object script)
    {
        MemberInfo[] members = Members(script.GetType());
        if (members.Length == 0)
            return null;

        JsonSerializerOptions options = CreateOptions();
        var values = new Dictionary<string, JsonElement>(members.Length, StringComparer.Ordinal);
        foreach (MemberInfo member in members)
        {
            (Type type, object? value) = Read(member, script);
            values[member.Name] = JsonSerializer.SerializeToElement(value, type, options);
        }

        return JsonSerializer.Serialize(values, options);
    }

    /// <summary>Applies each value whose member still exists with a compatible type; anything else keeps its default.</summary>
    public static void Restore(object script, JsonElement state)
    {
        if (state.ValueKind != JsonValueKind.Object)
            return;

        JsonSerializerOptions options = CreateOptions();
        foreach (MemberInfo member in Members(script.GetType()))
        {
            if (!state.TryGetProperty(member.Name, out JsonElement value))
                continue;

            Type type = member is FieldInfo field ? field.FieldType : ((PropertyInfo)member).PropertyType;
            object? restored;
            try
            {
                restored = value.Deserialize(type, options);
            }
            catch (JsonException)
            {
                // The member's type changed in this edit: it keeps the value the new code gave it.
                continue;
            }
            catch (NotSupportedException)
            {
                continue;
            }

            if (member is FieldInfo targetField)
                targetField.SetValue(script, restored);
            else
                ((PropertyInfo)member).SetValue(script, restored);
        }
    }

    private static MemberInfo[] Members(Type type) =>
        Cache.GetValue(type, static scriptType =>
        {
            var members = new List<MemberInfo>();
            var names = new HashSet<string>(StringComparer.Ordinal);
            for (Type? current = scriptType; current is not null && current != typeof(ComponentScript); current = current.BaseType)
            {
                const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
                foreach (MemberInfo member in current.GetMembers(Flags))
                {
                    bool kept = member.IsDefined(typeof(KeepOnReloadAttribute), inherit: true);
                    bool usable = member switch
                    {
                        FieldInfo field => !field.IsInitOnly && !field.IsLiteral,
                        PropertyInfo property => property.CanRead && property.CanWrite && property.GetIndexParameters().Length == 0,
                        _ => false,
                    };

                    if (kept && usable && names.Add(member.Name))
                        members.Add(member);
                }
            }

            return members.ToArray();
        });

    private static (Type Type, object? Value) Read(MemberInfo member, object script) => member switch
    {
        FieldInfo field => (field.FieldType, field.GetValue(script)),
        PropertyInfo property => (property.PropertyType, property.GetValue(script)),
        _ => throw new InvalidOperationException(),
    };

    private static JsonSerializerOptions CreateOptions() => new() { IncludeFields = true };
}
