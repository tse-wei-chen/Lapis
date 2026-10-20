# Changelog — M1c (2026-10-20)

- **Added**: bounded threaded DecodePump, IMediaDecodeSource and in-memory test frame generator.
- **Added**: ABI 1.1 nonblocking caller-buffer frame read; new status codes, C# DTO, smoke tests.
- **Added**: first frame PPM + audio PCM offline native export tool; synthetic end-to-end validation.
- **Changed**: FFmpeg adapter linked into MediaRuntime when `BDMV_WITH_FFMPEG=ON` (still not compiled locally); packet EAGAIN handling revised.
- **Fixed**: CMake Python discovery before native test registration so optional decoder fixtures enable when tools are present.
- **Preserved**: prior M1a/M1b project files, versioned ABI structs, Windows/Linux portability, manual-only GitHub workflow.
- **Limitations**: no verified libav-enabled build, PCM audio device, actual video display or time-synchronized playback; no platform hardware tests.
