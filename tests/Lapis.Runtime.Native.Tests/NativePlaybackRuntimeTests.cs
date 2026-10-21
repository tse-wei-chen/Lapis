using System.Runtime.InteropServices;
using Lapis.Application.Runtime;
using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;
using Lapis.Runtime.Native.Interop;
using Xunit;

namespace Lapis.Runtime.Native.Tests;

public sealed class NativePlaybackRuntimeTests
{
    [Fact]
    public void ConstructorRejectsAbiMismatchBeforeCreatingRuntime()
    {
        var api = new FakeNativeApi { AbiVersion = 99 };

        var exception = Assert.Throws<NotSupportedException>(
            () => new NativePlaybackRuntime(api));

        Assert.Contains("ABI 99", exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("runtime_create", api.Calls);
    }

    [Fact]
    public async Task FileOnlyRuntimeRejectsDirectoryWithoutCreatingSession()
    {
        var api = new FakeNativeApi { SourceFeatures = 1 };
        await using var runtime = new NativePlaybackRuntime(api);
        Assert.False(runtime.SupportsBdmvDirectory);
        var error = await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.OpenAsync(
                new MediaLocator(MediaLocatorKind.Directory, @"C:\Disc"),
                new RuntimeCommandContext(new PlaybackSessionId(Guid.NewGuid()),
                    new PlaybackEpoch(1)), CancellationToken.None));
        Assert.Contains("build-bdmv-player.ps1", error.Message);
        Assert.DoesNotContain("session_open", api.Calls);
        Assert.Equal(0, api.SessionCreateCount);
    }

