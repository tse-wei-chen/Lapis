對。這種專案最值得花時間的其實不是第一個月把影片播出來，而是先把 \*\*10 種未來需求加入後，架構仍然不用推翻\*\* 想清楚。



我會把它設計成一個「Media Platform」，PowerDVD 類 UI 只是它的第一個 frontend。



\---



\# 1. 最上層架構



我建議一開始就切成 \*\*Control Plane 與 Media Data Plane\*\*。



```text

┌─────────────────────────────────────────────────────────────┐

│                       Player.App                            │

│                    C# / .NET / Avalonia                     │

│                                                             │

│   UI / MVVM / Settings / Library / Commands / View State    │

└───────────────────────────┬─────────────────────────────────┘

&#x20;                           │

&#x20;                  Commands │ Events/Snapshots

&#x20;                           │

┌───────────────────────────▼─────────────────────────────────┐

│                 Application Orchestrator                    │

│                         C#                                  │

│                                                             │

│ PlaybackSession   AI Coordinator   Plugin Manager           │

│ Library           Settings         Cache                    │

│ Command Router    Capability DB    Telemetry                │

└───────────────┬───────────────────┬─────────────────────────┘

&#x20;               │                   │

&#x20;               │ Stable C ABI      │ IPC

&#x20;               │                   │

┌───────────────▼──────────────┐    │     ┌───────────────────┐

│       MediaRuntime.dll       │    └────►│   AIHost.exe      │

│          C++20               │          │                   │

│                              │          │ Whisper / Qwen     │

│ Real-time playback engine    │          │ OpenAI / OCR       │

│ FFmpeg / libbluray / D3D     │          │ Translation       │

│ WASAPI / Scheduler / Clock   │          └───────────────────┘

└──────────────────────────────┘

```



Avalonia/Windows App SDK 現在仍是 Microsoft 推薦的新 Windows desktop app 路線，而且可以透過 native interop 把自己的 `IDXGISwapChain` 接進 `SwapChainPanel`，很適合這種自己掌握影音 renderer 的產品。\[Microsoft Learn](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/?utm\_source=chatgpt.com)



重點是：



> \*\*UI 不播放影片。C# Application 層也不播放影片。MediaRuntime 才播放影片。\*\*



\---



\# 2. 為什麼我不會全部寫 C#



不是 C# 效能不夠，而是：



```text

AVPacket

AVFrame

D3D11 Texture

GPU Fence

WASAPI buffer

decoder callback

render timing

```



這些如果每一幀都：



```text

Native

&#x20;  ↓ P/Invoke

C#

&#x20;  ↓ P/Invoke

Native

```



架構久了會非常難搞。



所以我的界線會是：



```text

&#x20;            LOW FREQUENCY

C# -------------------------------- Native

&#x20;    Play()

&#x20;    Pause()

&#x20;    Seek()

&#x20;    SelectTrack()

&#x20;    Open()

&#x20;    GetState()



&#x20;            HIGH FREQUENCY

Native ---------------------- Native

&#x20;    AVPacket

&#x20;    AVFrame

&#x20;    Audio samples

&#x20;    GPU texture

&#x20;    Subtitle bitmap

&#x20;    Present

```



C# 永遠不碰：



```cpp

AVFormatContext\*

AVCodecContext\*

AVFrame\*

AVPacket\*

ID3D11Texture2D\*

```



