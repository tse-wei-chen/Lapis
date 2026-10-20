#include "adapters/synthetic_source.hpp"
#include <algorithm>
#include <utility>

namespace bdmv::adapters {
core::DecodeReadStatus SyntheticSource::next(core::DecodedBuffer& frame, std::string& error) {
    error.clear();
    if (cursor_ >= 16) return core::DecodeReadStatus::EndOfStream;
    frame = {};
    const auto index = cursor_++;
    frame.pts_ns = static_cast<std::int64_t>(index / 2) * 40'000'000;
    frame.duration_ns = 40'000'000;
    if (index % 2 == 0) {
        frame.kind = core::DecodedKind::VideoRgba8;
        frame.width = 2;
        frame.height = 2;
        frame.bytes.assign(16, static_cast<std::uint8_t>(index));
    } else {
        frame.kind = core::DecodedKind::AudioS16Stereo;
        frame.sample_rate = 48000;
        frame.channels = 2;
        frame.bytes.assign(480 * 4, static_cast<std::uint8_t>(index));
    }
    return core::DecodeReadStatus::Frame;
}
bool SyntheticSource::seek(std::int64_t ns, std::string& error) {
    if (ns < 0) { error = "negative seek"; return false; }
    cursor_ = static_cast<std::uint32_t>(std::min<std::int64_t>(16, (ns / 40'000'000) * 2));
    error.clear();
    return true;
}
} // namespace bdmv::adapters
