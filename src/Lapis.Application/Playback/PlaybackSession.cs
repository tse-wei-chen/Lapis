using System.Runtime.CompilerServices;
using System.Threading.Channels;
using Lapis.Application.Runtime;
using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Errors;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;

namespace Lapis.Application.Playback;

/// <summary>
/// Single logical owner for playback commands. All command mutations are
/// serialized through a bounded mailbox.
/// </summary>
public sealed class PlaybackSession : IAsyncDisposable
{
    private const int CommandCapacity = 64;
    private const int SnapshotCapacity = 32;

    private readonly IPlaybackRuntime _runtime;
    private readonly Channel<CommandEnvelope> _commands;
    private readonly Channel<PlaybackSnapshot> _snapshots;
    private readonly Task _processingLoop;
    private PlaybackEpoch _epoch = PlaybackEpoch.Initial;
    private PlaybackSnapshot _snapshot;
    private bool _disposed;

    public PlaybackSession(IPlaybackRuntime runtime, PlaybackSessionId? sessionId = null)
    {
        _runtime = runtime ?? throw new ArgumentNullException(nameof(runtime));
        SessionId = sessionId ?? PlaybackSessionId.New();
        _snapshot = PlaybackSnapshot.CreateInitial(SessionId);

        _commands = Channel.CreateBounded<CommandEnvelope>(new BoundedChannelOptions(CommandCapacity)
        {
            SingleReader = true,
            SingleWriter = false,
            FullMode = BoundedChannelFullMode.Wait
        });

        _snapshots = Channel.CreateBounded<PlaybackSnapshot>(new BoundedChannelOptions(SnapshotCapacity)
        {
            SingleReader = false,
            SingleWriter = true,
            FullMode = BoundedChannelFullMode.DropOldest
        });

        _snapshots.Writer.TryWrite(_snapshot);
        _processingLoop = Task.Run(ProcessCommandsAsync);
    }

    public PlaybackSessionId SessionId { get; }

    public PlaybackSnapshot Snapshot => Volatile.Read(ref _snapshot);

    public async ValueTask ExecuteAsync(
        PlaybackCommand command,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(command);

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var envelope = new CommandEnvelope(command, cancellationToken, completion);

        await _commands.Writer.WriteAsync(envelope, cancellationToken).ConfigureAwait(false);
        await completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public async IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        await foreach (var snapshot in _snapshots.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            yield return snapshot;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _commands.Writer.TryComplete();
        await _processingLoop.ConfigureAwait(false);
        _snapshots.Writer.TryComplete();
        await _runtime.DisposeAsync().ConfigureAwait(false);
    }

    private async Task ProcessCommandsAsync()
    {
        await foreach (var envelope in _commands.Reader.ReadAllAsync().ConfigureAwait(false))
        {
            try
            {
                await HandleCommandAsync(envelope.Command, envelope.CancellationToken).ConfigureAwait(false);
                envelope.Completion.TrySetResult();
            }
            catch (OperationCanceledException) when (envelope.CancellationToken.IsCancellationRequested)
            {
                envelope.Completion.TrySetCanceled(envelope.CancellationToken);
            }
            catch (InvalidOperationException exception)
            {
                envelope.Completion.TrySetException(exception);
            }
            catch (Exception exception)
            {
                Publish(_snapshot with
                {
                    Transport = TransportState.Faulted,
                    LastError = new PlayerError(
                        "Playback",
                        "RuntimeFailure",
                        ErrorSeverity.Error,
                        ErrorRecoverability.Retryable,
                        exception.Message,
                        exception.GetType().FullName)
                });

                envelope.Completion.TrySetException(exception);
            }
        }
    }

    private async ValueTask HandleCommandAsync(
        PlaybackCommand command,
        CancellationToken cancellationToken)
    {
        switch (command)
        {
            case OpenMediaCommand open:
                await HandleOpenAsync(open, cancellationToken).ConfigureAwait(false);
                break;
            case PlayCommand:
                await HandlePlayAsync(cancellationToken).ConfigureAwait(false);
                break;
            case PauseCommand:
                await HandlePauseAsync(cancellationToken).ConfigureAwait(false);
                break;
            case SeekCommand seek:
                await HandleSeekAsync(seek, cancellationToken).ConfigureAwait(false);
                break;
            case CloseCommand:
                await HandleCloseAsync(cancellationToken).ConfigureAwait(false);
                break;
            default:
                throw new InvalidOperationException($"Unsupported command type: {command.GetType().Name}.");
        }
    }

    private async ValueTask HandleOpenAsync(
        OpenMediaCommand command,
        CancellationToken cancellationToken)
    {
        _epoch = _epoch.Next();
        Publish(_snapshot with
        {
            Epoch = _epoch,
            Position = MediaTime.Zero,
            Transport = TransportState.Opening,
            Capabilities = MediaCapability.None,
            LastError = null
        });

        var result = await _runtime.OpenAsync(
            command.Locator,
            Context(),
            cancellationToken).ConfigureAwait(false);

        Publish(_snapshot with
        {
            Transport = TransportState.Ready,
            Capabilities = result.Capabilities,
            LastError = null
        });
    }

    private async ValueTask HandlePlayAsync(CancellationToken cancellationToken)
    {
        EnsureTransport(TransportState.Ready, TransportState.Paused);
        await _runtime.PlayAsync(Context(), cancellationToken).ConfigureAwait(false);
        Publish(_snapshot with { Transport = TransportState.Playing, LastError = null });
    }

    private async ValueTask HandlePauseAsync(CancellationToken cancellationToken)
    {
        EnsureTransport(TransportState.Playing);
        await _runtime.PauseAsync(Context(), cancellationToken).ConfigureAwait(false);
        Publish(_snapshot with { Transport = TransportState.Paused, LastError = null });
    }

    private async ValueTask HandleSeekAsync(
        SeekCommand command,
        CancellationToken cancellationToken)
    {
        EnsureTransport(TransportState.Ready, TransportState.Playing, TransportState.Paused);

        var returnState = _snapshot.Transport;
        _epoch = _epoch.Next();
        Publish(_snapshot with { Epoch = _epoch, Transport = TransportState.Seeking, LastError = null });

        await _runtime.SeekAsync(command.Position, Context(), cancellationToken).ConfigureAwait(false);

        Publish(_snapshot with
        {
            Position = command.Position,
            Transport = returnState,
            LastError = null
        });
    }

    private async ValueTask HandleCloseAsync(CancellationToken cancellationToken)
    {
        _epoch = _epoch.Next();
        await _runtime.CloseAsync(Context(), cancellationToken).ConfigureAwait(false);
        Publish(PlaybackSnapshot.CreateInitial(SessionId) with { Epoch = _epoch });
    }

    private RuntimeCommandContext Context() => new(SessionId, _epoch);

    private void EnsureTransport(params TransportState[] allowed)
    {
        if (!allowed.Contains(_snapshot.Transport))
        {
            throw new InvalidOperationException(
                $"Command is invalid while transport state is {_snapshot.Transport}.");
        }
    }

    private void Publish(PlaybackSnapshot snapshot)
    {
        Volatile.Write(ref _snapshot, snapshot);
        _snapshots.Writer.TryWrite(snapshot);
    }

    private sealed record CommandEnvelope(
        PlaybackCommand Command,
        CancellationToken CancellationToken,
        TaskCompletionSource Completion);
}