    [Fact]
    public async Task BdmvEnabledRuntimeStillDelegatesValidationToNative()
    {
        var api = new FakeNativeApi { SourceFeatures = 3 };
        await using var runtime = new NativePlaybackRuntime(api);
        Assert.True(runtime.SupportsBdmvDirectory);
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.Directory, @"C:\Disc"),
            new RuntimeCommandContext(new PlaybackSessionId(Guid.NewGuid()),
                new PlaybackEpoch(1)), CancellationToken.None);
        Assert.Equal(NativeMediaLocatorKind.Directory, api.LastLocatorKind);
    }

    [Fact]
    public async Task OlderRuntimeWithoutFeatureQueryRemainsCompatible()
    {
        var api = new FakeNativeApi { HasFeatureQuery = false };
        await using var runtime = new NativePlaybackRuntime(api);
        Assert.Null(runtime.SupportsBdmvDirectory);
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.Directory, @"C:\Disc"),
            new RuntimeCommandContext(new PlaybackSessionId(Guid.NewGuid()),
                new PlaybackEpoch(1)), CancellationToken.None);
        Assert.Contains("session_open", api.Calls);
    }

    [Fact]
    public async Task UnsupportedBdmvOpenProvidesBuildGuidance()
    {
        var api = new FakeNativeApi { SessionOpenResult = NativeResult.Unsupported };
        await using var runtime = new NativePlaybackRuntime(api);
        var error = await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.OpenAsync(
                new MediaLocator(MediaLocatorKind.Directory, @"C:\Disc"),
                new RuntimeCommandContext(
                    new PlaybackSessionId(Guid.NewGuid()), new PlaybackEpoch(1)),
                CancellationToken.None));
        Assert.Contains("libbluray", error.Message);
        Assert.Contains("build-bdmv-player.ps1", error.Message);
        Assert.Contains("AACS/BD+", error.Message);
        Assert.Equal(NativeMediaLocatorKind.Directory, api.LastLocatorKind);
    }

    [Fact]
    public async Task UnsupportedFileOpenPreservesNativeOperationError()
    {
        var api = new FakeNativeApi { SessionOpenResult = NativeResult.Unsupported };
        await using var runtime = new NativePlaybackRuntime(api);
        var error = await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.OpenAsync(
                new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
                new RuntimeCommandContext(
                    new PlaybackSessionId(Guid.NewGuid()), new PlaybackEpoch(1)),
                CancellationToken.None));
        Assert.Contains("session_open", error.Message);
        Assert.DoesNotContain("build-bdmv-player.ps1", error.Message);
    }

    [Fact]
    public async Task CommandsMapContractsAndDisposeSessionBeforeRuntime()
    {
        var api = new FakeNativeApi
        {
            OpenResult = new NativeOpenResult
            {
                Capabilities = (ulong)(MediaCapability.Chapters | MediaCapability.HdrOutput),
                Duration = new NativeMediaTime { Nanoseconds = 9_500_000_000 },
                HasDuration = 1
            }
        };
        var sessionId = new PlaybackSessionId(
            Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"));
        var runtime = new NativePlaybackRuntime(api);

        var opened = await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.m2ts"),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);

        Assert.Equal(
            MediaCapability.Chapters | MediaCapability.HdrOutput,
            opened.Capabilities);
        Assert.Equal(new MediaTime(9_500_000_000), opened.Duration);
        Assert.Equal(sessionId.Value.ToByteArray(), api.LastSessionIdBytes);
        Assert.Equal(NativeMediaLocatorKind.File, api.LastLocatorKind);
        Assert.Equal("movie.m2ts", api.LastLocatorValue);
        Assert.Equal((ulong)1, api.LastEpoch.Value);
        Assert.Equal(1, api.SessionCreateCount);

        await runtime.PlayAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);
        await runtime.PauseAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);
        await runtime.SeekAsync(
            new MediaTime(3_000_000_000),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(2)),
            CancellationToken.None);
        Assert.Equal(3_000_000_000, api.LastPosition.Nanoseconds);
        Assert.Equal((ulong)2, api.LastEpoch.Value);

        await runtime.CloseAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(3)),
            CancellationToken.None);
        Assert.Equal((ulong)3, api.LastEpoch.Value);

        await runtime.DisposeAsync();
        await runtime.DisposeAsync();

        Assert.Equal(1, api.SessionDestroyCount);
        Assert.Equal(1, api.RuntimeDestroyCount);
        Assert.Equal(
            new[] { "session_destroy", "runtime_destroy" },
            api.Calls.TakeLast(2));
    }

    [Fact]
    public async Task ChapterLookupIsReadOnlyEpochScopedAndRejectsUnsupportedVersions()
    {
        var api = new FakeNativeApi
        {
            NavigationStart = new NativeMediaTime { Nanoseconds = 8_000_000_000 }
        };
        await using var runtime = new NativePlaybackRuntime(api);
        var navigation = (IChapterNavigationRuntime)runtime;
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await navigation.ResolveChapterAsync(4, context, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.Directory, "BDMV"),
            context, CancellationToken.None);
        Assert.Equal(MediaTime.FromSeconds(8),
            await navigation.ResolveChapterAsync(4, context, CancellationToken.None));
        Assert.Contains("session_resolve_chapter", api.Calls);
        Assert.DoesNotContain("session_seek", api.Calls);
        Assert.Equal(4U, api.LastChapterId);
        Assert.Equal(1UL, api.LastEpoch.Value);

        api.ChapterResult = NativeResult.InvalidArgument;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await navigation.ResolveChapterAsync(999, context, CancellationToken.None));
        api.ChapterResult = NativeResult.Unsupported;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await navigation.ResolveChapterAsync(4, context, CancellationToken.None));
        api.ChapterResult = NativeResult.Ok;
        api.NavigationVersion = 99;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await navigation.ResolveChapterAsync(4, context, CancellationToken.None));
    }

    [Fact]
    public async Task RuntimeBindsToExactlyOnePlaybackSessionId()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var first = new PlaybackSessionId(Guid.NewGuid());
        var second = new PlaybackSessionId(Guid.NewGuid());

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "first.mkv"),
            new RuntimeCommandContext(first, new PlaybackEpoch(1)),
            CancellationToken.None);

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.PlayAsync(
                new RuntimeCommandContext(second, new PlaybackEpoch(1)),
                CancellationToken.None));

        Assert.Equal(1, api.SessionCreateCount);
    }

    [Fact]
    public async Task NativeStateAndEpochFailuresBecomeInvalidOperationExceptions()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var sessionId = new PlaybackSessionId(Guid.NewGuid());

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.PlayResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.PlayAsync(
                new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
                CancellationToken.None));

        api.PlayResult = NativeResult.InvalidState;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.PlayAsync(
                new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
                CancellationToken.None));
    }

    [Fact]
    public async Task CancellationBeforeInteropDoesNotInvokeNativeCommand()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var context = new RuntimeCommandContext(
            new PlaybackSessionId(Guid.NewGuid()),
            new PlaybackEpoch(1));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.PlayAsync(context, cancellation.Token));

        Assert.Equal(0, api.SessionCreateCount);
        Assert.DoesNotContain("session_play", api.Calls);
    }

    [Fact]
    public void VideoOutputSurfaceAbiUsesNativePointerAlignment()
    {
        Assert.Equal(2u, NativeAbi.Version);
        Assert.Equal(typeof(int), Enum.GetUnderlyingType(typeof(NativeVideoOutputSurfaceKind)));
        Assert.Equal(IntPtr.Size == 8 ? 8L : 4L,
            Marshal.OffsetOf<NativeVideoOutputSurface>(
                nameof(NativeVideoOutputSurface.NativeHandle)).ToInt64());
        Assert.Equal(IntPtr.Size == 8 ? 16 : 8,
            Marshal.SizeOf<NativeVideoOutputSurface>());
    }

    [Fact]
    public async Task OutputBindingUsesOwnPortPreservesEpochAndSupportsDetach()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        IVideoOutputSurfaceBinding bindingPort = runtime;
        var sessionId = new PlaybackSessionId(Guid.NewGuid());
        var initial = new RuntimeCommandContext(sessionId, new PlaybackEpoch(0));
        var hwnd = (nint)0x123456;
        var state = await bindingPort.SetVideoOutputSurfaceAsync(
            VideoOutputSurface.ForWin32Window(hwnd),
            initial,
            CancellationToken.None);

        Assert.Equal(VideoOutputSurfaceBindingState.Deferred, state);
        Assert.Equal(NativeVideoOutputSurfaceKind.Win32Window, api.LastSurface.Kind);
        Assert.Equal((nuint)hwnd, api.LastSurface.NativeHandle);
        Assert.Equal((ulong)0, api.LastEpoch.Value);
        Assert.Equal(1, api.SessionCreateCount);
        Assert.Contains("session_set_video_output_surface", api.Calls);

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.SurfaceBinding = NativeVideoOutputSurfaceBinding.Applied;
        state = await runtime.ClearVideoOutputSurfaceAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);
        Assert.Equal(VideoOutputSurfaceBindingState.Applied, state);
        Assert.Equal(NativeVideoOutputSurfaceKind.None, api.LastSurface.Kind);
        Assert.Equal((nuint)0, api.LastSurface.NativeHandle);
        Assert.Equal((ulong)1, api.LastEpoch.Value);
        Assert.Equal(1, api.SessionCreateCount);
    }

    [Fact]
    public async Task OutputBindingValidatesBeforeSessionCreationAndMapsFailures()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var sessionId = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(sessionId, new PlaybackEpoch(0));

        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                new VideoOutputSurface(VideoOutputSurfaceKind.Win32Window, 0),
                context, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                new VideoOutputSurface(VideoOutputSurfaceKind.None, (nint)1),
                context, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                new VideoOutputSurface((VideoOutputSurfaceKind)99, (nint)1),
                context, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        api.SurfaceResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
        api.SurfaceResult = NativeResult.Unsupported;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
        api.SurfaceResult = NativeResult.OutputUnavailable;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
        api.SurfaceResult = NativeResult.Ok;
        api.SurfaceBinding = (NativeVideoOutputSurfaceBinding)99;
        await Assert.ThrowsAsync<NativeMediaRuntimeException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
    }

    [Fact]
    public async Task OutputBindingCancellationDoesNotEnterNative()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var context = new RuntimeCommandContext(
            new PlaybackSessionId(Guid.NewGuid()), new PlaybackEpoch(0));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                VideoOutputSurface.ForWin32Window((nint)111),
                context, cancellation.Token));
        Assert.Equal(0, api.SessionCreateCount);
        Assert.DoesNotContain("session_set_video_output_surface", api.Calls);
    }

    [Fact]
    public void DiagnosticsWireLayoutIsStable()
    {
        Assert.Equal(1u, NativeAbi.DiagnosticsVersion);
        Assert.Equal(48, Marshal.SizeOf<NativeDiagnosticEvent>());
        Assert.Equal(40L, Marshal.OffsetOf<NativeDiagnosticEvent>(
            nameof(NativeDiagnosticEvent.Detail)).ToInt64());
    }

    [Fact]
    public async Task DiagnosticsExportReadsOneBoundedSessionJournal()
    {
        var api = new FakeNativeApi
        {
            DiagnosticEvents =
            [
                new NativeDiagnosticEvent
                {
                    Sequence = 42,
                    Epoch = 7,
                    Kind = 1,
                    FailureStage = 10,
                    Hresult = 0x887A0005,
                    RemovedReason = 0x887A0006
                }
            ],
            DiagnosticDropped = 10
        };
        await using var runtime = new NativePlaybackRuntime(api);
        Assert.Empty(runtime.CaptureDiagnostics().Events);
        Assert.Equal(0, api.SessionCreateCount);

        var id = new PlaybackSessionId(Guid.NewGuid());
        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "secret-file-path.mkv"),
            new RuntimeCommandContext(id, new PlaybackEpoch(1)),
            CancellationToken.None);

        var report = runtime.CaptureDiagnostics();
        Assert.Equal((ulong)10, report.OverwrittenEvents);
        var diagnostic = Assert.Single(report.Events);
        Assert.Equal((ulong)42, diagnostic.Sequence);
        Assert.Equal("0x887A0005", diagnostic.Hresult);
        Assert.Equal("0x887A0006", diagnostic.RemovedReason);
        Assert.DoesNotContain("secret-file-path", report.ToString());
        Assert.Contains("session_read_diagnostic_events", api.Calls);
    }

    [Fact]
    public async Task UnsupportedDiagnosticsVersionIsRejectedWithoutRead()
    {
        var api = new FakeNativeApi { DiagnosticVersion = 44 };
        await using var runtime = new NativePlaybackRuntime(api);
        Assert.Throws<NotSupportedException>(() => runtime.CaptureDiagnostics());
        Assert.DoesNotContain("session_read_diagnostic_events", api.Calls);
    }

    [Fact]
    public void NativeSnapshotLayoutMirrorsCAbi()
    {
        Assert.Equal(40, Marshal.SizeOf<NativePlaybackSnapshot>());
        Assert.Equal(16L,
            Marshal.OffsetOf<NativePlaybackSnapshot>(
                nameof(NativePlaybackSnapshot.Epoch)).ToInt64());
        Assert.Equal(24L,
            Marshal.OffsetOf<NativePlaybackSnapshot>(
                nameof(NativePlaybackSnapshot.Position)).ToInt64());
        Assert.Equal(32L,
            Marshal.OffsetOf<NativePlaybackSnapshot>(
                nameof(NativePlaybackSnapshot.Transport)).ToInt64());
    }

    [Fact]
    public async Task ObservationUsesExistingSessionAndPreservesNativeEpochAndPosition()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        IPlaybackRuntimeObserver observer = runtime;
        var id = new PlaybackSessionId(
            Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await observer.ReadSnapshotAsync(id, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(id, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.Snapshot = new NativePlaybackSnapshot
        {
            SessionId = NativePlaybackSessionId.From(id),
            Epoch = new NativePlaybackEpoch { Value = 1 },
            Position = new NativeMediaTime { Nanoseconds = 1_250_000_000 },
            Transport = NativeTransportState.Ended
        };
        var observed = await observer.ReadSnapshotAsync(id, CancellationToken.None);
        Assert.Equal(id, observed.SessionId);
        Assert.Equal(new PlaybackEpoch(1), observed.Epoch);
        Assert.Equal(new MediaTime(1_250_000_000), observed.Position);
        Assert.Equal(TransportState.Ended, observed.Transport);
        Assert.Equal(1, api.SessionCreateCount);
        Assert.Contains("session_get_snapshot", api.Calls);

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await observer.ReadSnapshotAsync(
                new PlaybackSessionId(Guid.NewGuid()), CancellationToken.None));
    }

    [Fact]
    public async Task ObservationRejectsInvalidNativePayloadAndCancellation()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(id, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.Snapshot = new NativePlaybackSnapshot
        {
            SessionId = NativePlaybackSessionId.From(id),
            Epoch = new NativePlaybackEpoch { Value = 1 },
            Position = new NativeMediaTime { Nanoseconds = -1 },
            Transport = NativeTransportState.Playing
        };
        await Assert.ThrowsAsync<NativeMediaRuntimeException>(async () =>
            await runtime.ReadSnapshotAsync(id, CancellationToken.None));

        var malformed = api.Snapshot;
        malformed.Position.Nanoseconds = 5;
        malformed.Transport = (NativeTransportState)99;
        api.Snapshot = malformed;
        await Assert.ThrowsAsync<NativeMediaRuntimeException>(async () =>
            await runtime.ReadSnapshotAsync(id, CancellationToken.None));

        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        var calls = api.Calls.Count(x => x == "session_get_snapshot");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.ReadSnapshotAsync(id, canceled.Token));
        Assert.Equal(calls, api.Calls.Count(x => x == "session_get_snapshot"));
    }

    [Fact]
    public void NativeSubtitleDescriptorIsExactlyTheNative72ByteAbi()
    {
        Assert.Equal(72, Marshal.SizeOf<NativeSubtitleTrackInfo>());
        Assert.Equal(0L, Marshal.OffsetOf<NativeSubtitleTrackInfo>(
            nameof(NativeSubtitleTrackInfo.StreamIndex)).ToInt64());
        Assert.Equal(4L, Marshal.OffsetOf<NativeSubtitleTrackInfo>(
            nameof(NativeSubtitleTrackInfo.Kind)).ToInt64());
        Assert.Equal(8L, Marshal.OffsetOf<NativeSubtitleTrackInfo>(
            "LanguageUtf8").ToInt64());
        Assert.Equal(32L, Marshal.OffsetOf<NativeSubtitleTrackInfo>(
            "CodecNameUtf8").ToInt64());
    }

    [Fact]
    public async Task EmbeddedSubtitleInventoryIsOwnedAndSourceQualifiedMetadata()
    {
        var api = new FakeNativeApi
        {
            SubtitleTracks =
            [
                MakeSubtitle(2, NativeSubtitleTrackKind.Text, "jpn", "subrip"),
                MakeSubtitle(5, NativeSubtitleTrackKind.Bitmap, "zho",
                    "hdmv_pgs_subtitle"),
                MakeSubtitle(7, NativeSubtitleTrackKind.Unknown, "", "unknown")
            ]
        };
        await using var runtime = new NativePlaybackRuntime(api);
        IEmbeddedSubtitleTrackInventoryRuntime reader = runtime;
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await reader.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);

        var tracks = await reader.ReadEmbeddedSubtitleTracksAsync(
            context, CancellationToken.None);
        Assert.Equal(3, tracks.Count);
        Assert.Equal(new EmbeddedSubtitleTrack(2, EmbeddedSubtitleTrackFormat.Text,
            "jpn", "subrip"), tracks[0]);
        Assert.Equal(new EmbeddedSubtitleTrack(5, EmbeddedSubtitleTrackFormat.Bitmap,
            "zho", "hdmv_pgs_subtitle"), tracks[1]);
        Assert.Equal(EmbeddedSubtitleTrackFormat.Unknown, tracks[2].Format);
        Assert.Equal(1, api.SessionCreateCount);
        Assert.Equal(2, api.SubtitleReadCount); // probe + caller-owned buffer
        Assert.DoesNotContain("session_seek", api.Calls);
        Assert.Equal(1UL, api.LastEpoch.Value);
    }

    [Fact]
    public async Task OlderOrIncompatibleNativeSubtitleApisFailGracefully()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);

        api.HasSubtitleVersionExport = false;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));
        Assert.Equal(0, api.SubtitleReadCount);

        api.HasSubtitleVersionExport = true;
        api.SubtitleVersion = 99;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));
        Assert.Equal(0, api.SubtitleReadCount);

        api.SubtitleVersion = NativeAbi.SubtitleTracksVersion;
        api.HasSubtitleQueryExport = false;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));
    }

    [Fact]
    public async Task EmbeddedSubtitleReadHandlesEmptyListAndEpochErrors()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);

        var tracks = await runtime.ReadEmbeddedSubtitleTracksAsync(
            context, CancellationToken.None);
        Assert.Empty(tracks);
        Assert.Equal(1, api.SubtitleReadCount); // no unnecessary second call

        api.SubtitleQueryResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));
        api.SubtitleQueryResult = NativeResult.InvalidState;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));

        api.SubtitleQueryResult = NativeResult.Ok;
        api.SubtitleCountOverride = 65;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));

        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        var calls = api.SubtitleReadCount;
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, canceled.Token));
        Assert.Equal(calls, api.SubtitleReadCount);
    }

    [Fact]
    public async Task NativeSubtitleInventoryRejectsMalformedDescriptorAndBufferCount()
    {
        var bad = MakeSubtitle(3, NativeSubtitleTrackKind.Text, "eng", "ass");
        unsafe
        {
            // The native protocol requires NUL termination of every field.
            for (var i = 0; i < 24; ++i) bad.LanguageUtf8[i] = (byte)'x';
        }
        var api = new FakeNativeApi { SubtitleTracks = [bad] };
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));

        api.SubtitleTracks = [MakeSubtitle(1, NativeSubtitleTrackKind.Text, "eng", "ass")];
        api.SubtitleCountOverride = null;
        api.SubtitleReadCountOverride = 2; // More than the probed buffer size.
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedSubtitleTracksAsync(context, CancellationToken.None));
    }

    private static unsafe NativeSubtitleTrackInfo MakeSubtitle(
        uint index, NativeSubtitleTrackKind kind, string language, string codec)
    {
        var track = new NativeSubtitleTrackInfo { StreamIndex = index, Kind = kind };
        var languageBytes = System.Text.Encoding.UTF8.GetBytes(language);
        var codecBytes = System.Text.Encoding.UTF8.GetBytes(codec);
        for (var i = 0; i < languageBytes.Length && i < 23; ++i)
            track.LanguageUtf8[i] = languageBytes[i];
        for (var i = 0; i < codecBytes.Length && i < 39; ++i)
            track.CodecNameUtf8[i] = codecBytes[i];
        return track;
    }

    [Fact]
    public void NativeTextCueRecordLayoutMatchesCAbi()
    {
        Assert.Equal(24, Marshal.SizeOf<NativeTextSubtitleCueInfo>());
        Assert.Equal(0L, Marshal.OffsetOf<NativeTextSubtitleCueInfo>(
            nameof(NativeTextSubtitleCueInfo.StartNanoseconds)).ToInt64());
        Assert.Equal(8L, Marshal.OffsetOf<NativeTextSubtitleCueInfo>(
            nameof(NativeTextSubtitleCueInfo.EndNanoseconds)).ToInt64());
        Assert.Equal(16L, Marshal.OffsetOf<NativeTextSubtitleCueInfo>(
            nameof(NativeTextSubtitleCueInfo.TextOffset)).ToInt64());
        Assert.Equal(20L, Marshal.OffsetOf<NativeTextSubtitleCueInfo>(
            nameof(NativeTextSubtitleCueInfo.TextLength)).ToInt64());
        Assert.Equal(16, NativeAbi.TextSubtitlePageCapacity);
        Assert.Equal(65_536, NativeAbi.TextSubtitlePageBytes);
    }

    [Fact]
    public async Task EmbeddedTextCuePortDoesNotCreateSessionAndHonorsCancellation()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        IEmbeddedTextSubtitleCueRuntime reader = runtime;
        var context = new RuntimeCommandContext(
            new PlaybackSessionId(Guid.NewGuid()), new PlaybackEpoch(1));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await reader.StartEmbeddedTextSubtitleLoadAsync(
                2, context, CancellationToken.None));
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await reader.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await reader.StartEmbeddedTextSubtitleLoadAsync(
                2, context, cancelled.Token));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await reader.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, cancelled.Token));
        Assert.Equal(0, api.TextCueStartCalls);
        Assert.Equal(0, api.TextCueReadCalls);
    }

    [Fact]
    public async Task EmbeddedCueReaderMapsUnicodeBytesAndOriginalNanosecondTimes()
    {
        var subtitleText = "字幕 Hello 🌸";
        var data = System.Text.Encoding.UTF8.GetBytes(subtitleText);
        var api = new FakeNativeApi
        {
            TextCueDescriptors =
            [
                new NativeTextSubtitleCueInfo
                {
                    StartNanoseconds = 1_123_456_789,
                    EndNanoseconds = 3_987_654_321,
                    TextOffset = 0,
                    TextLength = checked((uint)data.Length)
                }
            ],
            TextCueUtf8 = data,
            TextCueTotal = 1
        };
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(3));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);
        await runtime.StartEmbeddedTextSubtitleLoadAsync(5, context, CancellationToken.None);

        IEmbeddedTextSubtitleCueRuntime reader = runtime;
        var page = await reader.ReadEmbeddedTextSubtitleCuesAsync(
            0, context, CancellationToken.None);
        Assert.Equal(EmbeddedTextSubtitleLoadState.Ready, page.State);
        Assert.Equal(1U, page.Total);
        Assert.Equal(new EmbeddedTextSubtitleCue(
            1_123_456_789, 3_987_654_321, subtitleText), Assert.Single(page.Cues));
        Assert.Equal(5U, api.TextCueSelectedStreamIndex);
        Assert.Equal(3UL, api.LastEpoch.Value);
        Assert.Contains("session_start_text_subtitle_load", api.Calls);
        Assert.Contains("session_read_text_subtitle_cues", api.Calls);
        Assert.DoesNotContain("session_seek", api.Calls);
    }

    [Fact]
    public async Task CuePollingStatesDoNotPublishIncompleteOrFailedText()
    {
        var api = new FakeNativeApi { TextCueState = NativeTextSubtitleLoadState.Loading };
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);
        var states = new[]
        {
            (NativeTextSubtitleLoadState.Loading, EmbeddedTextSubtitleLoadState.Loading),
            (NativeTextSubtitleLoadState.Failed, EmbeddedTextSubtitleLoadState.Failed),
            (NativeTextSubtitleLoadState.Cancelled, EmbeddedTextSubtitleLoadState.Cancelled),
            (NativeTextSubtitleLoadState.LimitExceeded, EmbeddedTextSubtitleLoadState.LimitExceeded)
        };
        foreach (var (native, expected) in states)
        {
            api.TextCueState = native;
            var page = await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None);
            Assert.Equal(expected, page.State);
            Assert.Empty(page.Cues);
            Assert.Equal(0U, page.Total);
        }
    }

    [Fact]
    public async Task IncompatibleAndMissingCueExportsDoNotBreakBaseRuntime()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);

        api.HasTextCueVersionExport = false;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.StartEmbeddedTextSubtitleLoadAsync(
                1, context, CancellationToken.None));
        Assert.Equal(0, api.TextCueStartCalls);

        api.HasTextCueVersionExport = true;
        api.TextCueVersion = 99;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));
        Assert.Equal(0, api.TextCueReadCalls);

        api.TextCueVersion = NativeAbi.TextSubtitleCuesVersion;
        api.HasTextCueStartExport = false;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.StartEmbeddedTextSubtitleLoadAsync(
                1, context, CancellationToken.None));

        api.HasTextCueReadExport = false;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));
        Assert.Equal(1, api.SessionCreateCount);
    }

    [Fact]
    public async Task CueReaderRejectsMaliciousCountsOffsetsUtf8AndTiming()
    {
        var api = new FakeNativeApi
        {
            TextCueDescriptors =
            [
                new NativeTextSubtitleCueInfo
                {
                    StartNanoseconds = 1, EndNanoseconds = 2,
                    TextOffset = 0, TextLength = 1
                }
            ],
            TextCueUtf8 = [(byte)'a'],
            TextCueTotal = 1
        };
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(1));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);

        api.TextCueCountOverride = 17;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueCountOverride = null;
        api.TextCueTotal = 4097;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueTotal = 1;
        api.TextCueDescriptors[0].TextOffset = 5;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueDescriptors[0].TextOffset = 0;
        api.TextCueDescriptors[0].EndNanoseconds = 1;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueDescriptors[0].EndNanoseconds = 2;
        api.TextCueUtf8 = [0xFF];
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueUtf8 = [(byte)'a'];
        api.TextCueState = (NativeTextSubtitleLoadState)999;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));
    }

    [Fact]
    public async Task CueErrorsPropagateEpochAndStateWithoutPublishingText()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(id, new PlaybackEpoch(3));
        await runtime.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            context, CancellationToken.None);

        api.TextCueStartResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.StartEmbeddedTextSubtitleLoadAsync(
                1, context, CancellationToken.None));

        api.TextCueStartResult = NativeResult.Ok;
        api.TextCueReadResult = NativeResult.InvalidState;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueReadResult = NativeResult.BufferTooSmall;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));

        api.TextCueReadResult = NativeResult.Ok;
        await runtime.CloseAsync(
            new RuntimeCommandContext(id, new PlaybackEpoch(4)),
            CancellationToken.None);
        api.TextCueReadResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.ReadEmbeddedTextSubtitleCuesAsync(
                0, context, CancellationToken.None));
    }

    private sealed class FakeNativeApi : INativeMediaRuntimeApi
    {
        internal uint AbiVersion { get; set; } = NativeAbi.Version;
        internal uint SourceFeatures { get; set; } = 3;
        internal bool HasFeatureQuery { get; set; } = true;
        internal uint DiagnosticVersion { get; set; } = NativeAbi.DiagnosticsVersion;
        internal uint NavigationVersion { get; set; } = NativeAbi.NavigationVersion;
        internal uint SubtitleVersion { get; set; } = NativeAbi.SubtitleTracksVersion;
        internal uint TextCueVersion { get; set; } = NativeAbi.TextSubtitleCuesVersion;
        internal bool HasTextCueVersionExport { get; set; } = true;
        internal bool HasTextCueStartExport { get; set; } = true;
        internal bool HasTextCueReadExport { get; set; } = true;
        internal NativeResult TextCueStartResult { get; set; } = NativeResult.Ok;
        internal NativeResult TextCueReadResult { get; set; } = NativeResult.Ok;
        internal NativeTextSubtitleLoadState TextCueState { get; set; } = NativeTextSubtitleLoadState.Ready;
        internal NativeTextSubtitleCueInfo[] TextCueDescriptors { get; set; } = [];
        internal byte[] TextCueUtf8 { get; set; } = [];
        internal uint TextCueTotal { get; set; }
        internal uint? TextCueCountOverride { get; set; }
        internal uint TextCueSelectedStreamIndex { get; private set; }
        internal int TextCueStartCalls { get; private set; }
        internal int TextCueReadCalls { get; private set; }

        internal bool HasSubtitleVersionExport { get; set; } = true;
        internal bool HasSubtitleQueryExport { get; set; } = true;
        internal NativeSubtitleTrackInfo[] SubtitleTracks { get; set; } = [];
        internal NativeResult SubtitleQueryResult { get; set; } = NativeResult.Ok;
        internal uint? SubtitleCountOverride { get; set; }
        internal uint? SubtitleReadCountOverride { get; set; }
        internal int SubtitleReadCount { get; private set; }

        internal NativeResult ChapterResult { get; set; } = NativeResult.Ok;
        internal NativeMediaTime NavigationStart { get; set; }
        internal uint LastChapterId { get; private set; }
        internal NativeDiagnosticEvent[] DiagnosticEvents { get; set; } = [];
        internal ulong DiagnosticDropped { get; set; } = 0;
        internal NativeOpenResult OpenResult { get; set; }
        internal NativeResult SessionOpenResult { get; set; } = NativeResult.Ok;
        internal NativePlaybackSnapshot Snapshot { get; set; }
        internal NativeResult PlayResult { get; set; } = NativeResult.Ok;
        internal NativeResult SurfaceResult { get; set; } = NativeResult.Ok;
        internal NativeVideoOutputSurfaceBinding SurfaceBinding { get; set; } = NativeVideoOutputSurfaceBinding.Deferred;
        internal NativeVideoOutputSurface LastSurface { get; private set; }
        internal List<string> Calls { get; } = new();
        internal int SessionCreateCount { get; private set; }
        internal int SessionDestroyCount { get; private set; }
        internal int RuntimeDestroyCount { get; private set; }
        internal byte[]? LastSessionIdBytes { get; private set; }
        internal NativeMediaLocatorKind LastLocatorKind { get; private set; }
        internal string? LastLocatorValue { get; private set; }
        internal NativePlaybackEpoch LastEpoch { get; private set; }
        internal NativeMediaTime LastPosition { get; private set; }

        public uint GetAbiVersion() => AbiVersion;
        public uint GetSourceFeatures() =>
            HasFeatureQuery ? SourceFeatures :
                throw new EntryPointNotFoundException("Feature query unsupported");
        public uint GetDiagnosticsApiVersion() => DiagnosticVersion;
        public uint GetNavigationApiVersion() => NavigationVersion;
        public uint GetTextSubtitleCuesApiVersion() =>
            HasTextCueVersionExport ? TextCueVersion :
                throw new EntryPointNotFoundException("Optional cue version absent");

        public NativeResult SessionStartTextSubtitleLoad(
            nint session, NativePlaybackEpoch expectedEpoch, uint streamIndex)
        {
            if (!HasTextCueStartExport)
                throw new EntryPointNotFoundException("Optional cue start absent");
            Assert.Equal((nint)22, session);
            Calls.Add("session_start_text_subtitle_load");
            TextCueStartCalls++;
            LastEpoch = expectedEpoch;
            TextCueSelectedStreamIndex = streamIndex;
            return TextCueStartResult;
        }

        public NativeResult SessionReadTextSubtitleCues(
            nint session, NativePlaybackEpoch expectedEpoch, uint startIndex,
            NativeTextSubtitleCueInfo[] cues, byte[] utf8,
            out uint count, out uint total, out uint requiredTextBytes,
            out NativeTextSubtitleLoadState state)
        {
            if (!HasTextCueReadExport)
                throw new EntryPointNotFoundException("Optional cue read absent");
            Assert.Equal((nint)22, session);
            Assert.True(cues.Length <= NativeAbi.TextSubtitlePageCapacity);
            Assert.True(utf8.Length <= NativeAbi.TextSubtitlePageBytes);
            Calls.Add("session_read_text_subtitle_cues");
            TextCueReadCalls++;
            LastEpoch = expectedEpoch;
            total = TextCueTotal;
            state = TextCueState;
            count = TextCueCountOverride ?? checked((uint)TextCueDescriptors.Length);
            requiredTextBytes = checked((uint)TextCueUtf8.Length);
            if (TextCueReadResult != NativeResult.Ok)
                return TextCueReadResult;
            // Caller buffer owns all bytes after P/Invoke returns.
            Array.Copy(TextCueDescriptors, cues,
                Math.Min(TextCueDescriptors.Length, cues.Length));
            Array.Copy(TextCueUtf8, utf8,
                Math.Min(TextCueUtf8.Length, utf8.Length));
            return NativeResult.Ok;
        }

        public uint GetSubtitleTracksApiVersion() =>
            HasSubtitleVersionExport ? SubtitleVersion :
            throw new EntryPointNotFoundException("Subtitle API absent");

        public NativeResult SessionListSubtitleTracks(
            nint session, NativePlaybackEpoch expectedEpoch,
            NativeSubtitleTrackInfo[]? buffer, out uint count)
        {
            if (!HasSubtitleQueryExport)
                throw new EntryPointNotFoundException("Subtitle query absent");
            Assert.Equal((nint)22, session);
            Calls.Add("session_list_subtitle_tracks");
            SubtitleReadCount++;
            LastEpoch = expectedEpoch;
            count = (buffer is null ? SubtitleCountOverride : SubtitleReadCountOverride)
                ?? checked((uint)SubtitleTracks.Length);
            if (SubtitleQueryResult != NativeResult.Ok)
                return SubtitleQueryResult;
            if (buffer != null)
            {
                Array.Copy(SubtitleTracks, buffer,
                    Math.Min(SubtitleTracks.Length, buffer.Length));
            }
            return NativeResult.Ok;
        }

        public NativeResult SessionResolveChapter(
            nint session, uint chapterId, NativePlaybackEpoch expectedEpoch,
            out NativeMediaTime start)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_resolve_chapter");
            LastEpoch = expectedEpoch;
            LastChapterId = chapterId;
            start = NavigationStart;
            return ChapterResult;
        }

        public NativeResult SessionReadDiagnosticEvents(
            nint session, ulong sinceSequence, NativeDiagnosticEvent[] events,
            out uint count, out ulong nextSequence, out ulong dropped)
        {
            Assert.Equal((nint)22, session);
            Assert.Equal((ulong)0, sinceSequence);
            Calls.Add("session_read_diagnostic_events");
            count = checked((uint)DiagnosticEvents.Length);
            Assert.True(count <= events.Length);
            Array.Copy(DiagnosticEvents, events, DiagnosticEvents.Length);
            nextSequence = count == 0 ? sinceSequence : events[count - 1].Sequence;
            dropped = DiagnosticDropped;
            return NativeResult.Ok;
        }

        public NativeResult RuntimeCreate(out nint runtime)
        {
            Calls.Add("runtime_create");
            runtime = 11;
            return NativeResult.Ok;
        }

        public void RuntimeDestroy(nint runtime)
        {
            Assert.Equal((nint)11, runtime);
            Calls.Add("runtime_destroy");
            RuntimeDestroyCount++;
        }

        public NativeResult SessionCreate(
            nint runtime,
            NativePlaybackSessionId sessionId,
            out nint session)
        {
            Assert.Equal((nint)11, runtime);
            LastSessionIdBytes = sessionId.ToByteArray();
            Assert.Contains(LastSessionIdBytes, static value => value != 0);
            Calls.Add("session_create");
            SessionCreateCount++;
            session = 22;
            return NativeResult.Ok;
        }

        public void SessionDestroy(nint session)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_destroy");
            SessionDestroyCount++;
        }

        public NativeResult SessionOpen(
            nint session,
            NativeMediaLocatorKind kind,
            string value,
            NativePlaybackEpoch epoch,
            out NativeOpenResult result)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_open");
            LastLocatorKind = kind;
            LastLocatorValue = value;
            LastEpoch = epoch;
            result = OpenResult;
            return SessionOpenResult;
        }

        public NativeResult SessionPlay(nint session, NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_play");
            LastEpoch = epoch;
            return PlayResult;
        }

        public NativeResult SessionPause(nint session, NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_pause");
            LastEpoch = epoch;
            return NativeResult.Ok;
        }

        public NativeResult SessionSeek(
            nint session,
            NativeMediaTime position,
            NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_seek");
            LastPosition = position;
            LastEpoch = epoch;
            return NativeResult.Ok;
        }

        public NativeResult SessionSetVideoOutputSurface(
            nint session,
            NativeVideoOutputSurface surface,
            NativePlaybackEpoch epoch,
            out NativeVideoOutputSurfaceBinding binding)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_set_video_output_surface");
            LastSurface = surface;
            LastEpoch = epoch;
            binding = SurfaceBinding;
            return SurfaceResult;
        }

        public NativeResult SessionGetSnapshot(
            nint session, out NativePlaybackSnapshot snapshot)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_get_snapshot");
            snapshot = Snapshot;
            return NativeResult.Ok;
        }

        public NativeResult SessionClose(nint session, NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_close");
            LastEpoch = epoch;
            return NativeResult.Ok;
        }
    }
}
