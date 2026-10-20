# ADR-0005 — Linux 和 SteamOS 作為 first-class targets

- Status: Accepted.
- Minimum Linux baseline: x86_64 (glibc/driver versions pinned at release); use X11/XWayland UI while experimental native Wayland is isolated behind capability gate. Native Vulkan presentation only after real-surface negotiation spike.
- SteamOS: gaming mode runs in Gamescope; Deck LCD/OLED, handheld/docked, sleep/resume, Steam Input/controller focus, fullscreen and hotplug get their own tests. 不等同於一般 Ubuntu 測試完成。
- Packaging: Flatpak sandbox + portals, permissions minimal; DVD/BD external drives and mount paths are explicit privileged workflows, not granted globally by default.
- GPU: Deck AMD VA-API decode vs Vulkan compositor mapping needs actual driver/DMA-BUF tests; zero-copy is optimization, not requirement. HDR only declare supported after verified compositor/display/metadata end-to-end.
- CI: Linux native smoke is cheap; real SteamOS validation must run on hardware, not assumed from hosted CI. GitHub Actions manual-only by default.
