using BdmvPlayer.Contracts;

static void Ensure(NativeStatus status, string operation)
{
    if (status != NativeStatus.Ok) throw new InvalidOperationException($"{operation}: {status}");
}

var abi = AbiVersion.Create();
Ensure(NativeMethods.bdmv_get_abi_version(ref abi), "ABI check");
if (abi.Major != 1) throw new NotSupportedException($"Unsupported ABI major {abi.Major}");
Console.WriteLine($"MediaRuntime ABI {abi.Major}.{abi.Minor}.{abi.Patch}");

var config = EngineConfig.Create();
Ensure(NativeMethods.bdmv_engine_create(in config, out var engine), "engine_create");
try
{
    var caps = EngineCapabilities.Create();
    Ensure(NativeMethods.bdmv_engine_query_capabilities(engine, ref caps), "capabilities");
    Console.WriteLine($"Test source={caps.TestSource}; File={caps.FileSource}; Blu-ray={caps.BluraySource}");
    Ensure(NativeMethods.bdmv_session_create(engine, out var session), "session_create");
    try
    {
        if (caps.TestSource == 0)
        {
            Console.WriteLine("Diagnostic source not enabled; ABI check passed.");
            return;
        }
        Ensure(NativeMethods.bdmv_session_open_uri(session, "test://synthetic"), "open diagnostic source");
        Ensure(NativeMethods.bdmv_session_play(session), "play");
        Ensure(NativeMethods.bdmv_session_seek(session, 42_000_000_000), "seek");
        var snap = SessionSnapshot.Create();
        Ensure(NativeMethods.bdmv_session_get_snapshot(session, ref snap), "snapshot");
        if (snap.Epoch != 2 || snap.Transport != TransportState.Playing)
            throw new InvalidOperationException("Unexpected state / epoch");
        Console.WriteLine($"Session={snap.SessionId}, Epoch={snap.Epoch}, Position={snap.PositionNanoseconds}ns");
        Ensure(NativeMethods.bdmv_session_close(session), "close");
    }
    finally { NativeMethods.bdmv_session_destroy(session); }
}
finally { NativeMethods.bdmv_engine_destroy(engine); }
Console.WriteLine("C# -> native C ABI smoke passed; this does NOT decode or display media.");
