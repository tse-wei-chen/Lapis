# BDMV Player — Architecture v0.2（跨平台基準）

狀態：**Accepted as working baseline / 部分程式已實作**  
目標：Windows 10/11、Desktop Linux、SteamOS／Steam Deck；macOS 保留擴充邊界。  
來源：`architecture-v0.1-original.md`；本版延續原設計原則，修正 Windows-only 假設。

## 1. 不變的架構憲法

- **Control Plane / Data Plane 分離**。UI/Avalonia → Application/C# → versioned C ABI → C++ MediaRuntime；所有 AV packet、video frame、PCM 與 GPU 資源在 native hot path。
- `PlaybackClock` 單一權威；audio hardware clock 優先，失效時明確切換 policy。時間軸以 `MediaTime (int64 ns)` 表示，保留來源 PTS/rational 以避免多次轉換漂移。
- 每個非同步 payload 必須含 `SessionId + Epoch`；seek、來源分支變更、重建 Graph 或關閉 Session 都要遞增 Epoch。舊 Epoch 的資料不得進入新的輸出鏈。
- `GraphPlan` 概念上 immutable；切換採 build → prepare → warm → bounded handoff → atomic publish，不允許 UI 在運行中任意重連 node。
- AIHost 完全獨立行程，AI Tap 不可阻塞播放器；影音與 AI 各自有 bounded queue + backpressure 策略。
- Blu-ray navigation 與 content protection 分離；受保護光碟需**合法授權提供者**，不得假設 libbluray 自動支援商業光碟解密。
- Source/Decode/Render/Audio/Protection/AI/字幕/Plugins 均以 capabilities 選擇；避免在 Core 中放 `if Windows`、`if SteamOS`、`if FFmpeg`。

## 2. 新增的「真正跨平台」架構

```text
             Avalonia UI  /  Controller-focused Steam Deck UI
                                  |
             C# Application (commands, snapshots, policies)
                                  |
                   Stable versioned C ABI  (UTF-8)
                                  |
   +------------------- Portable C++20 Runtime -----------------+
   | Core: Session / Epoch / Clock / Graph / Sync / Scheduling  |
   | --------------------------------------------------------- |
   | Source adapters: File, SMB, libbluray, HTTP, synthetic     |
   | Codec adapters: FFmpeg CPU, D3D11VA, VA-API, Vulkan        |
   | Subtitle timeline: PGS, ASS, text, OCR, AI                 |
   | Shared interfaces: IVideoPresenter / IAudioSink / Surface  |
   +--------------|---------------------|-----------------------+
                  |                     |
     Windows platform adapters      Linux platform adapters
     HWND + D3D11/Vulkan            X11/Wayland + Vulkan
     WASAPI                         PipeWire / ALSA
     DXGI + monitor HDR state       DRM/Compositor HDR state
                  |                     |
             Display/audio             Display/audio
                       \             /
       AIHost (process, IPC: named pipes / Unix sockets,
               shared-memory ring buffer: OS-specific mapping)
```

**重點**：GPU frame 可以是「被 lease 的、後端擁有的 GPU resource」；不是 `void* pixels`。跨 API zero-copy 不能保證，一律以能力協商 + 有成本資訊的 copy/convert fallback 處理。CPU memory fallback 必須存在。

## 3. 平台矩陣（工程目標，不代表已支援）

| Domain | Windows | Linux desktop | SteamOS / Gamescope | macOS future |
|---|---|---|---|---|
| UI | Avalonia/Win32 | Avalonia/X11(default), Wayland驗證 | Avalonia + 10-foot controller UI | Avalonia/Cocoa |
| Native | C++20 DLL | C++20 .so | Linux .so | .dylib |
| Device/window | HWND | X11 Window / Wayland surfaces | Gamescope/WL/Xwayland | NSView/CAMetalLayer |
| Hardware decode | D3D11VA | VA-API, Vulkan (probe) | VA-API (Steam Deck), Vulkan path | VideoToolbox |
| Renderer | D3D11 or Vulkan | Vulkan + fallback | Vulkan + swapchain negotiation | Metal/MoltenVK（待驗證） |
| Audio | WASAPI | PipeWire → ALSA fallback | PipeWire | CoreAudio |
| HDR | DXGI/D3D capability | compositor dependent | Gamescope + monitor + driver dependent | platform dependent |
| Blu-ray | libbluray adapter | same adapter | BDMV folder first; physical disc conditional | same adapter |
| AI | subprocess/local/cloud | subprocess/local/cloud | power-aware throttling; cloud option | subprocess/local/cloud |
| Distribution | installer/portable | AppImage/Flatpak（later） | Flatpak + Steam launch（later） | DMG（later） |

`BDMV` 資料夾播放 ≠ 商業加密實體 BD/UHD BD 完整相容；BD-J 菜單、AACS/BD+、HDR10/Dolby Vision、Atmos passthrough 每一項分別驗收。

## 4. Linux/SteamOS 的主要「隱藏坑」

