#include "decode/ffmpeg/ffmpeg_text_subtitle_decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace lapis::media::ffmpeg {
namespace {
constexpr std::size_t max_cue_bytes = 4096;

std::string_view ass_payload(std::string_view line) {
    // AVSubtitleRect::ass holds FFmpeg's 8-comma ASS event format, not
    // text-only input. Never render layer/style/margins as subtitle text.
    for (int field = 0; field < 8; ++field) {
        const auto comma = line.find(',');
        if (comma == std::string_view::npos) return {};
        line.remove_prefix(comma + 1);
    }
    return line;
}

std::string strip_subtitle_markup(std::string_view input) {
    std::string out;
    out.reserve(input.size() < max_cue_bytes ? input.size() : max_cue_bytes);
    bool override_tag = false;
    bool html_tag = false;
    for (std::size_t i = 0; i < input.size() && out.size() < max_cue_bytes; ++i) {
        const auto ch = input[i];
        if (!html_tag && ch == '{') { override_tag = true; continue; }
        if (override_tag) {
            if (ch == '}') override_tag = false;
            continue;
        }
        if (!html_tag && ch == '<') { html_tag = true; continue; }
        if (html_tag) {
            if (ch == '>') html_tag = false;
            continue;
        }
        if (ch == '\\' && i + 1 < input.size()) {
            const auto next = input[i + 1];
            if (next == 'N' || next == 'n') {
                out.push_back('\n'); ++i; continue;
            }
            if (next == 'h') {
                out.push_back(' '); ++i; continue;
            }
        }
        if (ch == '\r') continue;
        if (ch == '\0') break;
        out.push_back(ch);
    }
    // Remove blank text; preserve meaningful embedded newlines.
    const auto first = out.find_first_not_of(" \t\n");
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(" \t\n");
    return out.substr(first, last - first + 1);
}

bool safe_subtract(std::int64_t timestamp, std::int64_t origin,
                   std::int64_t& result) {
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    if ((origin > 0 && timestamp < low + origin) ||
        (origin < 0 && timestamp > high + origin)) return false;
    result = timestamp - origin;
    return true;
}

std::optional<TextSubtitleCue> cue_from_subtitle(
    const AVSubtitle& subtitle, const AVPacket& packet,
    AVRational time_base, MediaTime origin) {
    std::string text;
    for (unsigned i = 0; i < subtitle.num_rects; ++i) {
        const auto* rect = subtitle.rects != nullptr ? subtitle.rects[i] : nullptr;
        if (rect == nullptr || (rect->type != SUBTITLE_ASS &&
                                rect->type != SUBTITLE_TEXT)) continue;

        std::string_view raw;
        if (rect->text != nullptr) raw = rect->text;
        else if (rect->ass != nullptr) raw = ass_payload(rect->ass);
        const auto line = strip_subtitle_markup(raw);
        if (line.empty()) continue;
        if (!text.empty()) text.push_back('\n');
        if (text.size() + line.size() > max_cue_bytes) return std::nullopt;
        text += line;
    }
    if (text.empty()) return std::nullopt;

    // AVSubtitle::pts is in AV_TIME_BASE units; packet.pts is stream-specific.
    const auto absolute = subtitle.pts != AV_NOPTS_VALUE
        ? av_rescale_q(subtitle.pts, AV_TIME_BASE_Q, AVRational{1, 1'000'000'000})
        : packet.pts != AV_NOPTS_VALUE
            ? av_rescale_q(packet.pts, time_base, AVRational{1, 1'000'000'000})
            : AV_NOPTS_VALUE;
    if (absolute == AV_NOPTS_VALUE) return std::nullopt;

    std::int64_t relative{};
    if (!safe_subtract(absolute, origin.nanoseconds, relative)) return std::nullopt;
    const auto ms_to_ns = [](std::uint32_t ms) -> std::int64_t {
        return static_cast<std::int64_t>(ms) * 1'000'000;
    };
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    const auto start_delay = ms_to_ns(subtitle.start_display_time);
    if (relative > max - start_delay) return std::nullopt;
    const auto start = relative + start_delay;

    std::int64_t end_delay = ms_to_ns(subtitle.end_display_time);
    if (end_delay <= start_delay && packet.duration > 0) {
        end_delay = av_rescale_q(packet.duration, time_base,
                               AVRational{1, 1'000'000'000});
    }
    if (end_delay <= start_delay || relative > max - end_delay) return std::nullopt;
    TextSubtitleCue cue{MediaTime{start}, MediaTime{relative + end_delay},
                        std::move(text)};
    return cue.valid() ? std::optional<TextSubtitleCue>(std::move(cue))
                       : std::nullopt;
}
} // namespace

FfmpegTextSubtitleDecoder::~FfmpegTextSubtitleDecoder() {
    avcodec_free_context(&codec_);
}

bool FfmpegTextSubtitleDecoder::open(const FfmpegSubtitleStreamInfo& stream) {
    if (codec_ != nullptr || stream.index < 0 ||
        stream.codec_parameters == nullptr ||
        stream.codec_parameters->codec_type != AVMEDIA_TYPE_SUBTITLE ||
        (stream.codec_parameters->codec_id != AV_CODEC_ID_SUBRIP &&
         stream.codec_parameters->codec_id != AV_CODEC_ID_MOV_TEXT))
        return false;

    const auto* decoder = avcodec_find_decoder(stream.codec_parameters->codec_id);
    if (decoder == nullptr) return false;
    auto* codec = avcodec_alloc_context3(decoder);
    if (codec == nullptr) return false;
    if (avcodec_parameters_to_context(codec, stream.codec_parameters) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        return false;
    }
    codec_ = codec;
    stream_index_ = stream.index;
    time_base_ = stream.time_base;
    timestamp_origin_ = stream.timestamp_origin;
    return true;
}

std::optional<TextSubtitleCue> FfmpegTextSubtitleDecoder::decode_next(
    FfmpegPacketRouter& router, std::stop_token stop) {
    if (codec_ == nullptr) return std::nullopt;
    while (!stop.stop_requested()) {
        auto next = router.read(stream_index_, stop);
        if (!next.has_packet()) return std::nullopt;
        AVSubtitle subtitle{};
        int got = 0;
        const auto result = avcodec_decode_subtitle2(
            codec_, &subtitle, &got, &next.packet->get());
        std::optional<TextSubtitleCue> cue;
        if (result >= 0 && got != 0)
            cue = cue_from_subtitle(subtitle, next.packet->get(),
                                    time_base_, timestamp_origin_);
        if (got != 0) avsubtitle_free(&subtitle);
        if (cue.has_value()) return cue;
    }
    return std::nullopt;
}

void FfmpegTextSubtitleDecoder::flush() noexcept {
    if (codec_ != nullptr) avcodec_flush_buffers(codec_);
}
} // namespace lapis::media::ffmpeg
