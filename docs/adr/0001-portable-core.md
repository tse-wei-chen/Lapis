# ADR-0001 — Portable Core，不嵌入 OS 媒體堆疊

- Status: Accepted (2026-10-20)
- Context: v0.1 以 D3D11/DXGI/WASAPI 作為具體路徑，Linux/SteamOS 若延後適配會使核心耦合平台。
- Decision: `core/` 只依賴 C++20 標準庫和自有 domain types。OS/window/audio/GPU/FFmpeg/libbluray/DRM 全放在 adapter 下。公開 DLL/SO 僅用 C ABI。
- Constraints: `MediaTime`、`Epoch`、Session ownership 和 Clock policy 必須跨 OS 行為一致；平台探測透過能力快照注入 GraphBuilder。
- Consequences: 多數業務程式碼共用；必須維護穩定契約、OS-level adapter 測試，初期工程量增加。
- Guardrail: `tests/architecture/test_boundaries.py` 是第一層 tripwire；之後加入 include dependency graph + compiler header validation。
