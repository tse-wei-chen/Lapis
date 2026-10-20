# ADR-0006 — Blu-ray Source / Navigation / Content Protection 分離

- Status: Accepted; no Blu-ray implementation in M0.
- Source: ContentAccessProvider (clear/legally licensed protected) → BluRaySource → virtual clip/playlist byte/packet stream + navigation events。libbluray 僅在 Blu-ray adapter 出現。
- Navigation independent of presentation: playlist/title/chapter/angle/branch / menu/overlay event → command mailbox/overlay compositor。BD-J callback 不得直接操作 GPU 和 C# UI。
- Timeline: virtual 90 kHz timestamps、clip discontinuities、seamless branching/multi-angle、underlying source mapping are explicit; playlist identity part of content fingerprints and AI cache.
- Legal/functional: libbluray by itself does not imply ability to play AACS/BD+ discs. UHD BD, Dolby Vision and certain proprietary audio/video features require separately assessed support/licensing. Unsupported paths must fail clearly without hidden bypasses.
- Java: BD-J JVM version/security-runtime/redistribution feasibility checked before claiming complete menu compatibility.
