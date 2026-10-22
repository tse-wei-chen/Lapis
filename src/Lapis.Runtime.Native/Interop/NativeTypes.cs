using System.Runtime.InteropServices;
using Lapis.Contracts.Playback;

namespace Lapis.Runtime.Native.Interop;

internal static class NativeAbi
{
    internal const uint Version = 2;
    internal const uint DiagnosticsVersion = 1;
    internal const uint NavigationVersion = 1;
    internal const uint SubtitleTracksVersion = 1;
    internal const uint TextSubtitleCuesVersion = 1;
    internal const uint TextSubtitleCancelVersion = 1;
    internal const uint FastForwardVersion = 1;
    internal const int TextSubtitleMaxCues = 4096;
    internal const int TextSubtitlePageCapacity = 16;
    internal const int TextSubtitlePageBytes = 65_536;
    internal const int SubtitleTrackCapacity = 64;
    internal const int DiagnosticRingCapacity = 128;
}

// 48-byte, all-blittable diagnostics v1 wire format.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeDiagnosticEvent
{
    public ulong Sequence;
    public ulong Epoch;
    public ulong MonotonicMilliseconds;
    public uint Kind;
    public uint FailureStage;
    public uint Hresult;
    public uint RemovedReason;
    public ulong Detail;
}

internal enum NativeResult : int
{
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    OutOfMemory = 3,
    StaleEpoch = 4,
    AbiMismatch = 5,
    Unsupported = 6,
    OutputUnavailable = 7,
    BufferTooSmall = 8,
    Internal = 100
}

// Exact 72-byte native subtitle descriptor with owned, fixed UTF-8 fields.
// Keep the two strings as inline byte buffers, never managed object pointers.
internal enum NativeTextSubtitleLoadState : int
{
    Loading = 0,
    Ready = 1,
    Failed = 2,
    LimitExceeded = 3,
    Cancelled = 4
}

// 24-byte native wire record, no string pointer and no variable-length data.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeTextSubtitleCueInfo
{
    public long StartNanoseconds;
    public long EndNanoseconds;
    public uint TextOffset;
    public uint TextLength;
}

internal enum NativeSubtitleTrackKind : uint
{
    Unknown = 0,
    Text = 1,
    Bitmap = 2
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeSubtitleTrackInfo
{
    public uint StreamIndex;
    public NativeSubtitleTrackKind Kind;
    public fixed byte LanguageUtf8[24];
    public fixed byte CodecNameUtf8[40];

    internal string GetLanguage()
    {
        fixed (byte* pointer = LanguageUtf8)
            return Decode(pointer, 24);
    }

    internal string GetCodecName()
    {
        fixed (byte* pointer = CodecNameUtf8)
            return Decode(pointer, 40);
    }

    private static string Decode(byte* pointer, int length)
    {
        var data = new ReadOnlySpan<byte>(pointer, length);
        var end = data.IndexOf((byte)0);
        if (end < 0)
            throw new InvalidOperationException(
                "Native subtitle descriptor has no NUL terminator.");
        return System.Text.Encoding.UTF8.GetString(data[..end]);
    }
}

internal enum NativeMediaLocatorKind : int
{
    File = 0,
    Directory = 1,
    Uri = 2,
    OpticalDisc = 3
}

// Mirrors `uint8_t bytes[16]` exactly. Do not model this as wider integer
// fields: ABI classification and byte order must not depend on architecture.
[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct NativePlaybackSessionId
{
    public byte Byte00;
    public byte Byte01;
    public byte Byte02;
    public byte Byte03;
    public byte Byte04;
    public byte Byte05;
    public byte Byte06;
    public byte Byte07;
    public byte Byte08;
    public byte Byte09;
    public byte Byte10;
    public byte Byte11;
    public byte Byte12;
    public byte Byte13;
    public byte Byte14;
    public byte Byte15;

    internal static NativePlaybackSessionId From(PlaybackSessionId sessionId)
    {
        Span<byte> bytes = stackalloc byte[16];
        if (!sessionId.Value.TryWriteBytes(bytes))
        {
            throw new InvalidOperationException("Could not serialize the playback session id.");
        }

        return new NativePlaybackSessionId
        {
            Byte00 = bytes[0],
            Byte01 = bytes[1],
            Byte02 = bytes[2],
            Byte03 = bytes[3],
            Byte04 = bytes[4],
            Byte05 = bytes[5],
            Byte06 = bytes[6],
            Byte07 = bytes[7],
            Byte08 = bytes[8],
            Byte09 = bytes[9],
            Byte10 = bytes[10],
            Byte11 = bytes[11],
            Byte12 = bytes[12],
            Byte13 = bytes[13],
            Byte14 = bytes[14],
            Byte15 = bytes[15]
        };
    }

    internal readonly byte[] ToByteArray() =>
    [
        Byte00, Byte01, Byte02, Byte03,
        Byte04, Byte05, Byte06, Byte07,
        Byte08, Byte09, Byte10, Byte11,
        Byte12, Byte13, Byte14, Byte15
    ];
}

internal enum NativeVideoOutputSurfaceKind : int
{
    None = 0,
    Win32Window = 1,
    X11Window = 2
}

internal enum NativeVideoOutputSurfaceBinding : int
{
    Applied = 0,
    Deferred = 1
}

// Mirrors enum(int32) + uintptr_t with the platform's natural alignment.
// Do not pack: on 64-bit Windows the handle begins at offset 8, not 4.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeVideoOutputSurface
{
    public NativeVideoOutputSurfaceKind Kind;
    public nuint NativeHandle;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeMediaTime
{
    public long Nanoseconds;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativePlaybackEpoch
{
    public ulong Value;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeMediaLocator
{
    public NativeMediaLocatorKind Kind;
    public nint ValueUtf8;
}

internal enum NativeTransportState : int
{
    Closed = 0,
    Opening = 1,
    Ready = 2,
    Playing = 3,
    Paused = 4,
    Seeking = 5,
    Ended = 6,
    Faulted = 7
}

// Mirrors lapis_playback_snapshot from C ABI v2. Guid byte order is
// preserved by NativePlaybackSessionId's 16 distinct byte fields.
[StructLayout(LayoutKind.Sequential)]
internal struct NativePlaybackSnapshot
{
    public NativePlaybackSessionId SessionId;
    public NativePlaybackEpoch Epoch;
    public NativeMediaTime Position;
    public NativeTransportState Transport;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeOpenResult
{
    public ulong Capabilities;
    public NativeMediaTime Duration;
    public uint HasDuration;
}
