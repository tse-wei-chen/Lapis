#include "adapters/ffmpeg/ffmpeg_decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace bdmv::adapters {
namespace {
constexpr AVRational kNanosecondsBase{1, 1'000'000'000};
constexpr int kAudioRate = 48000;
std::string describe(int err) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, text, sizeof(text)) < 0) return "unknown FFmpeg error";
    return text;
}
struct CodecStream final {
    AVCodecContext* codec{nullptr};
    int index{-1};
    AVRational time_base{1, 1};
    void clear() noexcept { avcodec_free_context(&codec); index = -1; }
};

bool init_codec(AVFormatContext* format, AVMediaType type, CodecStream& result,
                std::string& error) {
    const int index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) return true; // no such track
    if (index < 0) { error = "stream selection: " + describe(index); return false; }
    AVStream* stream = format->streams[index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) { error = "unsupported stream codec"; return false; }
    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    if (!codec) { error = "cannot allocate decoder"; return false; }
    int rc = avcodec_parameters_to_context(codec, stream->codecpar);
    if (rc >= 0) rc = avcodec_open2(codec, decoder, nullptr);
    if (rc < 0) {
        error = "open decoder: " + describe(rc);
        avcodec_free_context(&codec);
        return false;
    }
    result.codec = codec;
    result.index = index;
    result.time_base = stream->time_base;
    return true;
}
std::int64_t nanos(std::int64_t ts, AVRational tb) {
    return ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, tb, kNanosecondsBase);
}
} // namespace

struct FfmpegDecoder::Impl final {
    AVFormatContext* format{nullptr};
    CodecStream video;
    CodecStream audio;
    AVPacket* packet{nullptr};
    AVFrame* image{nullptr};
    SwsContext* scaler{nullptr};
    SwrContext* resampler{nullptr};
    AVChannelLayout audio_input_layout{};
    AVSampleFormat audio_input_fmt{AV_SAMPLE_FMT_NONE};
    int audio_input_rate{0};
    bool input_eof{false};
    bool sent_flush{false};
    bool packet_pending{false};
    bool video_flushed{false};
    bool audio_flushed{false};

