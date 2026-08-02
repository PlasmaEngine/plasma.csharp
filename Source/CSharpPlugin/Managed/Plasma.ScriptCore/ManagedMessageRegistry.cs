using System.Collections.Concurrent;
using System.ComponentModel;
using System.Threading;

namespace Plasma;

/// <summary>Internal synchronous object registry used by custom C# messages.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static class ManagedMessageRegistry
{
    private static readonly ConcurrentDictionary<ulong, object> Messages = [];
    private static long s_nextToken;

    public static ulong Register(object message)
    {
        ArgumentNullException.ThrowIfNull(message);

        ulong token;
        do
        {
            token = checked((ulong)Interlocked.Increment(ref s_nextToken));
        }
        while (token == 0 || !Messages.TryAdd(token, message));

        return token;
    }

    public static object Resolve(ulong token, Type expectedType)
    {
        ArgumentNullException.ThrowIfNull(expectedType);
        if (token == 0 || !Messages.TryGetValue(token, out object? message))
        {
            throw new InvalidOperationException(
                $"Custom C# message token 0x{token:X16} is no longer valid.");
        }

        if (!expectedType.IsInstanceOfType(message))
        {
            throw new InvalidCastException(
                $"Custom C# message token 0x{token:X16} contains '{message.GetType()}', " +
                $"not the expected '{expectedType}'.");
        }

        return message;
    }

    public static void Release(ulong token)
    {
        if (token != 0)
        {
            Messages.TryRemove(token, out _);
        }
    }
}
