namespace Lapis.Contracts.Media;

public enum MediaLocatorKind
{
    File,
    Directory,
    Uri,
    OpticalDisc
}

public sealed record MediaLocator
{
    public MediaLocator(MediaLocatorKind kind, string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new ArgumentException("A media locator value is required.", nameof(value));
        }

        Kind = kind;
        Value = value;
    }

    public MediaLocatorKind Kind { get; }

    public string Value { get; }
}
