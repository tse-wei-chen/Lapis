# Lapis

Lapis is a Windows-first media platform for high-quality local and Blu-ray playback, with non-blocking AI transcription and translation designed as first-class capabilities rather than playback dependencies.

## Architecture baseline

Lapis separates the **control plane** from the **media data plane**:

- `src/Lapis.Contracts` contains stable product/domain contracts only.
- `src/Lapis.Application` owns playback orchestration, serialized commands, state snapshots, epochs, and immutable graph planning.
- `native/MediaRuntime` owns the hot media path and exposes a stable C ABI.
- third-party media libraries will be added behind native adapters; their types must not leak into managed contracts.
- AI will observe media through taps and must never block playback.

The first architecture foundation deliberately does **not** add FFmpeg or libbluray yet. The project establishes the time model, epoch invalidation, session boundary, graph-plan model, native ABI, and architecture tests first.

## Build

Managed:

```powershell
dotnet restore Lapis.slnx
dotnet build Lapis.slnx -c Release --no-restore
dotnet test Lapis.slnx -c Release --no-build
```

Native:

```powershell
cmake -S native/MediaRuntime -B build/native -A x64
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
```

## Repository layout

```text
src/
  Lapis.Contracts/
  Lapis.Application/
native/
  MediaRuntime/
tests/
  Lapis.Contracts.Tests/
  Lapis.Application.Tests/
  Lapis.Architecture.Tests/
docs/
  architecture/
  adr/
```

Architecture decisions live in `docs/adr`. The dependency rules are executable in `Lapis.Architecture.Tests`.
