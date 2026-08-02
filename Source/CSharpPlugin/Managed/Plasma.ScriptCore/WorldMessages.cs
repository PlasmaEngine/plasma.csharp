using System.Reflection;
using System.Text.Json;

namespace Plasma;

/// <summary>Native message routing helpers for generated Plasma message values.</summary>
public static class WorldMessages
{
    public static void SendMessage<T>(this GameObject target, in T message)
        =>
        Send(target, default, message, 0);

    public static void SendMessageRecursive<T>(this GameObject target, in T message)
        =>
        Send(target, default, message, 1);

    public static void SendEventMessage<T>(
        this GameObject target,
        in T message,
        Component sender)
        =>
        Send(target, sender.Native, message, 2);

    private static void Send<T>(
        GameObject target,
        NativeObject sender,
        in T message,
        uint routing)
    {
        Type messageType = typeof(T);
        if (message is null)
        {
            throw new ArgumentNullException(nameof(message));
        }

        if (messageType.GetCustomAttribute<GeneratedMessageAttribute>() is
            GeneratedMessageAttribute generated)
        {
            string payload = JsonSerializer.Serialize(message);
            ThrowIfFailed(
                NativeBridge.SendMessage(
                    target.Native,
                    sender,
                    generated.NativeTypeName,
                    payload,
                    routing),
                generated.NativeTypeName);
            return;
        }

        if (messageType.GetCustomAttribute<PlasmaMessageAttribute>() is null)
        {
            throw new ArgumentException(
                $"'{messageType}' is neither a generated native message nor marked [PlasmaMessage].",
                nameof(message));
        }

        ulong token = ManagedMessageRegistry.Register(message!);
        try
        {
            ThrowIfFailed(
                NativeBridge.SendManagedMessage(
                    target.Native,
                    sender,
                    StableId.CustomMessage(messageType),
                    token,
                    routing),
                messageType.FullName ?? messageType.Name);
        }
        finally
        {
            ManagedMessageRegistry.Release(token);
        }
    }

    private static void ThrowIfFailed(NativeCallStatus status, string messageName)
    {
        if (status != NativeCallStatus.Success)
        {
            throw new PlasmaException(
                $"Sending Plasma message '{messageName}' failed with status {status}.",
                (int)status);
        }
    }
}