1. **原生子視窗不是跨平台影片 overlay 的保證**：Avalonia NativeControlHost 在不同環境的 z-order / transparency / transformations 受限。不要先承諾用 Avalonia 按鈕直接疊在任意 native Vulkan 視窗上。Phase 2 必須做 spike，二擇一：同 swapchain 合成 OSD/字幕/BD-J，或採 Avalonia composition 支援的明確共享表面；若不滿足 z-order 與 HDR，回退全 native presenter 合成。
2. **Wayland 協定與 UI thread**：native Wayland 尚需實測；先以 X11/XWayland 建立 SteamOS baseline，再單獨推進 native Wayland。不能隨意把 `wl_surface*` 跨 thread/producer 重建。
3. **Hardware decode ≠ zero-copy render**：VA-API surface 轉 Vulkan textures 涉及 DMA-BUF modifier、synchronization、device alignment。採 probe/interop contract，無法協商時 fallback staging copy/CPU decode。
4. **HDR 完整交付是一條路徑**：decode 10-bit → metadata → colorspace → compositor → display negotiation；不能用「能輸出 P010」宣稱 HDR support，OLED Steam Deck 也需 real-device validation。
5. **Bitstream 與 AI PCM Tap 並存**：audio packet 分岔給 bitstream output 與 AI-only decoder。Linux 的 PipeWire/ALSA capability 需查 endpoint；bitstream、exclusive audio 非所有裝置可用。
6. **Steam Deck 電源與控制**：按鍵焦點/快捷鍵、休眠恢復、GPU reset、手把 hotplug、720/800p/外接顯示器、GPU VRAM budget、AI 改用小模型或關閉本機推理，測試不可漏。
7. **Flatpak 沙盒**：BDMV 文件使用 portal/open folder；NAS、光碟裝置與 GPU 需明確權限。不可默認能讀取 `/run/media`, `/dev/sr0`, 任意 SMB mount。
8. **BD-J + JRE 發佈**：Java runtime version、security manager 相容性、overlay callback threading + sandbox/certification 分別處理，先測純 BDMV/BDMV menu，不能把 BD-J 視為保證。

## 5. Frame / Presenter 契約

- `VideoFrame = PTS + Epoch + ColorMetadata + ref-counted surface lease`；consumer 不知道具體 D3D11 texture、DMA-BUF 或 VkImage。
- `IVideoPresenter` 負責 attach/configure/present/detach、device lost、swapchain recreate。`IGraphicsDeviceManager`（未實作）每個 adapter/display context 的 resource owner；不允許每個 Node 自己建立 Device。
- `ColorMetadata` 至少含 primaries/transfer/matrix/range、mastering info、content light level、HDR dynamic side data、rotation/interlaced/aspect；目前 header 是**設計雛形**，完整色彩結構在 phase 2 確立。
- 音訊契約需分 PCM/DSP 和 IEC bitstream；音訊設備變更要帶同步時鐘切換與 Graph transaction。
- CPU/GPU surfaces 不跨 C# P/Invoke per-frame；C# 只收到 `PlaybackSnapshot` 和渲染 viewport identifier。

## 6. ABI / 生命週期 / 版本

- 公開介面只使用 C ABI：opaque `bdmv_engine`, `bdmv_session` + fixed-width integer + `struct_size`（詳見 `player_c_api.h`）。目前 ABI `1.0.0` 是 skeleton 的**試驗契約**，未對第三方宣告穩定。
- 每個呼叫明確回傳 enum status；不讓例外、FFmpeg types、C++ STL、`std::string` 跨出 DLL/SO 邊界。
- `Engine` 代表 runtime 配置；Session ID 單調增加，每個 Session 的 command/snapshot 以 mutex 保護。**目前還未實作 mailbox actor 和 command ticket**；在引入非同步 pipeline 前必須完成，避免 mutex 排程不等於命令順序的問題。
- `bdmv_session_destroy` 不允許與同一 Session 的其他呼叫同時進行；未來 Managed Layer 需用 SafeHandle/owner actor 保障使用中物件的生命週期。
- 雙方初始化時查 ABI major 和 struct size，跨 minor 只允許向後相容的 append-only fields；正式 plugin ABI/IPC 有獨立版本。

## 7. Graph / Session / Navigation

GraphPlan（待實作）的 node types：Source、Demux、VideoDecode、ColorProcess、AudioPCM、AudioBitstream、CaptionTimeline、Compositor、Output、AITap。

Graph build context 包含 content identity、tracks、source clock domains、runtime capabilities、estimated memory budget、requested sink、latency/battery preference、DRM protection policy。結果包含 **selection reasons + fallback chain + planned interop cost**，方便 debug。

Source Adapter return：`byte stream`、`navigation controller`、`track map`、`content fingerprint`。Blu-ray title/playlist/angle/clip/branch/chapter 的虛擬播放時間需要 discontinuity/epoch model，不只是單支 .m2ts 順播。

AI segment 對應 content ID + playback location + source track fingerprint + source timestamps；播放 Epoch 用於判斷是否丟棄當前顯示資料，不能當作永續翻譯 cache key（避免每次 seek 都重新跑 ASR）。

## 8. 下一個真正能播放的驗收門檻

**M0 [已具雛形]**：Portable runtime CMake + C ABI + session/synthetic state + epoch tests + dependency checks。  
**M1 [尚未開始]**：FileSource + FFmpeg demux + software decode + audio PCM → 黑畫面/測試影片（Windows/Linux 均跑通）。  
**M2 [尚未開始]**：Vulkan/D3D presenter + unified caption compositor + AV clock + seek regression。  
**M3 [尚未開始]**：libbluray BDMV clear content + navigation + PGS + menu。  
**M4 [尚未開始]**：SteamOS Gamescope / controllers / HDR / bitstream + Flatpak。  
**M5 [尚未開始]**：AIHost、ASR / OCR / translation、ahead scheduling、cache。

以上僅為功能先後關係，不是交付日期或已驗證性能。

## 9. 相容性相關權威文件

- Avalonia Linux（X11、Wayland）：https://docs.avaloniaui.net/docs/platform-specific-guides/linux
- Avalonia native interop：https://docs.avaloniaui.net/docs/app-development/native-interop
- FFmpeg HW devices：https://ffmpeg.org/ffmpeg.html
- libplacebo rendering：https://github.com/haasn/libplacebo/blob/master/docs/basic-rendering.md
- libbluray：https://code.videolan.org/videolan/libbluray

引用僅支持各套件公開能力；不構成本專案實機驗證。
