using System.Globalization;

namespace Plasma;

/// <summary>Writes gameplay messages to the Plasma log.</summary>
public static class Log
{
    public static void Debug(string message) => NativeBridge.WriteLog(0, message);

    public static void Debug(string format, params object?[] arguments) =>
        Debug(Format(format, arguments));

    public static void Dev(string message) => NativeBridge.WriteLog(4, message);

    public static void Dev(string format, params object?[] arguments) =>
        Dev(Format(format, arguments));

    public static void Info(string message) => NativeBridge.WriteLog(1, message);

    public static void Info(string format, params object?[] arguments) =>
        Info(Format(format, arguments));

    public static void Success(string message) => NativeBridge.WriteLog(5, message);

    public static void Success(string format, params object?[] arguments) =>
        Success(Format(format, arguments));

    public static void Warning(string message) => NativeBridge.WriteLog(2, message);

    public static void Warning(string format, params object?[] arguments) =>
        Warning(Format(format, arguments));

    public static void SeriousWarning(string message) =>
        NativeBridge.WriteLog(6, message);

    public static void SeriousWarning(string format, params object?[] arguments) =>
        SeriousWarning(Format(format, arguments));

    public static void Error(string message) => NativeBridge.WriteLog(3, message);

    public static void Error(string format, params object?[] arguments) =>
        Error(Format(format, arguments));

    public static void Error(Exception exception) => Error(exception.ToString());

    public static void Error(string message, Exception exception) =>
        Error("{0}{1}{2}", message, Environment.NewLine, exception);

    private static string Format(string format, object?[] arguments) =>
        string.Format(CultureInfo.InvariantCulture, format, arguments);
}
