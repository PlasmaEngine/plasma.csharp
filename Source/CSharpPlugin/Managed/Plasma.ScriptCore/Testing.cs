using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace Plasma.Testing;

/// <summary>Marks a method as a script test. The editor's project checks run it (ScriptTests kind).</summary>
/// <remarks>The method takes no parameters. It is static, or its class has a parameterless constructor.
/// Tests run outside a world, so they test script logic, not native objects.</remarks>
[AttributeUsage(AttributeTargets.Method, Inherited = false)]
public sealed class PlasmaTestAttribute : Attribute;

/// <summary>Thrown by <see cref="Assert"/> when a check fails. The test runner reports its message and place.</summary>
public sealed class AssertFailedException(string message) : Exception(message);

/// <summary>Checks for script tests. Each failure throws <see cref="AssertFailedException"/>.</summary>
public static class Assert
{
    public static void True(bool condition, string? message = null,
        [CallerArgumentExpression(nameof(condition))] string? expression = null)
    {
        if (!condition)
            Fail(message ?? $"Expected true: {expression}");
    }

    public static void False(bool condition, string? message = null,
        [CallerArgumentExpression(nameof(condition))] string? expression = null)
    {
        if (condition)
            Fail(message ?? $"Expected false: {expression}");
    }

    public static void Equal<T>(T expected, T actual, string? message = null)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            Fail(message ?? $"Assert.Equal failed. Expected {Describe(expected)}, actual {Describe(actual)}");
    }

    public static void NotEqual<T>(T unexpected, T actual, string? message = null)
    {
        if (EqualityComparer<T>.Default.Equals(unexpected, actual))
            Fail(message ?? $"Assert.NotEqual failed. Both are {Describe(actual)}");
    }

    /// <summary>Equal within a tolerance, for floating point results.</summary>
    public static void Near(double expected, double actual, double tolerance = 1e-4, string? message = null)
    {
        if (double.IsNaN(actual) || Math.Abs(expected - actual) > tolerance)
            Fail(message ?? $"Assert.Near failed. Expected {expected} +/- {tolerance}, actual {actual}");
    }

    public static void Null(object? value, string? message = null)
    {
        if (value is not null)
            Fail(message ?? $"Expected null, actual {Describe(value)}");
    }

    public static void NotNull(object? value, string? message = null)
    {
        if (value is null)
            Fail(message ?? "Expected a value, actual null");
    }

    /// <summary>Runs the action and fails unless it throws <typeparamref name="TException"/>, which is returned.</summary>
    public static TException Throws<TException>(Action action, string? message = null)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException exception)
        {
            return exception;
        }
        catch (Exception other)
        {
            Fail(message ?? $"Expected {typeof(TException).Name}, but {other.GetType().Name} was thrown: {other.Message}");
        }

        Fail(message ?? $"Expected {typeof(TException).Name}, but nothing was thrown");
        return null!;
    }

    public static void Fail(string message) => throw new AssertFailedException(message);

    private static string Describe<T>(T value) => value switch
    {
        null => "null",
        string text => $"\"{text}\"",
        _ => value.ToString() ?? string.Empty,
    };
}
