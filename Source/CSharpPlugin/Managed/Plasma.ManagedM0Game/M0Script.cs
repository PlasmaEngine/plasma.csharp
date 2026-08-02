using Plasma;

namespace Plasma.ManagedM0Game;

[PlasmaScript]
public sealed partial class M0Script : ComponentScript
{
    [Expose]
    public long Counter { get; set; } = 5;

    public override void Initialize()
    {
        Counter = CSharpM0.Probe(Counter);
    }

    public override void Update(Time deltaTime)
    {
        Counter += (long)deltaTime.Seconds;
    }

    [MessageHandler]
    private void OnM0Message(in MsgCSharpM0 message)
    {
        Counter += message.Amount;
    }
}
