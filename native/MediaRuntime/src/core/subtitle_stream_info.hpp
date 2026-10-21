#pragma once

#include <cstdint>
#include <string>

namespace lapis::media {

// Owned immutable subtitle track metadata. The packet/decode boundary remains
// inside FFmpeg; Core and the public ABI never borrow AVStream/AVCodec pointers.
enum class SubtitleTrackKind : std::uint32_t {
    Unknown = 0,
    Text = 1,
    Bitmap = 2
};

struct SubtitleStreamInfo final {
    std::uint32_t stream_index{};
    SubtitleTrackKind kind{SubtitleTrackKind::Unknown};
    std::string language;
    std::string codec_name;
};

} // namespace lapis::media
