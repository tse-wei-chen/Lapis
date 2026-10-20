# ADR-0004 — 跨平台 Audio 與非阻塞 AI Tap

- Status: Accepted in principle; implementation pending.
- Audio source compressed packets split into (1) passthrough/bitstream (if endpoint supports it); (2) PCM decode path for DSP/output; (3) low-rate AI-only decode/tap; AI path never blocks audio clock.
- Backend: Windows WASAPI PCM/exclusive/IEC capability probe；Linux PipeWire native backend then ALSA fallback，按 endpoint profile dynamic negotiation。不能硬編 Atmos/TrueHD passthrough 常駐可用。
- Clock: audio hardware sample position + endpoint latency 帶版本快照；sink not started/device lost 時切 monotonic fallback，恢復後以 bounded drift correction 同步。AI timestamps 投影到 MediaTime。
- IPC: Windows named pipe、Linux UDS with versioned protobuf control events；shared-memory ring 搬運大量低延遲 PCM，用固定大小 bounded blocks。AI crashes/OOM/timeouts degraded，不推播到 real-time audio threads。