    ~Impl() {
        sws_freeContext(scaler);
        swr_free(&resampler);
        av_channel_layout_uninit(&audio_input_layout);
        av_packet_free(&packet);
        av_frame_free(&image);
        video.clear();
        audio.clear();
        avformat_close_input(&format);
    }
    bool convert_video(const CodecStream& stream, core::DecodedBuffer& dst,
                       std::string& error) {
        if (image->width <= 0 || image->height <= 0) { error = "invalid image size"; return false; }
        auto expected = core::rgba_buffer_size(static_cast<std::uint32_t>(image->width),
                                               static_cast<std::uint32_t>(image->height));
        if (!expected) { error = "image too large"; return false; }
        scaler = sws_getCachedContext(scaler, image->width, image->height,
                                     static_cast<AVPixelFormat>(image->format),
                                     image->width, image->height, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) { error = "video conversion init failed"; return false; }
        core::DecodedBuffer result;
        result.kind = core::DecodedKind::VideoRgba8;
        result.width = static_cast<std::uint32_t>(image->width);
        result.height = static_cast<std::uint32_t>(image->height);
        result.pts_ns = nanos(image->best_effort_timestamp, stream.time_base);
        result.duration_ns = nanos(image->duration, stream.time_base);
        result.bytes.resize(*expected);
        uint8_t* output[4]{result.bytes.data(), nullptr, nullptr, nullptr};
        const int strides[4]{image->width * 4, 0, 0, 0};
        int rows = sws_scale(scaler, image->data, image->linesize, 0, image->height, output, strides);
        if (rows != image->height) { error = "incomplete video conversion"; return false; }
        dst = std::move(result);
        return true;
    }
    bool convert_audio(const CodecStream& stream, core::DecodedBuffer& dst,
                       std::string& error) {
        if (image->sample_rate <= 0 || image->nb_samples < 0 || image->nb_samples > 1'000'000) {
            error = "invalid audio block"; return false;
        }
        const bool changed = !resampler || audio_input_rate != image->sample_rate ||
            audio_input_fmt != static_cast<AVSampleFormat>(image->format) ||
            av_channel_layout_compare(&audio_input_layout, &image->ch_layout) != 0;
        if (changed) {
            swr_free(&resampler);
            av_channel_layout_uninit(&audio_input_layout);
            AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
            int rc = swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_S16,
                                         kAudioRate, &image->ch_layout,
                                         static_cast<AVSampleFormat>(image->format),
                                         image->sample_rate, 0, nullptr);
            if (rc < 0 || !resampler || (rc = swr_init(resampler)) < 0) {
                error = "audio resampler: " + describe(rc); return false;
            }
            if (av_channel_layout_copy(&audio_input_layout, &image->ch_layout) < 0) {
                error = "audio channel layout copy failed"; return false;
            }
            audio_input_fmt = static_cast<AVSampleFormat>(image->format);
            audio_input_rate = image->sample_rate;
        }
        const int count = swr_get_out_samples(resampler, image->nb_samples);
        if (count < 0 || count > 1'000'000) { error = "invalid resample bound"; return false; }
        core::DecodedBuffer result;
        result.kind = core::DecodedKind::AudioS16Stereo;
        result.pts_ns = nanos(image->best_effort_timestamp, stream.time_base);
        result.sample_rate = kAudioRate;
        result.channels = 2;
        result.bytes.resize(static_cast<std::size_t>(count) * 4);
        uint8_t* out[] = {result.bytes.data()};
        const int planes = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(image->format))
            ? image->ch_layout.nb_channels : 1;
        if (planes <= 0 || planes > 64) { error = "invalid number of audio planes"; return false; }
        std::vector<const uint8_t*> input(static_cast<std::size_t>(planes));
        for (int i = 0; i < planes; ++i) input[static_cast<std::size_t>(i)] = image->extended_data[i];
        int samples = swr_convert(resampler, out, count, input.data(), image->nb_samples);
        if (samples < 0) { error = "audio resampling failed: " + describe(samples); return false; }
        result.bytes.resize(static_cast<std::size_t>(samples) * 4);
        result.duration_ns = av_rescale_q(samples, AVRational{1, kAudioRate}, kNanosecondsBase);
        dst = std::move(result);
        return true;
    }
};

FfmpegDecoder::FfmpegDecoder() : impl_(std::make_unique<Impl>()) {}
FfmpegDecoder::~FfmpegDecoder() = default;

std::unique_ptr<FfmpegDecoder> FfmpegDecoder::open_file(const std::string& path,
                                                       std::string& error) {
    error.clear();
    // Never accept arbitrary FFmpeg protocols (http, crypto, shell-like names).
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
        error = "not a readable local media file";
        return {};
    }
    auto dec = std::unique_ptr<FfmpegDecoder>(new FfmpegDecoder());
    auto& impl = *dec->impl_;
    int rc = avformat_open_input(&impl.format, path.c_str(), nullptr, nullptr);
    if (rc >= 0) rc = avformat_find_stream_info(impl.format, nullptr);
    if (rc < 0) { error = "open media: " + describe(rc); return {}; }
    if (!init_codec(impl.format, AVMEDIA_TYPE_VIDEO, impl.video, error) ||
        !init_codec(impl.format, AVMEDIA_TYPE_AUDIO, impl.audio, error)) return {};
    if (!impl.video.codec && !impl.audio.codec) { error = "no usable video/audio track"; return {}; }
    impl.packet = av_packet_alloc();
    impl.image = av_frame_alloc();
    if (!impl.packet || !impl.image) { error = "cannot allocate decode frames"; return {}; }
    return dec;
}
bool FfmpegDecoder::has_video() const noexcept { return impl_->video.codec != nullptr; }
bool FfmpegDecoder::has_audio() const noexcept { return impl_->audio.codec != nullptr; }

