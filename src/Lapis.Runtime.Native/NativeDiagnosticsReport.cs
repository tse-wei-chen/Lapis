namespace Lapis.Runtime.Native;

/// <summary>
/// Portable no-path diagnostics event. MonotonicMilliseconds is a process clock
/// timestamp, not wall-clock UTC. Numeric kind/stage match diagnostics ABI v1.
/// </summary>
public sealed record NativeDiagnosticsEvent(
    ulong Sequence,
    ulong Epoch,
    ulong MonotonicMilliseconds,
    uint Kind,
    uint FailureStage,
    string Hresult,
    string RemovedReason,
    ulong Detail);

public sealed record NativeDiagnosticsReport(
    IReadOnlyList<NativeDiagnosticsEvent> Events,
    ulong OverwrittenEvents);
