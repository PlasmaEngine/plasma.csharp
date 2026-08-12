using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Plasma.ManagedHost;
using Plasma.ScriptTestGame;

namespace Plasma.ManagedTests;

/// <summary>
/// Round-trips [ConsoleCommand] through the real host: generator output, generation load, the
/// manifest the native registry parses, and the managed extension native code invokes through.
/// </summary>
public sealed unsafe class ConsoleCommandTests
{
    private static string s_lastLog = string.Empty;

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static CSharpStatus CaptureLog(CSharpLogLevel level, CSharpUtf8Span message)
    {
        _ = level;
        s_lastLog = message.Data is null
            ? string.Empty
            : Encoding.UTF8.GetString(message.Data, (int)message.Length);
        return CSharpStatus.Success;
    }

    [Fact]
    public void GeneratedCommandsSurviveTheHostRoundTrip()
    {
        ManagedApiV1 api = InitializeHost();
        try
        {
            ulong generation = LoadTestGameGeneration(api);

            JsonElement commands = ReadManifestCommands(api, generation);
            Assert.Equal(3, commands.GetArrayLength());

            JsonElement echo = FindCommand(commands, "test.echo");
            Assert.Equal(
                "(string text, int count, bool flag, float scale, double bias, uint steps)",
                echo.GetProperty("help").GetString());

            // Kinds must match Plasma.ScriptCommandParameterKind, which is what the native registry
            // turns into plVariantType values for the console.
            Assert.Equal(
                new[] { 5u, 1u, 0u, 3u, 4u, 2u },
                echo.GetProperty("parameters").EnumerateArray().Select(value => value.GetUInt32()).ToArray());

            ManagedConsoleApiV1* console = QueryConsoleApi(api);

            s_lastLog = string.Empty;
            Assert.Equal(CSharpStatus.Success, console->InvokeCommand(
                generation, StableId.ConsoleCommand("test.noargs"), null, 0));
            Assert.Equal("noargs", s_lastLog);

            // Every supported parameter kind, in one call, checked after marshalling.
            InvokeEcho(console, generation);
            Assert.Equal("hello|7|True|1.5|2.25|9", s_lastLog);
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, api.Shutdown());
        }
    }

    [Fact]
    public void CommandFailuresAreReportedRatherThanThrownAcrossTheBoundary()
    {
        ManagedApiV1 api = InitializeHost();
        try
        {
            ulong generation = LoadTestGameGeneration(api);
            ManagedConsoleApiV1* console = QueryConsoleApi(api);

            // A throwing command must come back as a status, not unwind into native code.
            Assert.Equal(CSharpStatus.ManagedException, console->InvokeCommand(
                generation, StableId.ConsoleCommand("test.throws"), null, 0));

            // Wrong argument count.
            Assert.Equal(CSharpStatus.InvalidArgument, console->InvokeCommand(
                generation, StableId.ConsoleCommand("test.noargs"), null, 3));

            // Unknown command id.
            Assert.Equal(CSharpStatus.MemberNotFound, console->InvokeCommand(
                generation, StableId.ConsoleCommand("test.missing"), null, 0));

            // Unknown generation.
            Assert.Equal(CSharpStatus.GenerationNotFound, console->InvokeCommand(
                generation + 1000, StableId.ConsoleCommand("test.noargs"), null, 0));

            // A string argument where a bool is expected.
            byte[] textBytes = Encoding.UTF8.GetBytes("not-a-bool");
            fixed (byte* text = textBytes)
            {
                CSharpValue* arguments = stackalloc CSharpValue[6];
                arguments[0] = new CSharpValue
                {
                    Kind = CSharpValueKind.Utf8String,
                    Payload0 = (ulong)text,
                    Payload1 = (ulong)textBytes.Length,
                };
                for (int index = 1; index < 6; ++index)
                {
                    // Deliberately the wrong kind for every remaining parameter.
                    arguments[index] = new CSharpValue { Kind = CSharpValueKind.Null };
                }

                Assert.Equal(CSharpStatus.InvalidValue, console->InvokeCommand(
                    generation, StableId.ConsoleCommand("test.echo"), arguments, 6));
            }
        }
        finally
        {
            Assert.Equal(CSharpStatus.Success, api.Shutdown());
        }
    }

    private static void InvokeEcho(ManagedConsoleApiV1* console, ulong generation)
    {
        byte[] textBytes = Encoding.UTF8.GetBytes("hello");
        fixed (byte* text = textBytes)
        {
            CSharpValue* arguments = stackalloc CSharpValue[6];
            arguments[0] = new CSharpValue
            {
                Kind = CSharpValueKind.Utf8String,
                Payload0 = (ulong)text,
                Payload1 = (ulong)textBytes.Length,
            };
            arguments[1] = new CSharpValue { Kind = CSharpValueKind.Int64, Payload0 = 7 };
            arguments[2] = new CSharpValue { Kind = CSharpValueKind.Boolean, Payload0 = 1 };
            arguments[3] = new CSharpValue
            {
                Kind = CSharpValueKind.Double,
                Payload0 = BitConverter.DoubleToUInt64Bits(1.5),
            };
            arguments[4] = new CSharpValue
            {
                Kind = CSharpValueKind.Double,
                Payload0 = BitConverter.DoubleToUInt64Bits(2.25),
            };
            arguments[5] = new CSharpValue { Kind = CSharpValueKind.UInt64, Payload0 = 9 };

            Assert.Equal(CSharpStatus.Success, console->InvokeCommand(
                generation, StableId.ConsoleCommand("test.echo"), arguments, 6));
        }
    }

    private static ManagedApiV1 InitializeHost()
    {
        var nativeApi = new NativeApiV1
        {
            Size = (uint)sizeof(NativeApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
            Log = &CaptureLog,
        };

        var managedApi = new ManagedApiV1
        {
            Size = (uint)sizeof(ManagedApiV1),
            Version = 1,
            PointerSize = (uint)sizeof(void*),
            ValueLayoutVersion = 2,
        };

        delegate* unmanaged[Cdecl]<NativeApiV1*, ManagedApiV1*, CSharpStatus> initialize =
            &Bootstrap.Initialize;
        Assert.Equal(CSharpStatus.Success, initialize(&nativeApi, &managedApi));
        return managedApi;
    }

    private static ulong LoadTestGameGeneration(ManagedApiV1 api)
    {
        byte[] pathBytes = Encoding.UTF8.GetBytes(typeof(RotatingBeacon).Assembly.Location);
        ulong generation = 0;
        fixed (byte* path = pathBytes)
        {
            var load = new CSharpGenerationLoadDesc
            {
                Size = (uint)sizeof(CSharpGenerationLoadDesc),
                Version = 1,
                AssemblyPath = new CSharpUtf8Span
                {
                    Data = path,
                    Length = checked((uint)pathBytes.Length),
                },
            };
            Assert.Equal(CSharpStatus.Success, api.LoadGeneration(&load, &generation));
        }

        return generation;
    }

    private static JsonElement ReadManifestCommands(ManagedApiV1 api, ulong generation)
    {
        CSharpBuffer descriptor = default;
        Assert.Equal(CSharpStatus.Success, api.GetGenerationDescriptor(generation, &descriptor));

        string json = Encoding.UTF8.GetString((byte*)descriptor.Data, (int)descriptor.Size);
        api.FreeBuffer(&descriptor);

        using var document = JsonDocument.Parse(json);
        return document.RootElement.GetProperty("commands").Clone();
    }

    private static JsonElement FindCommand(JsonElement commands, string name) =>
        commands.EnumerateArray().Single(command => command.GetProperty("name").GetString() == name);

    private static ManagedConsoleApiV1* QueryConsoleApi(ManagedApiV1 api)
    {
        byte[] nameBytes = Encoding.UTF8.GetBytes("Plasma.Console");
        void* extension = null;
        fixed (byte* name = nameBytes)
        {
            Assert.Equal(CSharpStatus.Success, api.QueryExtension(
                new CSharpUtf8Span { Data = name, Length = checked((uint)nameBytes.Length) },
                1,
                &extension));
        }

        Assert.True(extension is not null);
        var console = (ManagedConsoleApiV1*)extension;
        Assert.True(console->Size >= (uint)sizeof(ManagedConsoleApiV1));
        Assert.Equal(1u, console->Version);
        Assert.True(console->InvokeCommand is not null);
        return console;
    }
}