core::DecodeReadStatus FfmpegDecoder::next(core::DecodedBuffer& out, std::string& error) {
    error.clear();
    auto& p = *impl_;
    for (;;) {
        // Drain all available decoder output before submitting more input.
        for (auto* track : {&p.video, &p.audio}) {
            if (!track->codec) continue;
            const int rc = avcodec_receive_frame(track->codec, p.image);
            if (rc == 0) {
                const bool ok = track == &p.video
                    ? p.convert_video(*track, out, error)
                    : p.convert_audio(*track, out, error);
                av_frame_unref(p.image);
                return ok ? core::DecodeReadStatus::Frame : core::DecodeReadStatus::Error;
            }
            if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
                error = "receive decoder frame: " + describe(rc);
                return core::DecodeReadStatus::Error;
            }
        }
        // Retain the original AVPacket on EAGAIN. Dropping it would cause
        // silent A/V gaps; the next iteration first drains receive_frame().
        if (p.packet_pending) {
            AVCodecContext* decoder = nullptr;
            if (p.packet->stream_index == p.video.index) decoder = p.video.codec;
            else if (p.packet->stream_index == p.audio.index) decoder = p.audio.codec;
            const int rc = decoder ? avcodec_send_packet(decoder, p.packet) : 0;
            if (rc == AVERROR(EAGAIN)) continue;
            p.packet_pending = false;
            av_packet_unref(p.packet);
            if (rc < 0) {
                error = "send media packet: " + describe(rc);
                return core::DecodeReadStatus::Error;
            }
            continue;
        }
        if (p.input_eof) {
            bool retry = false;
            for (auto* track : {&p.video, &p.audio}) {
                if (!track->codec) continue;
                bool& flushed = track == &p.video ? p.video_flushed : p.audio_flushed;
                if (flushed) continue;
                const int rc = avcodec_send_packet(track->codec, nullptr);
                if (rc == AVERROR(EAGAIN)) { retry = true; continue; }
                if (rc < 0 && rc != AVERROR_EOF) {
                    error = "flush decoder: " + describe(rc);
                    return core::DecodeReadStatus::Error;
                }
                flushed = true;
            }
            if (retry) continue;
            if (p.sent_flush) return core::DecodeReadStatus::EndOfStream;
            p.sent_flush = true;
            continue; // let both decoders return all delayed frames
        }
        const int rc = av_read_frame(p.format, p.packet);
        if (rc == AVERROR_EOF) { p.input_eof = true; continue; }
        if (rc < 0) {
            error = "read media packet: " + describe(rc);
            return core::DecodeReadStatus::Error;
        }
        p.packet_pending = true;
    }
}

bool FfmpegDecoder::seek(std::int64_t ns, std::string& error) {
    error.clear();
    if (ns < 0) { error = "negative seek time"; return false; }
    auto& p = *impl_;
    const auto timestamp_us = av_rescale_q(ns, kNanosecondsBase, AV_TIME_BASE_Q);
    const int rc = avformat_seek_file(p.format, -1, std::numeric_limits<std::int64_t>::min(),
                                      timestamp_us, std::numeric_limits<std::int64_t>::max(), 0);
    if (rc < 0) { error = "media seek failed: " + describe(rc); return false; }
    if (p.video.codec) avcodec_flush_buffers(p.video.codec);
    if (p.audio.codec) avcodec_flush_buffers(p.audio.codec);
    swr_free(&p.resampler);
    av_channel_layout_uninit(&p.audio_input_layout);
    p.audio_input_fmt = AV_SAMPLE_FMT_NONE;
    p.audio_input_rate = 0;
    av_packet_unref(p.packet);
    av_frame_unref(p.image);
    p.input_eof = false;
    p.sent_flush = false;
    p.packet_pending = false;
    p.video_flushed = false;
    p.audio_flushed = false;
    return true;
}
} // namespace bdmv::adapters
