# 實作工作清單（允許平行開發，禁止破壞模組邊界）

## M0 — Cross-platform contract baseline

- [x] CMake C++20 shared library，可在 Linux 編譯
- [x] C header 固定 ABI major、enum/error、struct_size、opaque handles
- [x] Session 狀態機 + Epoch seek/close + concurrency smoke
- [x] C# P/Invoke contracts + managed smoke program（程式已建立，尚未在本環境編譯）
- [x] 靜態依賴掃描，禁止 Core 引入第三方 native types
- [x] GitHub Windows/Linux 手動 smoke workflow（未執行 Windows）
- [ ] 真正 C# Managed app Actor + SafeHandle / lifetime guard
- [ ] ABI append-only evolution + fuzz/error table tests
- [ ] clang-cl/MSVC compile & Windows smoke

## M1 — File / FFmpeg / software decode

- [ ] `IMediaSourceSession`：file + bounded async reader + seek/cancel；不可把 FFmpeg 型別暴露給 contract
- [ ] `DemuxAdapter`：FFmpeg、track descriptors、90kHz timestamp mapping + discontinuity handling
- [ ] software decoder and PCM sink：WASAPI / PipeWire minimal adapters
- [ ] frame queues bounded + duplicate/lost timestamp behavior
- [~] Portable video timing decisions + SDL dummy preview pass; hardware audio-master clock, A/V drift tests and audio device changes remain
- [~] Standalone SDL2 CPU RGBA/PCM preview on Linux dummy devices; Windows and Linux real hardware validation pending, THEN accelerated presenter spike

## M2 — GPU render/HDR architecture gate

- [ ] Same visual output plane for video+PGS+AI+BD-J overlay, validated on Linux/Windows
- [ ] Vulkan and D3D11 presenters, shader/color matrix capabilities
- [ ] VA-API → Vulkan DMA-BUF + fallback, D3D11VA → D3D resource + fallback
- [ ] 8-bit SDR, 10-bit HDR10, HDR→SDR test assets + screenshots/measurement
- [ ] Device lost recovery and multi-monitor HDR hotplug

## M3 — Blu-ray

- [ ] `libbluray` provider for clear BDMV folder/ISO/physical disc, granular support claims
- [ ] playlist/title/angle/chapter/seamless branching virtual timeline tests
- [ ] BD menu overlay event queue; thread-hop; input mapping; PGS
- [ ] BD-J Java runtime compatibility matrix (bundle/license/sandbox), separate feature switch
- [ ] DRM licensing provider contract, error reporting; no unlicensed decrypt path

## M4 — SteamOS and release

- [ ] real hardware S01–S14 matrix, do not infer from generic Linux run
- [ ] controller mapping, couch UI, Steam input and touch, display loss
- [ ] Flatpak manifest/permissions, desktop portals, native subprocess AI sandbox
- [ ] license/SBOM + FFmpeg config manifests + native dependency audit
- [ ] Windows installer, Linux Flatpak packaging, reproducible release artifacts

## M5 — AI

- [ ] IPC protobuf protocol with schema/version, Unix domain socket vs named pipe
- [ ] shared memory ring buffer + lifetime/backpressure/epoch metadata
- [ ] audio dual path bitstream/PCM-AI + ASR/translation/OCR separate pipelines
- [ ] ahead scheduler prioritized by playback position and seek
- [ ] ASR/translation caches keyed on content fingerprints not session epoch

## Engineering rules

每一個 PR：說明 Contract 差異、執行 architecture/native/sync regression tests、附 PlayTrace 前後樣本。GitHub workflow 只 `workflow_dispatch`，避免 push/commit 自動觸發付費 Actions。
