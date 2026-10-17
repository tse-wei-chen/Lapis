namespace Lapis.Contracts.Errors;

public enum ErrorSeverity
{
    Info,
    Warning,
    Error,
    Fatal
}

public enum ErrorRecoverability
{
    Recoverable,
    Retryable,
    FallbackAvailable,
    Fatal
}

public sealed record PlayerError(
    string Domain,
    string Code,
    ErrorSeverity Severity,
    ErrorRecoverability Recoverability,
    string? Context = null,
    string? NativeError = null);
