#include "session/playback_media_session.hpp"

#include "core/graph_builder.hpp"
#include "decode/ffmpeg/ffmpeg_audio_decoder.hpp"
#include "decode/ffmpeg/ffmpeg_video_decoder.hpp"
#include "demux/ffmpeg/ffmpeg_demuxer.hpp"
#include "demux/ffmpeg/ffmpeg_packet_router.hpp"
#include "source/file/file_source.hpp"
#include "source/media_source_registry.hpp"

#include <memory>
#include <vector>

namespace lapis::media {

class PlaybackMediaSession::Impl final {
public:
    Impl() {
        sources.add(std::make_unique<FileSourceProvider>());
    }

    void stop_decode_generation() noexcept {
        if (packet_router != nullptr) {
            packet_router->stop();
            packet_router.reset();
        }
        if (video_decoder != nullptr) {
            video_decoder->flush();
        }
        if (audio_decoder != nullptr) {
            audio_decoder->flush();
        }
    }

    void reset() noexcept {
        stop_decode_generation();
        plan.reset();
        audio_decoder.reset();
        video_decoder.reset();
        demuxer.reset();
        source.reset();
        video_stream_index = -1;
        audio_stream_index = -1;
    }

    MediaSourceRegistry sources;
    std::unique_ptr<MediaSourceSession> source;
    std::unique_ptr<ffmpeg::FfmpegDemuxer> demuxer;
    std::unique_ptr<ffmpeg::FfmpegPacketRouter> packet_router;
    std::unique_ptr<ffmpeg::FfmpegVideoDecoder> video_decoder;
    std::unique_ptr<ffmpeg::FfmpegAudioDecoder> audio_decoder;
    std::unique_ptr<GraphPlan> plan;
    int video_stream_index{-1};
    int audio_stream_index{-1};
};

PlaybackMediaSession::PlaybackMediaSession()
    : impl_(std::make_unique<Impl>()) {}

PlaybackMediaSession::~PlaybackMediaSession() = default;

MediaOpenInfo PlaybackMediaSession::open(const MediaLocator& locator) {
    impl_->reset();

    auto source_result = impl_->sources.open(locator);
    if (!source_result.ok()) {
        return {
            source_result.error == SourceError::UnsupportedLocator
                ? MediaOpenError::UnsupportedSource
                : MediaOpenError::SourceOpenFailed
        };
    }

    impl_->source = std::move(source_result.session);
    auto demuxer = std::make_unique<ffmpeg::FfmpegDemuxer>();
    if (!demuxer->open(*impl_->source->stream, impl_->source->demux_hint)) {
        impl_->reset();
        return {MediaOpenError::DemuxFailed};
    }

    GraphBuilder graph_builder;
    graph_builder
        .add_node("source", GraphNodeKind::Source)
        .add_node("demux", GraphNodeKind::Demuxer)
        .connect("source", "demux");

    MediaOpenInfo result{};
    result.duration = demuxer->duration();

    const auto video_stream = demuxer->best_video_stream();
    if (video_stream.has_value()) {
        auto decoder = std::make_unique<ffmpeg::FfmpegVideoDecoder>();
        if (!decoder->open(*video_stream)) {
            impl_->reset();
            return {MediaOpenError::DecoderFailed};
        }

        graph_builder
            .add_node("video-decoder", GraphNodeKind::VideoDecoder)
            .connect("demux", "video-decoder");

        result.has_video = true;
        result.video_width = video_stream->width > 0
            ? static_cast<std::uint32_t>(video_stream->width)
            : 0;
        result.video_height = video_stream->height > 0
            ? static_cast<std::uint32_t>(video_stream->height)
            : 0;
        impl_->video_stream_index = video_stream->index;
        impl_->video_decoder = std::move(decoder);
    }

    const auto audio_stream = demuxer->best_audio_stream();
    if (audio_stream.has_value()) {
        auto decoder = std::make_unique<ffmpeg::FfmpegAudioDecoder>();
        if (!decoder->open(*audio_stream)) {
            impl_->reset();
            return {MediaOpenError::DecoderFailed};
        }

        graph_builder
            .add_node("audio-decoder", GraphNodeKind::AudioDecoder)
            .connect("demux", "audio-decoder");

        result.has_audio = true;
        result.audio_sample_rate = audio_stream->sample_rate > 0
            ? static_cast<std::uint32_t>(audio_stream->sample_rate)
            : 0;
        result.audio_channel_count = audio_stream->channel_count > 0
            ? static_cast<std::uint32_t>(audio_stream->channel_count)
            : 0;
        impl_->audio_stream_index = audio_stream->index;
        impl_->audio_decoder = std::move(decoder);
    }

    impl_->plan = std::make_unique<GraphPlan>(graph_builder.build());
    impl_->demuxer = std::move(demuxer);
    return result;
}

bool PlaybackMediaSession::start_decode_generation(MediaDecodeSelection selection) {
    if (impl_->demuxer == nullptr || impl_->packet_router != nullptr ||
        (!selection.video && !selection.audio)) {
        return false;
    }

    std::vector<int> streams;
    streams.reserve(2);

    if (selection.video) {
        if (impl_->video_decoder == nullptr || impl_->video_stream_index < 0) {
            return false;
        }
        streams.push_back(impl_->video_stream_index);
    }

    if (selection.audio) {
        if (impl_->audio_decoder == nullptr || impl_->audio_stream_index < 0) {
            return false;
        }
        streams.push_back(impl_->audio_stream_index);
    }

    auto router = std::make_unique<ffmpeg::FfmpegPacketRouter>(
        *impl_->demuxer,
        std::move(streams));
    router->start();
    impl_->packet_router = std::move(router);
    return true;
}

void PlaybackMediaSession::stop_decode_generation() noexcept {
    impl_->stop_decode_generation();
}

bool PlaybackMediaSession::has_video() const noexcept {
    return impl_->video_decoder != nullptr && impl_->video_stream_index >= 0;
}

bool PlaybackMediaSession::has_audio() const noexcept {
    return impl_->audio_decoder != nullptr && impl_->audio_stream_index >= 0;
}

std::optional<VideoFrame> PlaybackMediaSession::decode_next_video_frame(
    std::stop_token stop_token) {
    if (impl_->video_decoder == nullptr || impl_->packet_router == nullptr) {
        return std::nullopt;
    }

    return impl_->video_decoder->decode_next(*impl_->packet_router, stop_token);
}

std::optional<AudioFrame> PlaybackMediaSession::decode_next_audio_frame(
    std::stop_token stop_token) {
    if (impl_->audio_decoder == nullptr || impl_->packet_router == nullptr) {
        return std::nullopt;
    }

    return impl_->audio_decoder->decode_next(*impl_->packet_router, stop_token);
}

bool PlaybackMediaSession::seek(MediaTime target) {
    if (impl_->demuxer == nullptr) {
        return false;
    }

    // A discontinuity always ends the current packet generation. Decoders are
    // flushed before the demux position changes so no buffered frame can cross
    // the next PlaybackEpoch.
    impl_->stop_decode_generation();

    const int reference_stream = impl_->video_stream_index >= 0
        ? impl_->video_stream_index
        : impl_->audio_stream_index;
    if (!impl_->demuxer->seek(target, reference_stream)) {
        return false;
    }

    return true;
}

const GraphPlan* PlaybackMediaSession::graph_plan() const noexcept {
    return impl_->plan.get();
}

} // namespace lapis::media
