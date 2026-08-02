using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Plasma.Engine.Generator;

namespace Plasma.ManagedTests;

public sealed class GeneratorDiagnosticTests
{
    [Fact]
    public void ReportsInvalidScriptShapeAtCompileTime()
    {
        const string source =
            """
            using Plasma;
            [PlasmaScript(Id = "not-a-guid")]
            public class BadScript : ComponentScript
            {
                [Expose] public object Value { get; set; } = new();
                [MessageHandler] private int BadHandler() => 0;
            }
            """;

        CSharpCompilation compilation = CreateCompilation(source);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out _,
            out ImmutableArray<Diagnostic> diagnostics);

        string[] ids = driver.GetRunResult().Diagnostics
            .Concat(diagnostics)
            .Select(static diagnostic => diagnostic.Id)
            .Distinct()
            .ToArray();

        Assert.Contains("PLCS001", ids);
        Assert.Contains("PLCS002", ids);
        Assert.Contains("PLCS005", ids);
        Assert.Contains("PLCS006", ids);
        Assert.Contains("PLCS007", ids);
    }

    [Fact]
    public void ReportsInaccessibleExposedMembersWithoutRawCompilerErrors()
    {
        const string source =
            """
            using Plasma;

            [PlasmaScript]
            public sealed partial class AccessScript : ComponentScript
            {
                [Expose] private int PrivateField;
                [Expose] protected int ProtectedField;
                [Expose] private protected int PrivateProtectedField;

                [Expose]
                public int PrivateSetter { get; private set; }

                [Expose]
                internal int PrivateGetter { private get; set; }
            }
            """;

        CSharpCompilation compilation = CreateCompilation(source);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out _);

        string[] messages = driver.GetRunResult().Diagnostics
            .Where(static diagnostic => diagnostic.Id == "PLCS006")
            .Select(static diagnostic => diagnostic.GetMessage())
            .ToArray();

        Assert.Equal(5, messages.Length);
        Assert.Contains(
            messages,
            static message => message.Contains(
                "the field must be public, internal, or protected internal",
                StringComparison.Ordinal));
        Assert.Contains(
            messages,
            static message => message.Contains(
                "the property and both accessors must be public, internal, or protected internal",
                StringComparison.Ordinal));
        Assert.DoesNotContain(
            generated.GetDiagnostics(),
            static diagnostic =>
                diagnostic.Severity == DiagnosticSeverity.Error &&
                diagnostic.Id.StartsWith("CS", StringComparison.Ordinal));
    }

    [Fact]
    public void ReportsUnsupportedExposedPropertyShapes()
    {
        const string source =
            """
            using Plasma;

            public interface IValue
            {
                int Value { get; set; }
            }

            [PlasmaScript]
            public sealed partial class PropertyShapeScript : ComponentScript, IValue
            {
                [Expose]
                public int this[int index]
                {
                    get => index;
                    set { }
                }

                [Expose]
                public int InitOnly { get; init; }

                [Expose]
                int IValue.Value { get; set; }
            }
            """;

        CSharpCompilation compilation = CreateCompilation(source);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out _);

        string[] messages = driver.GetRunResult().Diagnostics
            .Where(static diagnostic => diagnostic.Id == "PLCS006")
            .Select(static diagnostic => diagnostic.GetMessage())
            .ToArray();

        Assert.Equal(3, messages.Length);
        Assert.Contains(
            messages,
            static message => message.Contains("indexers are not supported", StringComparison.Ordinal));
        Assert.Contains(
            messages,
            static message => message.Contains(
                "init-only properties are not writable after construction",
                StringComparison.Ordinal));
        Assert.Contains(
            messages,
            static message => message.Contains(
                "explicit interface implementations are not supported",
                StringComparison.Ordinal));
        Assert.DoesNotContain(
            generated.GetDiagnostics(),
            static diagnostic =>
                diagnostic.Severity == DiagnosticSeverity.Error &&
                diagnostic.Id.StartsWith("CS", StringComparison.Ordinal));
    }

    [Fact]
    public void GeneratesDescriptorsForMembersAccessibleWithinTheAssembly()
    {
        const string source =
            """
            using Plasma;

            [PlasmaScript]
            public sealed partial class AccessibleScript : ComponentScript
            {
                [Expose] public int PublicField;
                [Expose] internal int InternalProperty { get; set; }
                [Expose] protected internal int ProtectedInternalProperty { get; set; }
            }
            """;

        CSharpCompilation compilation = CreateCompilation(source);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out _);

        Assert.DoesNotContain(
            driver.GetRunResult().Diagnostics,
            static diagnostic => diagnostic.Id == "PLCS006");
        Assert.DoesNotContain(
            generated.GetDiagnostics(),
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
    }

    [Fact]
    public void PrivateMessageHandlersRemainSupportedByGeneratedBridges()
    {
        const string source =
            """
            using Plasma;

            [GeneratedMessage("plMsgPrivate")]
            public readonly record struct PrivateMessage(int Value);

            [PlasmaScript]
            public sealed partial class PrivateHandlerScript : ComponentScript
            {
                [MessageHandler]
                private void OnPrivate(in PrivateMessage message) { }
            }
            """;

        CSharpCompilation compilation = CreateCompilation(source);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out _);

        Assert.DoesNotContain(
            driver.GetRunResult().Diagnostics,
            static diagnostic => diagnostic.Id == "PLCS007");
        Assert.DoesNotContain(
            generated.GetDiagnostics(),
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
        string generatedSource = Assert.Single(driver.GetRunResult().GeneratedTrees)
            .GetText()
            .ToString();
        Assert.Contains("OnPrivate(in message);", generatedSource, StringComparison.Ordinal);
    }

    [Fact]
    public void CustomMessagesUseAutomaticTypeIdentityAndNativeEnvelope()
    {
        const string source =
            """
            using Plasma;
            namespace Example;

            [PlasmaMessage]
            public sealed class AddConsumable
            {
                public int Amount { get; set; }
                public bool Consumed { get; set; }
            }

            [PlasmaScript]
            public sealed partial class Player : ComponentScript
            {
                [MessageHandler]
                private void OnAddConsumable(AddConsumable message)
                {
                    message.Consumed = true;
                }
            }
            """;

        string generatedSource = RunGenerator(source);
        ulong messageId = StableId.CustomMessage(
            "GeneratorTest",
            "Example.AddConsumable");

        Assert.Contains(
            $"[0x{messageId:X16}UL] = new global::Plasma.ScriptMessageDescriptor(",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "\"plMsgDeliverCSharpMsg\", typeof(global::Example.AddConsumable)",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains("OnAddConsumable(message);", generatedSource, StringComparison.Ordinal);
    }

    [Fact]
    public void ManagedMessageRegistryPreservesReferenceMutations()
    {
        var message = new RegistryMessage();
        ulong token = ManagedMessageRegistry.Register(message);
        try
        {
            var resolved = Assert.IsType<RegistryMessage>(
                ManagedMessageRegistry.Resolve(token, typeof(RegistryMessage)));
            resolved.Consumed = true;
            Assert.True(message.Consumed);
        }
        finally
        {
            ManagedMessageRegistry.Release(token);
        }

        Assert.Throws<InvalidOperationException>(() =>
            ManagedMessageRegistry.Resolve(token, typeof(RegistryMessage)));
    }

    [Fact]
    public void GeneratesPersistentScriptIdWhenOverrideIsOmitted()
    {
        const string source =
            """
            using Plasma;
            namespace Example;

            [PlasmaScript]
            public sealed partial class AutomaticIdScript : ComponentScript
            {
            }
            """;

        string generatedSource = RunGenerator(source);
        Guid expected = StableId.ScriptPersistentId(
            "GeneratorTest",
            "Example.AutomaticIdScript");

        Assert.Equal(new Guid("0df4ca6a-117c-5e36-ac87-b5f9224b6557"), expected);
        Assert.Contains(
            $"PersistentId = new global::System.Guid(\"{expected:D}\")",
            generatedSource,
            StringComparison.Ordinal);
    }

    private sealed class RegistryMessage
    {
        public bool Consumed { get; set; }
    }

    [Fact]
    public void RejectsEmptyAndDuplicatePersistentScriptIds()
    {
        const string source =
            """
            using Plasma;

            [PlasmaScript(Id = "00000000-0000-0000-0000-000000000000")]
            public sealed partial class EmptyIdScript : ComponentScript
            {
            }

            [PlasmaScript(Id = "8a1678c1-8f42-48ee-b956-7180bf16942e")]
            public sealed partial class FirstScript : ComponentScript
            {
            }

            [PlasmaScript(Id = "8a1678c1-8f42-48ee-b956-7180bf16942e")]
            public sealed partial class SecondScript : ComponentScript
            {
            }
            """;

        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGenerators(CreateCompilation(source));
        ImmutableArray<Diagnostic> diagnostics = driver.GetRunResult().Diagnostics;

        Assert.Contains(diagnostics, static diagnostic => diagnostic.Id == "PLCS005");
        Assert.Equal(2, diagnostics.Count(static diagnostic => diagnostic.Id == "PLCS009"));
    }

    [Fact]
    public void RequiresGeneratedUniqueNativeMessages()
    {
        const string source =
            """
            using Plasma;

            [GeneratedMessage("plMsgTest")]
            public readonly record struct TestMessage(int Value);

            [PlasmaScript(Id = "e5232664-376a-4adc-8549-23f8491316d7")]
            public sealed partial class MessageScript : ComponentScript
            {
                [MessageHandler] private void First(TestMessage message) { }
                [MessageHandler] private void Second(in TestMessage message) { }
                [MessageHandler] private void Primitive(long value) { }
            }
            """;

        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGenerators(CreateCompilation(source));
        ImmutableArray<Diagnostic> diagnostics = driver.GetRunResult().Diagnostics;

        Assert.Contains(diagnostics, static diagnostic => diagnostic.Id == "PLCS007");
        Assert.Contains(diagnostics, static diagnostic => diagnostic.Id == "PLCS008");
    }

    [Fact]
    public void GeneratedOutputIsDeterministic()
    {
        const string source =
            """
            using Plasma;
            namespace Example;
            [PlasmaScript]
            public sealed partial class Script : ComponentScript
            {
                [Expose] public int Count { get; set; } = 4;
            }
            """;

        string first = RunGenerator(source);
        string second = RunGenerator(source);
        Assert.Equal(first, second);
    }

    [Fact]
    public void ExplicitPersistentIdOverridesAutomaticIdentity()
    {
        const string source =
            """
            using Plasma;

            [PlasmaScript(Id = "52db5685-d52a-4d73-96c0-ae53a61af364")]
            public sealed partial class RenamedScript : ComponentScript
            {
            }
            """;

        string generatedSource = RunGenerator(source);

        Assert.Contains(
            "PersistentId = new global::System.Guid(\"52db5685-d52a-4d73-96c0-ae53a61af364\")",
            generatedSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            StableId.ScriptPersistentId("GeneratorTest", "RenamedScript")
                .ToString("D"),
            generatedSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void BindingManifestProducesCompilableTypedApi()
    {
        const string manifest =
            """
            {
              "manifestVersion": 1,
              "canonicalIdentityVersion": 1,
              "schemaHash": "0000000000000001",
              "types": [
                {
                  "id": "0000000000000001", "canonicalIdentity": "type:plGameObject",
                  "nativeName": "plGameObject", "pluginName": "Core", "parentNativeName": "",
                  "scriptExtensionName": "", "flags": 8, "variantType": 0,
                  "typeVersion": 1, "typeSize": 8, "canAllocate": false,
                  "hidden": false, "excludedFromScript": false, "attributes": []
                },
                {
                  "id": "0000000000000002", "canonicalIdentity": "type:plScriptExtensionClass_Log",
                  "nativeName": "plScriptExtensionClass_Log", "pluginName": "Core",
                  "parentNativeName": "", "scriptExtensionName": "Log", "flags": 8,
                  "variantType": 0, "typeVersion": 1, "typeSize": 1, "canAllocate": false,
                  "hidden": false, "excludedFromScript": false, "attributes": []
                },
                {
                  "id": "0000000000000003", "canonicalIdentity": "type:plMsgBeacon",
                  "nativeName": "plMsgBeacon", "pluginName": "Test", "parentNativeName": "plMessage",
                  "scriptExtensionName": "", "flags": 8, "variantType": 0,
                  "typeVersion": 1, "typeSize": 1, "canAllocate": true,
                  "hidden": false, "excludedFromScript": false, "attributes": []
                },
                {
                  "id": "0000000000000004", "canonicalIdentity": "type:plTestMode",
                  "nativeName": "plTestMode", "pluginName": "Test", "parentNativeName": "",
                  "scriptExtensionName": "", "flags": 2, "variantType": 0,
                  "typeVersion": 1, "typeSize": 2, "enumStorageTypeName": "plInt16", "canAllocate": false,
                  "hidden": false, "excludedFromScript": false, "attributes": []
                },
                {
                  "id": "0000000000000005", "canonicalIdentity": "type:plTestFlags",
                  "nativeName": "plTestFlags", "pluginName": "Test", "parentNativeName": "",
                  "scriptExtensionName": "", "flags": 4, "variantType": 0,
                  "typeVersion": 1, "typeSize": 4, "enumStorageTypeName": "plUInt32", "canAllocate": false,
                  "hidden": false, "excludedFromScript": false, "attributes": []
                }
              ],
              "properties": [
                {
                  "id": "0000000000000010", "canonicalIdentity": "property:name",
                  "declaringTypeId": "0000000000000001", "declaringTypeName": "plGameObject",
                  "nativeName": "Name", "category": 1, "flags": 1,
                  "valueTypeId": "0000000000000000", "valueTypeName": "plString",
                  "hasDefaultValue": false, "attributes": []
                },
                {
                  "id": "0000000000000014", "canonicalIdentity": "property:segments",
                  "declaringTypeId": "0000000000000001", "declaringTypeName": "plGameObject",
                  "nativeName": "Segments", "category": 1, "flags": 1,
                  "valueTypeId": "0000000000000000", "valueTypeName": "plVec2U32",
                  "hasDefaultValue": false, "attributes": []
                },
                {
                  "id": "0000000000000015", "canonicalIdentity": "property:unsupported",
                  "declaringTypeId": "0000000000000001", "declaringTypeName": "plGameObject",
                  "nativeName": "Unsupported", "category": 1, "flags": 1,
                  "valueTypeId": "0000000000000000", "valueTypeName": "plUnsupportedValue",
                  "hasDefaultValue": false, "attributes": []
                },
                {
                  "id": "0000000000000011", "canonicalIdentity": "property:enabled",
                  "declaringTypeId": "0000000000000003", "declaringTypeName": "plMsgBeacon",
                  "nativeName": "Enabled", "category": 1, "flags": 1,
                  "valueTypeId": "0000000000000000", "valueTypeName": "bool",
                  "hasDefaultValue": false, "attributes": []
                },
                {
                  "id": "0000000000000012", "canonicalIdentity": "property:test-mode",
                  "declaringTypeId": "0000000000000004", "declaringTypeName": "plTestMode",
                  "nativeName": "plTestMode_Enabled", "category": 0, "flags": 0,
                  "valueTypeId": "0000000000000004", "valueTypeName": "plTestMode",
                  "hasDefaultValue": true, "defaultValue": 1, "attributes": []
                },
                {
                  "id": "0000000000000013", "canonicalIdentity": "property:test-flags",
                  "declaringTypeId": "0000000000000005", "declaringTypeName": "plTestFlags",
                  "nativeName": "plTestFlags_Visible", "category": 0, "flags": 0,
                  "valueTypeId": "0000000000000005", "valueTypeName": "plTestFlags",
                  "hasDefaultValue": true, "defaultValue": 1, "attributes": []
                }
              ],
              "functions": [
                {
                  "id": "0000000000000020", "canonicalIdentity": "function:set-active",
                  "declaringTypeId": "0000000000000001", "declaringTypeName": "plGameObject",
                  "nativeName": "SetActiveFlag", "publicName": "SetActiveFlag",
                  "functionType": 0, "flags": 0, "returnTypeId": "0000000000000000",
                  "returnTypeName": "void", "returnFlags": 0, "isBaseClassFunction": false,
                  "parameters": [
                    {
                      "index": 0, "name": "active", "direction": "in",
                      "typeId": "0000000000000000", "typeName": "bool", "flags": 1,
                      "hasDefaultValue": false, "isDynamicPin": false, "attributes": []
                    }
                  ],
                  "attributes": []
                },
                {
                  "id": "0000000000000021", "canonicalIdentity": "function:log",
                  "declaringTypeId": "0000000000000002",
                  "declaringTypeName": "plScriptExtensionClass_Log",
                  "nativeName": "Info", "publicName": "Info", "functionType": 1, "flags": 0,
                  "returnTypeId": "0000000000000000", "returnTypeName": "void", "returnFlags": 0,
                  "isBaseClassFunction": false,
                  "parameters": [
                    {
                      "index": 0, "name": "text", "direction": "in",
                      "typeId": "0000000000000000", "typeName": "plString", "flags": 1,
                      "hasDefaultValue": false, "isDynamicPin": false, "attributes": []
                    }
                  ],
                  "attributes": []
                }
              ],
              "messages": [
                {
                  "id": "0000000000000030", "canonicalIdentity": "message:beacon",
                  "typeId": "0000000000000003", "typeName": "plMsgBeacon",
                  "parentTypeId": "0000000000000000", "parentTypeName": "plMessage",
                  "properties": ["0000000000000011"],
                  "writableProperties": ["0000000000000011"]
                }
              ],
              "diagnostics": []
            }
            """;

        CSharpCompilation compilation = CreateCompilation(string.Empty);
        var additionalText = new InMemoryAdditionalText("PlasmaBindings.json", manifest);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(
            new[] { new BindingManifestGenerator().AsSourceGenerator() },
            new AdditionalText[] { additionalText },
            (CSharpParseOptions)compilation.SyntaxTrees.Single().Options);

        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out ImmutableArray<Diagnostic> generatorDiagnostics);

        Assert.DoesNotContain(
            generatorDiagnostics,
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
        Assert.Contains(
            generatorDiagnostics,
            static diagnostic =>
                diagnostic.Id == "PLB002" &&
                diagnostic.Severity == DiagnosticSeverity.Info);
        Assert.DoesNotContain(
            generated.GetDiagnostics(),
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);

        string generatedSource = string.Join(
            "\n",
            driver.GetRunResult().GeneratedTrees.Select(static tree => tree.GetText().ToString()));
        Assert.Contains("public enum @TestMode : short", generatedSource, StringComparison.Ordinal);
        Assert.Contains("[global::System.Flags]", generatedSource, StringComparison.Ordinal);
        Assert.Contains("public enum @TestFlags : uint", generatedSource, StringComparison.Ordinal);
        Assert.Contains("@Enabled = 1", generatedSource, StringComparison.Ordinal);
        Assert.Contains("@Visible = 1", generatedSource, StringComparison.Ordinal);
        Assert.Contains(
            "public static partial class @LogBindings",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "global::Plasma.Vec2U32",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "\"GameObject.SetActiveFlag\"",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "\"GameObject.Name getter\"",
            generatedSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void SiblingGeneratorsResolveManifestGeneratedMessages()
    {
        const string source =
            """
            using Plasma;

            [PlasmaScript(Id = "06fcd70a-bd88-43d4-bb22-e090d80ab75a")]
            public sealed partial class BeaconScript : ComponentScript
            {
                [MessageHandler]
                private void OnBeacon(in MsgBeacon message) { }
            }
            """;
        const string manifest =
            """
            {
              "manifestVersion": 1,
              "canonicalIdentityVersion": 1,
              "schemaHash": "0000000000000001",
              "types": [
                {
                  "id": "0000000000000001", "canonicalIdentity": "type:plMsgBeacon",
                  "nativeName": "plMsgBeacon", "pluginName": "Test",
                  "parentNativeName": "plMessage", "scriptExtensionName": "",
                  "flags": 8, "variantType": 0, "typeVersion": 1, "typeSize": 1,
                  "canAllocate": true, "hidden": false, "excludedFromScript": false,
                  "attributes": []
                }
              ],
              "properties": [],
              "functions": [],
              "messages": [
                {
                  "id": "0000000000000002", "canonicalIdentity": "message:plMsgBeacon",
                  "typeId": "0000000000000001", "typeName": "plMsgBeacon",
                  "parentTypeId": "0000000000000000", "parentTypeName": "plMessage",
                  "properties": [], "writableProperties": []
                }
              ],
              "diagnostics": []
            }
            """;

        CSharpCompilation compilation = CreateCompilation(source);
        var additionalText = new InMemoryAdditionalText("PlasmaBindings.json", manifest);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(
            new ISourceGenerator[]
            {
                new BindingManifestGenerator().AsSourceGenerator(),
                new ScriptDescriptorGenerator().AsSourceGenerator(),
            },
            new AdditionalText[] { additionalText },
            (CSharpParseOptions)compilation.SyntaxTrees.Single().Options);

        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out ImmutableArray<Diagnostic> generatorDiagnostics);

        Assert.DoesNotContain(
            generatorDiagnostics,
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
        Assert.DoesNotContain(
            generated.GetDiagnostics(),
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
        string generatedSource = string.Join(
            "\n",
            driver.GetRunResult().GeneratedTrees.Select(static tree => tree.GetText().ToString()));
        Assert.Contains(
            "global::Plasma.MsgBeacon",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains("\"plMsgBeacon\"", generatedSource, StringComparison.Ordinal);
    }

    [Fact]
    public void BindingDefaultsAndSignatureOnlyHandlesAreCompilable()
    {
        const string manifest =
            """
            {
              "manifestVersion": 1,
              "canonicalIdentityVersion": 1,
              "schemaHash": "0000000000000001",
              "types": [
                {
                  "id": "0000000000000001", "canonicalIdentity": "type:plComponent",
                  "nativeName": "plComponent", "pluginName": "Core",
                  "parentNativeName": "", "scriptExtensionName": "", "flags": 8,
                  "variantType": 0, "typeVersion": 1, "typeSize": 8,
                  "canAllocate": false, "hidden": false, "excludedFromScript": false,
                  "attributes": []
                },
                {
                  "id": "0000000000000002", "canonicalIdentity": "type:plSignatureComponent",
                  "nativeName": "plSignatureComponent", "pluginName": "Test",
                  "parentNativeName": "plComponent", "scriptExtensionName": "", "flags": 8,
                  "variantType": 0, "typeVersion": 1, "typeSize": 8,
                  "canAllocate": false, "hidden": false, "excludedFromScript": false,
                  "attributes": []
                },
                {
                  "id": "0000000000000003", "canonicalIdentity": "type:plScriptExtensionClass_Test",
                  "nativeName": "plScriptExtensionClass_Test", "pluginName": "Test",
                  "parentNativeName": "", "scriptExtensionName": "Test", "flags": 8,
                  "variantType": 0, "typeVersion": 1, "typeSize": 1,
                  "canAllocate": false, "hidden": false, "excludedFromScript": false,
                  "attributes": []
                },
                {
                  "id": "0000000000000004", "canonicalIdentity": "type:plTestMode",
                  "nativeName": "plTestMode", "pluginName": "Test",
                  "parentNativeName": "", "scriptExtensionName": "", "flags": 2,
                  "variantType": 0, "typeVersion": 1, "typeSize": 8,
                  "enumStorageTypeName": "plInt64",
                  "canAllocate": false, "hidden": false, "excludedFromScript": false,
                  "attributes": []
                },
                {
                  "id": "0000000000000005", "canonicalIdentity": "type:plTestFlags",
                  "nativeName": "plTestFlags", "pluginName": "Test",
                  "parentNativeName": "", "scriptExtensionName": "", "flags": 4,
                  "variantType": 0, "typeVersion": 1, "typeSize": 8,
                  "enumStorageTypeName": "plUInt64",
                  "canAllocate": false, "hidden": false, "excludedFromScript": false,
                  "attributes": []
                }
              ],
              "properties": [],
              "functions": [
                {
                  "id": "0000000000000010", "canonicalIdentity": "function:defaults",
                  "declaringTypeId": "0000000000000003",
                  "declaringTypeName": "plScriptExtensionClass_Test",
                  "nativeName": "Defaults", "publicName": "Defaults", "functionType": 1,
                  "flags": 0, "returnTypeId": "0000000000000000",
                  "returnTypeName": "void", "returnFlags": 0, "isBaseClassFunction": false,
                  "parameters": [
                    { "index": 0, "name": "singleValue", "direction": "in", "typeId": "0000000000000000", "typeName": "float", "flags": 1, "hasDefaultValue": true, "defaultValue": 0.1, "isDynamicPin": false, "attributes": [] },
                    { "index": 1, "name": "doubleValue", "direction": "in", "typeId": "0000000000000000", "typeName": "double", "flags": 1, "hasDefaultValue": true, "defaultValue": 0.2, "isDynamicPin": false, "attributes": [] },
                    { "index": 2, "name": "uintValue", "direction": "in", "typeId": "0000000000000000", "typeName": "plUInt32", "flags": 1, "hasDefaultValue": true, "defaultValue": 3, "isDynamicPin": false, "attributes": [] },
                    { "index": 3, "name": "longValue", "direction": "in", "typeId": "0000000000000000", "typeName": "plInt64", "flags": 1, "hasDefaultValue": true, "defaultValue": 4, "isDynamicPin": false, "attributes": [] },
                    { "index": 4, "name": "ulongValue", "direction": "in", "typeId": "0000000000000000", "typeName": "plUInt64", "flags": 1, "hasDefaultValue": true, "defaultValue": 5, "isDynamicPin": false, "attributes": [] },
                    { "index": 5, "name": "mode", "direction": "in", "typeId": "0000000000000004", "typeName": "plTestMode", "flags": 1, "hasDefaultValue": true, "defaultValue": -1, "isDynamicPin": false, "attributes": [] },
                    { "index": 6, "name": "flags", "direction": "in", "typeId": "0000000000000005", "typeName": "plTestFlags", "flags": 1, "hasDefaultValue": true, "defaultValue": 6, "isDynamicPin": false, "attributes": [] },
                    { "index": 7, "name": "text", "direction": "in", "typeId": "0000000000000000", "typeName": "plString", "flags": 1, "hasDefaultValue": true, "defaultValue": "quoted", "isDynamicPin": false, "attributes": [] }
                  ],
                  "attributes": []
                },
                {
                  "id": "0000000000000011", "canonicalIdentity": "function:signature",
                  "declaringTypeId": "0000000000000003",
                  "declaringTypeName": "plScriptExtensionClass_Test",
                  "nativeName": "Signature", "publicName": "Signature", "functionType": 1,
                  "flags": 0, "returnTypeId": "0000000000000002",
                  "returnTypeName": "plSignatureComponent", "returnFlags": 1,
                  "isBaseClassFunction": false,
                  "parameters": [
                    { "index": 0, "name": "component", "direction": "in", "typeId": "0000000000000002", "typeName": "plSignatureComponent", "flags": 1, "hasDefaultValue": false, "isDynamicPin": false, "attributes": [] },
                    { "index": 1, "name": "hash", "direction": "in", "typeId": "0000000000000000", "typeName": "plTempHashedString", "flags": 1, "hasDefaultValue": false, "isDynamicPin": false, "attributes": [] }
                  ],
                  "attributes": []
                }
              ],
              "messages": [],
              "diagnostics": []
            }
            """;

        CSharpCompilation compilation = CreateCompilation(string.Empty);
        GeneratorDriver driver = CreateBindingDriver(compilation, manifest);
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation,
            out Compilation generated,
            out ImmutableArray<Diagnostic> diagnostics);

        Assert.DoesNotContain(
            diagnostics.Concat(generated.GetDiagnostics()),
            static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
        string generatedSource = Assert.Single(driver.GetRunResult().GeneratedTrees)
            .GetText()
            .ToString();
        Assert.Contains("float @singleValue = 0.1F", generatedSource, StringComparison.Ordinal);
        Assert.Contains("double @doubleValue = 0.2D", generatedSource, StringComparison.Ordinal);
        Assert.Contains("uint @uintValue = 3U", generatedSource, StringComparison.Ordinal);
        Assert.Contains("long @longValue = 4L", generatedSource, StringComparison.Ordinal);
        Assert.Contains("ulong @ulongValue = 5UL", generatedSource, StringComparison.Ordinal);
        Assert.Contains(
            "global::Plasma.@TestMode @mode = (global::Plasma.@TestMode)(-1L)",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "global::Plasma.@TestFlags @flags = (global::Plasma.@TestFlags)(6UL)",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains("string @text = \"quoted\"", generatedSource, StringComparison.Ordinal);
        Assert.Contains(
            "record struct @SignatureComponent",
            generatedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "string @hash",
            generatedSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void BindingManifestErrorsAbortGeneration()
    {
        const string manifest =
            """
            {
              "manifestVersion": 1,
              "canonicalIdentityVersion": 1,
              "schemaHash": "0000000000000001",
              "types": [],
              "properties": [],
              "functions": [],
              "messages": [],
              "diagnostics": [
                {
                  "severity": "error",
                  "code": "invalidDefaultValue",
                  "typeName": "plExample",
                  "memberName": "Run",
                  "parameterName": "count",
                  "message": "The default value cannot be converted."
                }
              ]
            }
            """;

        CSharpCompilation compilation = CreateCompilation(string.Empty);
        GeneratorDriver driver = CreateBindingDriver(compilation, manifest);
        driver = driver.RunGenerators(compilation);

        Diagnostic diagnostic = Assert.Single(
            driver.GetRunResult().Diagnostics,
            static diagnostic => diagnostic.Id == "PLB003");
        Assert.Contains("plExample.Run(count)", diagnostic.GetMessage(), StringComparison.Ordinal);
        Assert.Empty(driver.GetRunResult().GeneratedTrees);
    }

    private static GeneratorDriver CreateBindingDriver(
        CSharpCompilation compilation,
        string manifest)
    {
        var additionalText = new InMemoryAdditionalText("PlasmaBindings.json", manifest);
        return CSharpGeneratorDriver.Create(
            new[] { new BindingManifestGenerator().AsSourceGenerator() },
            new AdditionalText[] { additionalText },
            (CSharpParseOptions)compilation.SyntaxTrees.Single().Options);
    }

    private static string RunGenerator(string source)
    {
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ScriptDescriptorGenerator());
        driver = driver.RunGenerators(CreateCompilation(source));
        return Assert.Single(driver.GetRunResult().GeneratedTrees).GetText().ToString();
    }

    private static CSharpCompilation CreateCompilation(string source)
    {
        string[] trustedAssemblies =
            ((string?)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES"))!
            .Split(Path.PathSeparator);

        IEnumerable<MetadataReference> references = trustedAssemblies
            .Select(static path => MetadataReference.CreateFromFile(path))
            .Append(MetadataReference.CreateFromFile(typeof(ComponentScript).Assembly.Location));

        return CSharpCompilation.Create(
            "GeneratorTest",
            new[] { CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(LanguageVersion.Latest)) },
            references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
    }

    private sealed class InMemoryAdditionalText(string path, string content) : AdditionalText
    {
        public override string Path { get; } = path;
        public override Microsoft.CodeAnalysis.Text.SourceText GetText(
            CancellationToken cancellationToken = default) =>
            Microsoft.CodeAnalysis.Text.SourceText.From(content);
    }
}
