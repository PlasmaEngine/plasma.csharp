namespace Plasma;

public sealed class PlasmaException : Exception
{
    public PlasmaException(string message, int status = -1)
        : base(message)
    {
        Status = status;
    }

    public int Status { get; }
}
