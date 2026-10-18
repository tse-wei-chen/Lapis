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
    private readonly CancellationTokenSource _shutdown = new();
    private readonly object _disposeGate = new();
    private readonly Task _processingLoop;
    private readonly Task? _observationLoop;
    private static readonly TimeSpan ObservationPeriod = TimeSpan.FromMilliseconds(150);
    private PlaybackEpoch _epoch = PlaybackEpoch.Initial;
    private PlaybackSnapshot _snapshot;
    private Task? _disposeTask;
    private int _lifecycleState; // 0 = active, 1 = disposing, 2 = disposed

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
        if (runtime is IPlaybackRuntimeObserver observer)
        {
            _observationLoop = Task.Run(() => ObserveRuntimeAsync(observer));
        }
    }

    public PlaybackSessionId SessionId { get; }

    public PlaybackSnapshot Snapshot => Volatile.Read(ref _snapshot);

    public async ValueTask ExecuteAsync(
        PlaybackCommand command,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(command);
        ThrowIfStopping();

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var envelope = new CommandEnvelope(command, cancellationToken, completion);

        try
        {
            await _commands.Writer.WriteAsync(envelope, cancellationToken).ConfigureAwait(false);
        }
        catch (ChannelClosedException)
        {
            throw new ObjectDisposedException(nameof(PlaybackSession));
        }

        // Caller cancellation after enqueue cancels only this wait. If the actor has
        // already started the command, the runtime operation continues against the
        // session shutdown token so transient states cannot be stranded by a UI token.
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

    public ValueTask DisposeAsync()
    {
        Task disposeTask;
        lock (_disposeGate)
        {
            _disposeTask ??= DisposeCoreAsync();
            disposeTask = _disposeTask;
        }

        return new ValueTask(disposeTask);
    }

    private async Task DisposeCoreAsync()
    {
        if (Interlocked.CompareExchange(ref _lifecycleState, 1, 0) != 0)
        {
            return;
        }

        _commands.Writer.TryComplete();
        _shutdown.Cancel();

        Exception? disposeError = null;
        try
        {
            await _processingLoop.ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            disposeError = exception;
        }

        // Do not destroy the native runtime until every observation has
        // returned. The same adapter gate protects snapshots and native handles.
        if (_observationLoop is not null)
        {
            try
            {
                await _observationLoop.ConfigureAwait(false);
            }
            catch (Exception exception) when (disposeError is null)
            {
                disposeError = exception;
            }
        }

        Publish(PlaybackSnapshot.CreateInitial(SessionId) with { Epoch = _epoch });
        _snapshots.Writer.TryComplete();

        try
        {
            await _runtime.DisposeAsync().ConfigureAwait(false);
        }
        catch (Exception exception) when (disposeError is null)
        {
            disposeError = exception;
        }
        finally
        {
            Volatile.Write(ref _lifecycleState, 2);
            _shutdown.Dispose();
        }

        if (disposeError is not null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(disposeError).Throw();
        }
    }

    private async Task ObserveRuntimeAsync(IPlaybackRuntimeObserver observer)
    {
        using var timer = new PeriodicTimer(ObservationPeriod);
        try
        {
            while (await timer.WaitForNextTickAsync(_shutdown.Token).ConfigureAwait(false))
            {
                var snapshot = Snapshot;
                if (snapshot.Transport is not (TransportState.Playing or TransportState.Paused))
                {
                    continue;
                }

                try
                {
                    // Some native adapters expose synchronous P/Invoke under a
                    // ValueTask API. Always execute that read off the UI thread,
                    // and never enqueue an unbounded backlog of observations.
                    var observed = await Task.Run(async () =>
                        await observer.ReadSnapshotAsync(SessionId, _shutdown.Token)
                            .ConfigureAwait(false), _shutdown.Token).ConfigureAwait(false);
                    _commands.Writer.TryWrite(new CommandEnvelope(
                        new RuntimeObservationCommand(observed),
                        _shutdown.Token,
                        new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)));
                }
                catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
                {
                    break;
                }
                catch (Exception exception)
                {
                    _commands.Writer.TryWrite(new CommandEnvelope(
                        new RuntimeObservationFailureCommand(snapshot.Epoch, exception),
                        _shutdown.Token,
                        new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)));
                    // Do not spin on a permanently failing P/Invoke snapshot.
                    break;
                }
            }
        }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
        {
        }
    }

    private async Task ProcessCommandsAsync()
    {
        await foreach (var envelope in _commands.Reader.ReadAllAsync().ConfigureAwait(false))
        {
            if (_shutdown.IsCancellationRequested)
            {
                envelope.Completion.TrySetException(new ObjectDisposedException(nameof(PlaybackSession)));
                continue;
            }

            // This check is the command start boundary. Request cancellation observed
            // before this point prevents execution; after this point it only cancels
            // the caller's wait and cannot interrupt runtime state mutation.
            if (envelope.RequestCancellation.IsCancellationRequested)
            {
                envelope.Completion.TrySetCanceled(envelope.RequestCancellation);
                continue;
            }

            try
            {
                await HandleCommandAsync(envelope.Command, _shutdown.Token).ConfigureAwait(false);
                envelope.Completion.TrySetResult();
            }
            catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
            {
                envelope.Completion.TrySetException(new ObjectDisposedException(nameof(PlaybackSession)));
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
            case RuntimeObservationCommand observation:
                ReconcileObservation(observation.Observation);
                break;
            case RuntimeObservationFailureCommand observationFailure:
                HandleObservationFailure(observationFailure);
                break;
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

    private void ReconcileObservation(RuntimePlaybackObservation observed)
    {
        var current = _snapshot;
        if (observed.SessionId != SessionId ||
            observed.Epoch != current.Epoch ||
            observed.Position.Nanoseconds < 0 ||
            current.Transport is not (TransportState.Playing or TransportState.Paused))
        {
            return;
        }

        // An in-flight read from before Pause/Resume can complete later with
        // the same epoch. It must never undo the actor's newer command state.
        if (observed.Transport != current.Transport &&
            observed.Transport is not (TransportState.Ended or TransportState.Faulted))
        {
            return;
        }

        var next = current with
        {
            Transport = observed.Transport,
            Position = observed.Position,
            LastError = observed.Transport == TransportState.Faulted
                ? current.LastError ?? new PlayerError(
                    "Playback", "NativeFaulted", ErrorSeverity.Error,
                    ErrorRecoverability.Retryable,
                    "Native playback execution entered a faulted state.")
                : current.LastError
        };
        if (next != current)
        {
            Publish(next);
        }
    }

    private void HandleObservationFailure(RuntimeObservationFailureCommand failure)
    {
        if (failure.Epoch != _snapshot.Epoch ||
            _snapshot.Transport is not (TransportState.Playing or TransportState.Paused))
        {
            return;
        }
        Publish(_snapshot with
        {
            Transport = TransportState.Faulted,
            LastError = new PlayerError(
                "Playback", "ObservationFailure", ErrorSeverity.Error,
                ErrorRecoverability.Retryable, failure.Error.Message,
                failure.Error.GetType().FullName)
        });
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
            Duration = null,
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
            Duration = result.Duration,
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

    private void ThrowIfStopping()
    {
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref _lifecycleState) != 0,
            this);
    }

    private void Publish(PlaybackSnapshot snapshot)
    {
        Volatile.Write(ref _snapshot, snapshot);
        _snapshots.Writer.TryWrite(snapshot);
    }

    private sealed record RuntimeObservationCommand(
        RuntimePlaybackObservation Observation) : PlaybackCommand;

    private sealed record RuntimeObservationFailureCommand(
        PlaybackEpoch Epoch, Exception Error) : PlaybackCommand;

    private sealed record CommandEnvelope(
        PlaybackCommand Command,
        CancellationToken RequestCancellation,
        TaskCompletionSource Completion);
}
