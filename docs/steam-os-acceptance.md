# SteamOS / Linux 驗收矩陣（未驗證）

每列都必須紀錄：OS build、kernel、Mesa / Vulkan driver、GPU、Deck LCD/OLED、AV receiver/TV、display compositor、player build SHA、log bundle。

| ID | 裝置/環境 | 場景 | 最低驗收 | 狀態 |
|---|---|---|---|---|
| S01 | Steam Deck LCD / Desktop Mode | Flatpak launch / UI | 開啟/關閉/切換音量、焦點正確 | Not started |
| S02 | Steam Deck OLED / Desktop Mode | HDR10 + SDR | metadata/色彩/色帶測試，無 HDR 不誤報 | Not started |
| S03 | Deck LCD/OLED / Gaming Mode | Gamescope via Steam shortcut | 全螢幕、焦點、觸控和按鈕導航 | Not started |
| S04 | Deck LCD/OLED | sleep/resume + GPU reset | 回復後不黑屏、不死鎖；clock 重新同步 | Not started |
| S05 | Deck dock + HDMI AVR | PCM / compressed bitstream | endpoint capabilities probe；無支援時正確降級 | Not started |
| S06 | Fedora KDE Wayland | Wayland compositor + Vulkan | swapchain 再建立；XWayland fallback | Not started |
| S07 | Ubuntu X11 | VA-API + software fallback | decode + seek 連續使用不當機 | Not started |
| S08 | Arch / SteamOS | DMA-BUF modifier negotiation | zero-copy 或 clear copy fallback，不崩潰 | Not started |
| S09 | Deck + BDMV directory | clear Blu-ray title/PGS/menu | title、chapters、menu/buttons 對應正確 | Not started |
| S10 | Deck + protected physical disc | permission + protection provider | 不承諾繞過 DRM；明確報不支援 | Not started |
| S11 | Deck unplugged | local AI + 4K/1080p playback | playback never blocks on AI; resource budget enforced | Not started |
| S12 | Deck external display | hotplug SDR↔HDR/resolution | device/surface reconfigure + accurate capabilities | Not started |
| S13 | Linux Flatpak | file/portal/NAS permissions | denied paths produce actionable error, no blanket filesystem | Not started |
| S14 | Windows 11 | D3D11VA / WASAPI regression | identical core Session/Epoch behavior | Not started |

**Exit rule**：所有 S01–S08 與 S13/S14 通過才把 SteamOS 標為 beta；S09–S12 按功能個別標示。硬體未測試不得標示「支援 HDR / Atmos / BD-J」。
