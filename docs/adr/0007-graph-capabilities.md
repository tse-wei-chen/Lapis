# ADR-0007 — Immutable GraphPlan 與 Capability negotiation

- Status: Accepted in principle, implementation pending.
- GraphBuilder uses source/track/output/display/GPU/sandbox/DRM/latency/battery constraints to yield immutable `GraphPlan`, with `selected provider`, `reasons`, `fallbacks` and `estimated memory/interop cost`.
- Valid node interfaces are Source, Demux, Decode, VideoProcess, PCM Audio, Bitstream, SubtitleTimeline, AI observer Tap, Compositor, Sink. Explicit queues/fences/clock domains required per edge.
- Every queue bounded. Audio render/control callback 不得阻塞 AI, network, DB, GPU reconfiguration；超量時採 drop/background preemption / planned backpressure。
- Track/device/display switching uses build → warm → publish new epoch → drain old graph。Graph switch never opens the new output before mastering clocks and surface ownership resolution.
- Capability maps status into `Available`, `Unavailable`, `Degraded`, `PendingPermission` and error reasons; UI only reflects truth, not compile-time feature flags.
