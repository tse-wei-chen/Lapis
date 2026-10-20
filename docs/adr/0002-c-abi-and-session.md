# ADR-0002 — C ABI + Session/Epoch

- Status: Accepted for skeleton; breaking changes permitted until 1.0 release.
- Public ABI: opaque handles, UTF-8 NUL-terminated strings, fixed-width integers, `struct_size` on all public value structs, explicit status error codes。`bdmv_get_abi_version()` 先協商 major，不能直接將 C++ 物件與 `std::string` 暴露給 .NET。
- Lifetime: Caller owns Engine and Session; Engine may be destroyed after Session create in current self-contained test implementation, but real engine-owned resources must define stricter lifetime ownership before FFmpeg integration。**禁止** destroy 與 command 並發。
- Epoch: session start、seek、graph/source discontinuity、close、track swap 遞增；非同步結果與播放位置需帶上 `(session_id, epoch)`，舊代結果丟棄。Epoch 不屬於永久快取 key。
- Ordering: 現有 native mutex 只保證同一 Session 操作互斥，不保證命令依提交順序完成。接入 playback thread 前必須引入 C# Actor Command Mailbox，定義 ticket / completion / cancellation。
- Evolution: `struct_size` 的新版 append-only 規則必須 paired with old-client/new-runtime + new-client/old-runtime tests；目前 `struct_size >= sizeof(current)` 是初版檢查，不代表向前版本可自行協商。Header major=1, minor=0 仍為開發版，尚不承諾第三方 ABI 穩定。
