using System.Diagnostics;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using Plasma.Testing;

namespace Plasma.ScriptInspector;

/// <summary>Runs the [PlasmaTest] methods of a game assembly and writes one result per test as JSON.</summary>
/// <remarks>The output is <c>{"tests":[{"name","passed","message","file","line","ms"}]}</c>. The return code is 0 when
/// every test passed, 3 when one failed. Tests run without the engine, so a test that calls native code fails.</remarks>
internal static class TestRunner
{
    private sealed record Result(string Name, bool Passed, string Message, string File, int Line, double Milliseconds);

    public static int Run(string assemblyPath, string outputPath, Func<string, AssemblyLoadContext> createContext)
    {
        AssemblyLoadContext context = createContext(Path.GetFullPath(assemblyPath));
        var results = new List<Result>();

        try
        {
            Assembly assembly = context.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
            foreach (MethodInfo method in FindTests(assembly, results))
            {
                results.Add(RunOne(assembly, method));
            }
        }
        finally
        {
            context.Unload();
        }

        Write(outputPath, results);
        return results.All(static result => result.Passed) ? 0 : 3;
    }

    private static IEnumerable<MethodInfo> FindTests(Assembly assembly, List<Result> results)
    {
        Type[] types;
        try
        {
            types = assembly.GetTypes();
        }
        catch (ReflectionTypeLoadException exception)
        {
            // Types that do not load cannot run; say why instead of skipping their tests silently.
            types = exception.Types.Where(static type => type is not null).ToArray()!;
            string reasons = string.Join("; ", exception.LoaderExceptions.Where(static e => e is not null).Select(static e => e!.Message).Distinct());
            results.Add(new Result("Load test types", false, reasons, string.Empty, 0, 0));
        }

        const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly;
        return types
            .SelectMany(type => type.GetMethods(flags))
            .Where(static method => IsTest(method) && method.GetParameters().Length == 0)
            .OrderBy(static method => method.DeclaringType?.FullName, StringComparer.Ordinal)
            .ThenBy(static method => method.Name, StringComparer.Ordinal);
    }

    /// <summary>By name: the game may see a ScriptCore loaded apart from this tool's copy.</summary>
    private static bool IsTest(MethodInfo method) =>
        method.GetCustomAttributesData().Any(static data => data.AttributeType.FullName == typeof(PlasmaTestAttribute).FullName);

    private static Result RunOne(Assembly assembly, MethodInfo method)
    {
        string name = $"{method.DeclaringType?.Name}.{method.Name}";
        var watch = Stopwatch.StartNew();

        try
        {
            object? target = method.IsStatic ? null : Activator.CreateInstance(method.DeclaringType!);
            method.Invoke(target, null);
            return new Result(name, true, string.Empty, string.Empty, 0, watch.Elapsed.TotalMilliseconds);
        }
        catch (TargetInvocationException invocation) when (invocation.InnerException is not null)
        {
            return Failed(assembly, name, invocation.InnerException, watch);
        }
        catch (Exception exception)
        {
            return Failed(assembly, name, exception, watch);
        }
    }

    private static Result Failed(Assembly assembly, string name, Exception exception, Stopwatch watch)
    {
        string message = exception.GetType().FullName == typeof(AssertFailedException).FullName ? exception.Message : $"{exception.GetType().Name}: {exception.Message}";

        // The innermost frame in the game assembly that has a source line: where the test failed.
        string file = string.Empty;
        int line = 0;
        foreach (StackFrame frame in new StackTrace(exception, fNeedFileInfo: true).GetFrames())
        {
            if (frame.GetMethod()?.DeclaringType?.Assembly != assembly || frame.GetFileName() is not { Length: > 0 } frameFile)
                continue;

            file = frameFile;
            line = frame.GetFileLineNumber();
            break;
        }

        return new Result(name, false, message, file, line, watch.Elapsed.TotalMilliseconds);
    }

    private static void Write(string outputPath, IReadOnlyList<Result> results)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath) ?? ".");

        using FileStream stream = File.Create(outputPath);
        using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });

        writer.WriteStartObject();
        writer.WriteStartArray("tests");
        foreach (Result result in results)
        {
            writer.WriteStartObject();
            writer.WriteString("name", result.Name);
            writer.WriteBoolean("passed", result.Passed);
            writer.WriteString("message", result.Message);
            writer.WriteString("file", result.File);
            writer.WriteNumber("line", result.Line);
            writer.WriteNumber("ms", result.Milliseconds);
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
        writer.WriteEndObject();
    }
}