FFmpeg 的 `libavformat` 本身負責 demux / protocol，`libavcodec` 負責 codec；所以可以很自然地限制在 Media Runtime Adapter 中，而不是滲透整個應用。\[FFmpeg](https://www.ffmpeg.org/documentation.html?utm\_source=chatgpt.com)



\---



\# 3. 最重要的一條：Domain 不准知道 FFmpeg



錯誤設計：



```csharp

public class VideoTrack

{

&#x20;   public AVCodecParameters CodecParameters;

}

```



這等於：



> 整個播放器 = FFmpeg wrapper



未來換東西就炸掉。



應該自己定義：



```csharp

public sealed record MediaTrack(

&#x20;   TrackId Id,

&#x20;   MediaType Type,

&#x20;   CodecId Codec,

&#x20;   string? Language,

&#x20;   ChannelLayout? Channels,

&#x20;   VideoFormat? Video,

&#x20;   AudioFormat? Audio,

&#x20;   TrackFlags Flags);

```



Native 內：



```text

AVStream

&#x20;  ↓

FFmpegAdapter

&#x20;  ↓

Player MediaTrack

```



反方向也一樣。



因此未來：



```text

FFmpeg

Media Foundation

自家 decoder

某個商業 Dolby decoder

```



都只是 Adapter。



\---



\# 4. Media Source 也必須抽象



不要設計成：



```text

Player.OpenFile()

Player.OpenBluray()

Player.OpenUrl()

```



而是：



```text

&#x20;                  MediaSource

&#x20;                      │

&#x20;      ┌───────────────┼────────────────┐

&#x20;      ▼               ▼                ▼

&#x20;  FileSource     BluRaySource      NetworkSource

&#x20;                                      │

&#x20;                             ┌────────┼────────┐

&#x20;                             HTTP    SMB     Future...

```



概念 contract：



```csharp

public interface IMediaSourceProvider

{

&#x20;   bool CanOpen(MediaLocator locator);



&#x20;   Task<IMediaSourceSession> OpenAsync(

&#x20;       MediaLocator locator,

&#x20;       CancellationToken ct);

}

```



Session：



```csharp

public interface IMediaSourceSession

{

&#x20;   MediaIdentity Identity { get; }



&#x20;   StreamCapabilities Capabilities { get; }



&#x20;   IMediaByteStream Stream { get; }



&#x20;   INavigationController? Navigation { get; }

}

```



這裡有一個非常重要的決策：



\### Navigation 與 Stream 分開。



普通 MKV：



```text

FileSource

&#x20;  │

&#x20;  ├─ Stream

&#x20;  │

&#x20;  └─ Navigation = null

```



Blu-ray：



```text

BluraySource

&#x20;  │

&#x20;  ├─ Virtual MPEG-TS Stream

&#x20;  │

&#x20;  └─ BlurayNavigation

&#x20;        │

&#x20;        ├─ Title

&#x20;        ├─ Playlist

&#x20;        ├─ Chapter

&#x20;        ├─ Angle

&#x20;        ├─ Top Menu

&#x20;        ├─ Popup Menu

&#x20;        └─ BD-J

```



這非常重要。



\---



\# 5. libbluray 只能存在 Bluray Adapter



```text

Player.Core

&#x20;    │

&#x20;    ▼

INavigationController

&#x20;    ▲

&#x20;    │

BlurayAdapter

&#x20;    │

&#x20;    ▼

libbluray

```



libbluray 現在本身就整合了 playlist parsing、navigation、menu 與 BD-J；其 API 也有 Top Menu、First Play、BD-J title、ARGB overlay callback 等能力。\[GitHub](https://github.com/ShiftMediaProject/libbluray/blob/master/README.md?utm\_source=chatgpt.com)



所以：



```text

Player

```



不能知道：



```text

BLURAY\*

BLURAY\_TITLE

BD\_EVENT

```



只能知道：



```csharp

NavigationEvent

MenuOverlay

TitleInfo

ChapterInfo

AngleInfo

```



這樣萬一某天：



```text

libbluray 2

商用 Blu-ray SDK

licensed DRM SDK

```



全部可以替換。



\---



\# 6. DRM 更不能混進 Blu-ray Engine



架構：



```text

Physical Disc

&#x20;     │

&#x20;     ▼

Content Access Layer

&#x20;     │

&#x20;     ├── ClearContentProvider

&#x20;     │

&#x20;     └── LicensedProtectionProvider (future)

&#x20;     │

&#x20;     ▼

BluRaySource

```



不要：



```text

BluraySource

{

&#x20;   if (AACS)

&#x20;      decrypt...

}

```



Protection 必須是一個 capability。



因為 libbluray 自己也明確說明：它可以處理 Blu-ray navigation/menus/BD-J，但單靠 libbluray 並不足以播放多數 AACS/BD+ 保護的商業 BD。\[GitHub](https://github.com/ShiftMediaProject/libbluray/blob/master/README.md?utm\_source=chatgpt.com)



這個法律/授權邊界最好從第一天就乾淨。



\---



\# 7. 整個播放器真正的心臟：Media Graph



我不會硬編：



```text

Demux → Decode → Render

```



而會有一份 immutable：



```text

GraphPlan

```



例如普通播放：



```text

&#x20;                   Demuxer

&#x20;                      │

&#x20;         ┌────────────┼────────────┐

&#x20;         │            │            │

&#x20;       Video         Audio       Subtitle

&#x20;         │            │            │

&#x20;       Decode       Decode       Decode

&#x20;         │            │            │

&#x20;    VideoProcess    DSP        CueTimeline

&#x20;         │            │            │

&#x20;         ▼            ▼            │

&#x20;     Compositor   AudioSink         │

&#x20;         ▲                         │

&#x20;         └─────────────────────────┘

```



Graph 本身由：



```text

GraphBuilder

```



依 capabilities 建出來。



例如：



```text

Hardware supports HEVC Main10?

&#x20;       │

&#x20;      Yes

&#x20;       ↓

D3D11VA HEVC Decoder



HDR monitor?

&#x20;       │

&#x20;      Yes

&#x20;       ↓

HDR Passthrough



HDR monitor?

&#x20;       │

&#x20;      No

&#x20;       ↓

ToneMapper

```



因此：



```text

GraphBuilder

&#x20;   ↓

GraphPlan

&#x20;   ↓

MediaRuntime

```



\---



\# 8. Graph 不要「到處動態改」



這是另外一個我非常想避免的坑。



不要：



```text

node.Insert()

node.Remove()

node.Reconnect()

```



播放途中任意 mutation。



應該：



```text

Current Graph

&#x20;    │

&#x20;    │ track switch

&#x20;    ▼

Build New GraphPlan

&#x20;    │

Prepare

&#x20;    │

Warm

&#x20;    │

Atomic Switch

&#x20;    ▼

New Graph

```



也就是：



> \*\*Graph 是 immutable，換軌等於換 Plan。\*\*



會比 mutable pipeline 好維護非常多。



\---



\# 9. 時間模型從第一天就自己掌握



這個是最不能偷懶的。



不要所有地方都用：



```csharp

TimeSpan

```



也不要依賴：



```text

AV\_TIME\_BASE

```



我會有：



```cpp

struct MediaTime

{

&#x20;   int64\_t nanoseconds;

};

```



以及：



```cpp

struct SourceTimestamp

{

&#x20;   int64\_t value;

&#x20;   Rational timeBase;

};

```



因此：



```text

Blu-ray 90kHz PTS

FFmpeg timebase

Audio sample position

AI subtitle timestamp

```



進核心後全部投影到：



```text

MediaTime

```



原始 timestamp 可以留下來做 diagnostics。



\---



\# 10. 還要有 Epoch



這東西非常重要。



假設：



```text

正在播放

epoch = 14

```



使用者突然：



```text

Seek 01:00 → 01:40

```



立刻：



```text

epoch = 15

```



這時之前所有：



```text

epoch 14 video frame

epoch 14 subtitle

epoch 14 AI translation

epoch 14 decoder callback

```



即使晚到：



```text

DROP

```



全部丟掉。



資料：



```csharp

record TimedPayload(

&#x20;   PlaybackEpoch Epoch,

&#x20;   MediaTime Timestamp);

```



這一招可以解決大量：



```text

Seek 後突然出現舊字幕

切音軌後舊音訊冒出

AI 翻譯晚到

decoder flush race

```



問題。



\---



\# 11. Playback Clock 只有一個權威



不要：



```text

Video 有自己的 currentTime

Audio 有 currentTime

UI 有 currentTime

Subtitle 有 currentTime

```



會瘋掉。



必須：



```text

&#x20;            PlaybackClock

&#x20;                  │

&#x20;       ┌──────────┼─────────────┐

&#x20;       ▼          ▼             ▼

&#x20;     Video      Subtitle        UI

```



通常正常 PCM playback：



```text

Audio Hardware Clock

&#x20;       │

&#x20;       ▼

PlaybackClock

```



Video：



```text

frame PTS

&#x20;  ↓

PlaybackClock

&#x20;  ↓

early → wait

late  → drop

```



Subtitle 同理。



\---



\# 12. Audio 一開始就要支援兩條路



很多播放器後期才想到 TrueHD / DTS-HD bitstream，然後架構直接炸掉。



所以一開始：



```text

&#x20;                    Audio Packet

&#x20;                         │

&#x20;               ┌─────────┴──────────┐

&#x20;               │                    │

&#x20;         Bitstream Path          Decode Path

&#x20;               │                    │

&#x20;       IEC / WASAPI             PCM Decoder

&#x20;               │                    │

&#x20;            AVR/TV              Mixer/DSP

&#x20;                                    │

&#x20;                                 WASAPI

```



Windows WASAPI 本身就區分 shared / exclusive stream；exclusive 可以直接以硬體支援格式和 endpoint 溝通，而 shared-mode 的低延遲路徑現在也能做到相當低的 period。\[Microsoft Learn](https://learn.microsoft.com/en-us/windows/desktop/coreaudio/exclusive-mode-streams?utm\_source=chatgpt.com)



UI：



```text

Audio output



○ Shared PCM

○ Exclusive PCM

● Bitstream Passthrough

```



\---



\# 13. AI 出現一個很有意思的問題



假設使用者：



```text

TrueHD 7.1 → AVR Bitstream

```



那播放器其實沒有 PCM。



但是 AI 需要 PCM。



所以架構第一天就必須支援：



```text

&#x20;             Compressed Audio Packets

&#x20;                       │

&#x20;             ┌─────────┴─────────┐

&#x20;             ▼                   ▼

&#x20;      Bitstream Output        AI Decoder

&#x20;             │                   │

&#x20;            AVR              Downmix Mono

&#x20;                                 │

&#x20;                              16 kHz

&#x20;                                 │

&#x20;                              AI Tap

```



這樣才能同時：



> Atmos bitstream + AI 字幕



否則做到 AI 時一定重構。



\---



\# 14. AI 不准成為 Playback Graph 的 blocking node



錯：



```text

Audio

&#x20;↓

AI

&#x20;↓

Translation

&#x20;↓

Playback

```



AI 掛了影片也掛。



正確：



```text

&#x20;                 Audio

&#x20;                   │

&#x20;            ┌──────┴──────┐

&#x20;            ▼             ▼

&#x20;        Playback        AudioTap

&#x20;                           │

&#x20;                        AI Queue

&#x20;                           │

&#x20;                         AIHost

```



AI 永遠只能：



```text

observe playback

```



不能：



```text

control playback

```



\---



\# 15. AI Host 我會直接獨立 Process



```text

Player.exe



MediaRuntime.dll





AIHost.exe

&#x20;├─ ASR

&#x20;├─ OCR

&#x20;├─ Translation

&#x20;├─ Embedding

&#x20;└─ Future models

```



原因很實際：



```text

CUDA OOM

DirectML crash

Python dependency

模型更新

模型 unload

VRAM 爆掉

```



不能讓這些東西污染 Player process。



兩邊：



```text

Named Pipe

\+

Protobuf

```



控制資料。



大量 audio：



```text

Shared Memory Ring Buffer

```



\---



\# 16. AI 字幕本身也分成三層



```text

SpeechRecognition

&#x20;       ↓

Source Subtitle

&#x20;       ↓

Translation

&#x20;       ↓

Presentation

```



資料不能只有：



```csharp

string text;

```



應該是：



```csharp

SubtitleSegment

{

&#x20;   SegmentId Id;



&#x20;   MediaTime Start;

&#x20;   MediaTime End;



&#x20;   string SourceText;

&#x20;   string? TranslatedText;



&#x20;   string SourceLanguage;

&#x20;   string TargetLanguage;



&#x20;   float Confidence;



&#x20;   SegmentState State;

&#x20;   Revision Revision;

}

```



因為 Realtime ASR 很可能：



```text

Partial

&#x20;  ↓

Partial

&#x20;  ↓

Final

```



甚至翻譯也可能 revise。



\---



\# 17. AI Ahead Buffer 要是一級公民



不是 workaround。



直接設計：



```text

PlaybackPosition

&#x20;     │

&#x20;     ▼

&#x20; 01:20:00



AI Priority Window

&#x20;     │

&#x20;     ├─ Urgent    01:20 → 01:20:10

&#x20;     ├─ Ahead     01:20:10 → 01:21:00

&#x20;     └─ Background 01:21+

```



Scheduler：



```text

&#x20;            AI Work Scheduler

&#x20;                   │

&#x20;         ┌─────────┼────────┐

&#x20;         ▼         ▼        ▼

&#x20;      Urgent      Ahead   Background

```



Seek：



```text

01:20 → 02:00



cancel old priority

&#x20;     ↓

02:00 becomes Urgent

```



\---



\# 18. Subtitle 必須是完整獨立系統



來源：



```text

&#x20;                   SubtitleTimeline

&#x20;                          ▲

&#x20;           ┌──────────────┼──────────────┐

&#x20;           │              │              │

&#x20;         SRT/ASS          PGS            AI

&#x20;                           │

&#x20;                          OCR

```



Renderer 不應該知道字幕來源。



Renderer 只收：



```text

TextCue

BitmapCue

```



如此：



```text

SRT

ASS

PGS

OCR

AI

External Plugin

```



全部共用。



\---



\# 19. BD-J Overlay 不等於 Subtitle



這也非常容易設計錯。



libbluray 的 BD-J 可以送 ARGB graphics overlay，而且 callback 甚至可能從 JVM 建立的 thread 進來。\[GitHub](https://github.com/ShiftMediaProject/libbluray/blob/master/src/libbluray/bluray.h?utm\_source=chatgpt.com)



因此：



```text

libbluray callback

&#x20;     │

&#x20;     X 千萬不要直接操作 Renderer

&#x20;     │

&#x20;     ▼

Navigation Event Queue

&#x20;     │

&#x20;     ▼

Overlay Manager

```



最後：



```text

&#x20;              Video Frame

&#x20;                   │

&#x20;                   ▼

&#x20;            Video Processor

&#x20;                   │

&#x20;                   ▼

&#x20;              Compositor

&#x20;            ▲      ▲      ▲

&#x20;            │      │      │

&#x20;          PGS     AI     BD-J

&#x20;                   │

&#x20;                   ▼

&#x20;             Video Presenter

```



\---



\# 20. VideoFrame 千萬不要等於 pixel buffer



應該是：



```text

VideoFrame

&#x20;│

&#x20;├─ Surface

&#x20;│    ├─ SystemMemory

&#x20;│    ├─ D3D11Texture

&#x20;│    └─ Future D3D12Resource

&#x20;│

&#x20;├─ Timestamp

&#x20;├─ ColorInfo

&#x20;├─ HDR Metadata

&#x20;├─ Aspect

&#x20;├─ InterlaceInfo

&#x20;└─ SideData

```



因此未來：



```text

D3D11VA

D3D12 Video

Intel

AMD

NVIDIA

Software

```



Renderer 不用重寫。



\---



\# 21. HDR 也不能藏在 decoder 裡



獨立：



```text

Decode

&#x20; ↓

VideoFrame + ColorMetadata

&#x20; ↓

ColorPipeline

&#x20; │

&#x20; ├─ SDR → SDR

&#x20; ├─ HDR10 → HDR

&#x20; ├─ HDR10 → ToneMap → SDR

&#x20; └─ Future DV processing

&#x20; ↓

Compositor

```



也就是：



> pixels 是 pixels，color metadata 是 metadata。



不能混為一談。



\---



\# 22. DeviceManager 要是 singleton resource owner



不要每個 module：



```text

自己 CreateD3DDevice()

```



應該：



```text

GraphicsDeviceManager

&#x20;     │

&#x20;     ├─ D3D Device

&#x20;     ├─ DXGI Factory

&#x20;     ├─ Adapter

&#x20;     ├─ SwapChain

&#x20;     └─ DeviceLost recovery

```



Decoder：



```text

borrow

```



Renderer：



```text

borrow

```



VideoProcessor：



```text

borrow

```



如此 GPU device lost：



```text

DeviceManager

&#x20;    ↓

Rebuild resources

&#x20;    ↓

Graph rebind

```



而不是 App restart。



\---



\# 23. State 也不要用一個巨大 Enum



很多播放器最後變：



```csharp

enum PlayerState

{

&#x20;   Opening,

&#x20;   Playing,

&#x20;   PlayingMenu,

&#x20;   PlayingHdr,

&#x20;   PlayingMenuPaused,

&#x20;   BufferingMenu...

}

```



最後爆炸。



我會用 orthogonal states：



```text

PlaybackSessionState



Transport

&#x20;├ Closed

&#x20;├ Opening

&#x20;├ Ready

&#x20;├ Playing

&#x20;├ Paused

&#x20;├ Seeking

&#x20;├ Ended

&#x20;└ Faulted



Navigation

&#x20;├ Feature

&#x20;├ Menu

&#x20;├ Popup

&#x20;└ Still



Output

&#x20;├ Ready

&#x20;├ Reconfiguring

&#x20;└ DeviceLost



AI

&#x20;├ Off

&#x20;├ Warming

&#x20;├ Active

&#x20;└ Degraded

```



最後 UI 收：



```csharp

PlaybackSnapshot

```



\---



\# 24. 所有 Command 必須序列化



UI 不直接：



```text

Thread A Seek

Thread B Pause

Thread C SelectAudio

```



全部：



```text

&#x20;               Command Mailbox

&#x20;                     │

&#x20;      ┌──────────────┼──────────────┐

&#x20;      ▼              ▼              ▼

&#x20;     Seek          Pause        TrackChange

&#x20;                     │

&#x20;                     ▼

&#x20;            PlaybackCoordinator

```



單一 logical owner。



因此 race condition 會少掉非常多。



\---



\# 25. Event 與 Command 完全分開



```text

UI

&#x20;│

&#x20;│ Command

&#x20;▼

Engine

&#x20;│

&#x20;│ Event

&#x20;▼

UI

```



Command：



```text

Play

Pause

Seek

SelectTrack

OpenMenu

```



Event：



```text

StateChanged

PositionChanged

TrackChanged

BufferingChanged

NavigationChanged

ErrorOccurred

```



禁止：



```text

ViewModel.Player.SeekInternal()

```



\---



\# 26. Public Plugin 架構不要直接暴露 DLL



這也是未來會後悔的地方。



內建 module：



```text

In Process

```



第三方 plugin：



```text

PluginHost.exe

```



透過：



```text

IPC Contract

```



例如：



```json

{

&#x20; "contractVersion": "1.2",

&#x20; "plugin": "ExampleTranslator",

&#x20; "capabilities": \[

&#x20;   "translation"

&#x20; ]

}

```



這樣插件就算：



```text

.NET 10

.NET 12

Rust

Python

C++

```



都無所謂。



也不會因為你的：



```csharp

IPluginV2

```



interface 改一個 method 全部 plugin 爆炸。



\---



\# 27. Capability-Based Architecture



不要問：



```csharp

if (player is BlurayPlayer)

```



而要問：



```csharp

if (session.Capabilities.Has(

&#x20;   MediaCapability.MenuNavigation))

```



例如：



```text

MenuNavigation

MultipleAngles

Chapters

HdrOutput

BitstreamOutput

BitmapSubtitles

AiTranslation

RealtimeAsr

OfflineAsr

DiscNavigation

```



UI 自動：



```text

Capability 不存在

&#x20;       ↓

按鈕不顯示

```



\---



\# 28. Cache 也要拆



我會至少分：



```text

Library.db

&#x20;  電影資訊

&#x20;  resume

&#x20;  history



Player.db

&#x20;  settings

&#x20;  devices



AiCache.db

&#x20;  ASR segments

&#x20;  translations

&#x20;  OCR



Artwork/

Waveform/

SubtitleCache/

```



AI cache key：



```text

ContentFingerprint

\+

Playlist

\+

Angle

\+

TrackFingerprint

\+

SourceLanguage

\+

Model

\+

ModelVersion

\+

PromptVersion

```



特別是：



> ASR 與 Translation cache 分開。



這樣換翻譯模型：



```text

不用重新跑 ASR。

```



\---



\# 29. Content Identity 很重要



不能只用 filename：



```text

MOVIE.m2ts

```



Blu-ray 要有：



```text

DiscFingerprint

PlaylistId

AngleId

AudioTrackFingerprint

```



普通影片：



```text

File size

\+

hash ranges

\+

duration

\+

stream signature

```



這會影響：



```text

Resume

AI cache

Subtitle

Library

Metadata

```



\---



\# 30. Threading Model 我會直接定死



| Thread / Process | 責任 |

|---|---|

| UI Thread | Avalonia |

| Control Actor | Commands / State |

| Demux Thread | packet reading |

| Video Decode Workers | decode |

| Video Scheduler | frame timing |

| Render Thread | GPU |

| Audio/MMCSS Thread | WASAPI |

| Navigation ingress | libbluray event |

| AIHost Process | ASR/OCR/translation |

| Background Worker | DB/cache/library |



原則：



```text

Audio thread NEVER wait AI.

Render thread NEVER wait DB.

Demux thread NEVER wait UI.

```



\---



\# 31. Backpressure 必須存在



所有 queue：



```text

bounded

```



不能：



```text

ConcurrentQueue<Frame>

無限塞

```



例如：



```text

Demux

&#x20;↓

Video Packet Queue \[bounded]

&#x20;↓

Decoder

&#x20;↓

Video Frame Queue \[bounded]

&#x20;↓

Scheduler

```



queue 滿：



```text

producer wait / drop

```



由 policy 決定。



AI queue 則可以：



```text

drop background work

```



但絕對不能拖 playback。



\---



\# 32. Error Model 也要標準化



不要到處：



```text

Exception: HRESULT -214...

```



核心：



```csharp

PlayerError

{

&#x20;   Domain;

&#x20;   Code;

&#x20;   Severity;

&#x20;   Recoverability;

&#x20;   Context;

&#x20;   NativeError;

}

```



例如：



```text

Audio.DeviceUnavailable

Video.HardwareDecoderFailed

Bluray.MenuRuntimeMissing

AI.ProviderUnavailable

Source.ReadFailed

```



然後 policy：



```text

Hardware decode failed

&#x20;      ↓

Fallback software decode

```



不是直接 crash。



\---



\# 33. Observability 第一版就做



播放器是非常難 debug 的系統。



建議每場 PlaybackSession 都有：



```text

Playback Trace

```



例如：



```text

15:31:02.123 open

15:31:02.541 graph-created

15:31:02.612 audio-start

15:31:02.613 clock-master=audio

15:31:03.124 video-drift=+4.2ms

15:31:05.514 queue-video=7

15:31:08.121 seek 124s → 552s

15:31:08.122 epoch=14→15

```



甚至提供：



```text

Export Diagnostic Bundle

```



未來你會非常感謝自己。



\---



\# 34. Solution 我會長這樣



```text

YourPlayer/

│

├── src/

│

│   ├── Player.App/

│   │     Avalonia

│   │

│   ├── Player.Application/

│   │     Commands

│   │     Sessions

│   │     Library

│   │     Settings

│   │     AI orchestration

│   │

│   ├── Player.Contracts/

│   │     DTO

│   │     MediaTime

│   │     TrackInfo

│   │     PlaybackSnapshot

│   │

│   ├── Player.AI/

│   │     AI client

│   │     scheduler

│   │     subtitle pipeline

│   │

│   ├── Player.Storage/

│   │

│   └── Player.PluginSdk/

│

├── native/

│

│   ├── MediaRuntime/

│   │

│   │   ├── Core/

│   │   │    Clock

│   │   │    Scheduler

│   │   │    Graph

│   │   │    Session

│   │   │

│   │   ├── Source/

│   │   │    File

│   │   │    Network

│   │   │    BluRay

│   │   │

│   │   ├── Demux/

│   │   │    FFmpeg

│   │   │

│   │   ├── Decode/

│   │   │    FFmpeg

│   │   │    Hardware

│   │   │

│   │   ├── Video/

│   │   │    D3D

│   │   │    HDR

│   │   │    Compositor

│   │   │

│   │   ├── Audio/

│   │   │    WASAPI

│   │   │    Bitstream

│   │   │

│   │   ├── Subtitle/

│   │   │

│   │   ├── Navigation/

│   │   │    libbluray

│   │   │

│   │   └── Interop/

│   │        C ABI

│   │

│   └── third\_party/

│         FFmpeg

│         libbluray

│         libass

│

├── hosts/

│

│   ├── Player.AIHost/

│   └── Player.PluginHost/

│

├── tests/

│   ├── Core.Tests/

│   ├── Media.Tests/

│   ├── Sync.Tests/

│   ├── Bluray.Tests/

│   ├── AI.Tests/

│   └── Integration.Tests/

│

└── docs/

&#x20;   ├── architecture/

&#x20;   ├── adr/

&#x20;   └── contracts/

```



\---



\# 35. 最重要的是 Architecture Tests



直接寫測試禁止：



```text

Player.Application → FFmpeg

Player.App → MediaRuntime internals

AI → UI

Storage → Playback engine

libbluray → Application

```



依賴方向固定：



```text

UI

&#x20;↓

Application

&#x20;↓

Contracts



Native adapters

&#x20;↓

Native Core

```



第三方 library 永遠在最外圍。



\---



\# 36. 哪些東西「反而不要過度抽象」



這點也很重要。



擴展性 ≠ 所有東西都 `IWhateverService`。



以下我反而不會允許隨便 plugin：



```text

PlaybackClock

A/V Scheduler

Session State Machine

MediaTime

Epoch

Graph Runtime

Command Ordering

```



這些是：



> \*\*播放器憲法。\*\*



必須非常穩定。



可以替換的是：



```text

Source

Decoder

Renderer

AI Provider

Translator

OCR

Metadata Provider

Protection Provider

Output Backend

```



所以架構應該是：



```text

穩定核心

&#x20;  +

可插拔邊界

```



不是：



```text

所有東西都可插拔

```



這兩者差非常多。



\---



\# 37. 最後用「未來需求」驗證架構



| 未來加入 | 要改的地方 |

|---|---|

| DVD | Source + Navigation |

| UHD BD | Bluray adapter / capabilities |

| 新 DRM SDK | Protection Provider |

| AV1 | Decoder adapter |

| D3D12 | Video backend |

| Dolby Vision | Color pipeline |

| Atmos bitstream | Audio bitstream backend |

| Whisper vNext | AI provider |

| OpenAI Realtime | AI provider |

| OCR 新模型 | OCR provider |

| YouTube | Source provider |

| NAS/SMB | Source provider |

| 新字幕格式 | Subtitle decoder |

| Shader | VideoProcessor |

| Frame interpolation | VideoProcessor |

| Super Resolution | VideoProcessor |

| 第二種 UI | App frontend |

| CLI player | App frontend |

| Linux | Platform layer，大量上層不動 |



如果新增其中任何一項需要：



> 「改 Player.Core 到處的 if/else」



那就代表架構失敗。



\---



\## 所以我目前會把整個系統的「憲法」定成這樣



```text

&#x20;              USER / UI

&#x20;                  │

&#x20;            Control Plane

&#x20;                  │

&#x20;         Playback Coordinator

&#x20;                  │

&#x20;            Immutable Graph

&#x20;                  │

&#x20;       ┌──────────┼───────────┐

&#x20;       ▼          ▼           ▼

&#x20;     Video      Audio      Subtitle

&#x20;       │          │           │

&#x20;       └────── MediaTime ─────┘

&#x20;                  │

&#x20;            PlaybackClock





Third Party libraries

&#x20;       │

&#x20;       ▼

&#x20;    Adapters

&#x20;       │

&#x20;       ▼

&#x20;  Stable Contracts





AI

&#x20;│

&#x20;└── Observe Media

&#x20;      NEVER block Media





Every async payload

&#x20;    │

&#x20;    └── SessionId + Epoch





Every extension

&#x20;    │

&#x20;    └── Capability based





Every hot path

&#x20;    │

&#x20;    └── Native





Every product feature

&#x20;    │

&#x20;    └── Managed/Application layer

```



FFmpeg 本身主要是 LGPL，但某些 optional component 會使 build 進入 GPL 條件；libbluray 本身是 LGPL。因此我也會從 Day 1 增加 `ThirdPartyManifest`，固定每個 native build 啟用了什麼 dependency/license，避免商用後才處理授權炸彈。\[FFmpeg](https://ffmpeg.org/doxygen/trunk/md\_README.html?utm\_source=chatgpt.com)



\### 我認為這套架構最重要的地方



不是「未來完全不用重構」——這不可能。



而是把未來重構限制成：



```text

換一個 Adapter

換一個 Graph Node

換一個 Provider

```



而不是：



```text

三年後：



「慘了，時間軸不支援 branching。」

「慘了，bitstream 沒 PCM 給 AI。」

「慘了，FFmpeg type 到處都是。」

「慘了，BD-J callback 直接控制 UI。」

「慘了，Seek 後 AI async result 全亂掉。」



→ 重寫播放器。

```



上面這五個坑，我現在這套設計都已經預先避掉了。



我會把這版視為 \*\*Architecture v0.1 的基準線\*\*。下一步真正應該做的，不是立刻寫播放器，而是先把 \*\*`MediaTime / Epoch / PlaybackSession / GraphPlan / Contracts / Native C ABI` 這六個核心規格寫成 ADR + interface\*\*；它們定完後，才開始接 FFmpeg。這會大幅降低你最擔心的「半年後發現底層假設錯了，整片砍掉」的風險。

