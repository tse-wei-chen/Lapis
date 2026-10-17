#include "session/playback_media_session.hpp"

#include "core/graph_builder.hpp"
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

    void reset() {
        if (packet_router != nullptr) {
            packet_router->stop();
        }
        packet_router.reset();
        plan.reset();
        decoder.reset();
        demuxer.reset();
        source.reset();
        video_stream_index = -1;
    }

    MediaSourceRegistry sources;
    std::unique_ptr<MediaSourceSession> source;
    std::unique_ptr<ffmpeg::FfmpegDemuxer> demuxer;
    std::unique_ptr<ffmpeg::FfmpegPacketRouter> packet_router;
    std::unique_ptr<ffmpeg::FfmpegVideoDecoder> decoder;
    std::unique_ptr<GraphPlan> plan;
    int video_stream_index{-1};
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
        impl_->decoder = std::move(decoder);
    }

    impl_->plan = std::make_unique<GraphPlan>(graph_builder.build());
    impl_->demuxer = std::move(demuxer);

    if (impl_->decoder != nullptr) {
        try {
            impl_->packet_router = std::make_unique<ffmpeg::FfmpegPacketRouter>(
                *impl_->demuxer,
                std::vector<int>{impl_->video_stream_index});
            impl_->packet_router->start();
        } catch (...) {
            impl_->reset();
            return {MediaOpenError::PacketRoutingFailed};
        }
    }

    return result;
}

std::optional<VideoFrame> PlaybackMediaSession::decode_next_video_frame(
    std::stop_token stop_token) {
    if (impl_->decoder == nullptr || impl_->packet_router == nullptr) {
        return std::nullopt;
    }

    return impl_->decoder->decode_next(*impl_->packet_router, stop_token);
}

bool PlaybackMediaSession::seek(MediaTime target) {
    if (impl_->demuxer == nullptr) {
        return false;
    }

    if (impl_->packet_router != nullptr) {
        impl_->packet_router->stop();
    }

    if (!impl_->demuxer->seek(target, impl_->video_stream_index)) {
        return false;
    }

    if (impl_->decoder != nullptr) {
        impl_->decoder->flush();
    }

    if (impl_->packet_router != nullptr) {
        try {
            impl_->packet_router->start();
        } catch (...) {
            return false;
        }
    }
    return true;
}

const GraphPlan* PlaybackMediaSession::graph_plan() const noexcept {
    return impl_->plan.get();
}

} // namespace lapis::media
