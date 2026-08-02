using Plasma.Generated;
using Plasma.ScriptTestGame;

namespace Plasma.ManagedTests;

public sealed class GeneratedDescriptorTests
{
    [Fact]
    public void DescriptorUsesStableIdentityAndDirectDelegates()
    {
        var provider = new PlasmaGeneratedScriptDescriptorProvider();
        ScriptTypeDescriptor descriptor = Assert.Single(provider.GetDescriptors());

        Assert.Equal(
            StableId.ScriptType("Plasma.ScriptTestGame.RotatingBeacon"),
            descriptor.Id);
        Assert.Equal(
            StableId.ScriptPersistentId(
                "Plasma.ScriptTestGame",
                "Plasma.ScriptTestGame.RotatingBeacon"),
            descriptor.PersistentId);
        Assert.Equal(
            new Guid("3fe42b1d-70ab-5b3b-8597-2a8f2c877328"),
            descriptor.PersistentId);
        Assert.Equal("RotatingBeacon.cs", descriptor.SourceFile);
        Assert.Equal(typeof(RotatingBeacon), descriptor.ManagedType);
        Assert.Equal(
            new[]
            {
                ScriptLifecycleMethod.OnSimulationStarted,
                ScriptLifecycleMethod.Update,
            },
            descriptor.Lifecycle.Keys.OrderBy(static method => method));

        var instance = Assert.IsType<RotatingBeacon>(descriptor.Create());
        instance.__Attach(new ScriptOwnerContext(
            new NativeObject(1, 1, NativeObjectKind.GameObject),
            new NativeObject(2, 1, NativeObjectKind.World),
            new NativeObject(3, 1, NativeObjectKind.Component)));

        ScriptFieldDescriptor speed = descriptor.Fields[
            StableId.ExposedField(descriptor.ManagedName, nameof(RotatingBeacon.DegreesPerSecond))];
        Assert.Equal(90.0f, speed.Get(instance));
        speed.Set(instance, 180.0f);
        Assert.Equal(180.0f, instance.DegreesPerSecond);
        Assert.Equal("0", speed.EditorMetadata["Range.Minimum"]);
        Assert.Equal("720", speed.EditorMetadata["Range.Maximum"]);
        Assert.Equal("Motion", speed.EditorMetadata["Category"]);

        descriptor.Lifecycle[ScriptLifecycleMethod.OnSimulationStarted](instance, default);
        descriptor.Lifecycle[ScriptLifecycleMethod.Update](instance, Time.FromSeconds(0.25));
        Assert.Equal(1, instance.SimulationStartCount);
        Assert.Equal(0.25, instance.LastUpdate.Seconds);
    }

    [Fact]
    public void PrivateInMessageHandlerUsesGeneratedBridge()
    {
        ScriptTypeDescriptor descriptor =
            Assert.Single(new PlasmaGeneratedScriptDescriptorProvider().GetDescriptors());
        var instance = Assert.IsType<RotatingBeacon>(descriptor.Create());
        Assert.Equal(BeaconDirection.Reverse, instance.Direction);
        ScriptMessageDescriptor message = descriptor.Messages[
            StableId.MessageHandler(
                descriptor.ManagedName,
                "OnBeacon",
                "plMsgBeacon")];

        message.Invoke(instance, new object?[] { new MsgBeacon { Enabled = false } });

        Assert.False(instance.Enabled);
    }

    [Fact]
    public void CustomMessageUsesAutomaticIdentityAndPreservesMutations()
    {
        ScriptTypeDescriptor descriptor =
            Assert.Single(new PlasmaGeneratedScriptDescriptorProvider().GetDescriptors());
        var instance = Assert.IsType<RotatingBeacon>(descriptor.Create());
        ulong messageId = StableId.CustomMessage(typeof(SetBeaconSpeed));
        ScriptMessageDescriptor descriptorMessage = descriptor.Messages[messageId];
        var message = new SetBeaconSpeed { DegreesPerSecond = 240.0f };

        ulong token = ManagedMessageRegistry.Register(message);
        try
        {
            object resolved = ManagedMessageRegistry.Resolve(token, typeof(SetBeaconSpeed));
            descriptorMessage.Invoke(instance, new[] { resolved });
        }
        finally
        {
            ManagedMessageRegistry.Release(token);
        }

        Assert.Equal(240.0f, instance.DegreesPerSecond);
        Assert.True(message.Consumed);
        Assert.Equal("plMsgDeliverCSharpMsg", descriptorMessage.NativeTypeName);
    }
}
