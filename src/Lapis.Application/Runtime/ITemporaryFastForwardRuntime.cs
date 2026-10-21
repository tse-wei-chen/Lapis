namespace Lapis.Application.Runtime;

/// <summary>Optional temporary 2x fast-forward support.</summary>
public interface ITemporaryFastForwardRuntime
{
    bool SupportsTemporaryFastForward { get; }

    ValueTask SetTemporaryFastForwardAsync(
        bool enabled,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);
}
