#pragma once

#include "core/graph_builder.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace lapis::media {

// A discovery graph describes streams that can be decoded. An execution graph
// includes only media branches that have a configured downstream consumer.
// Compile a fresh immutable topology at each playback generation boundary;
// never mutate the running plan in place.
struct ExecutionGraphSelection final {
    bool video{};
    bool audio{};
};

[[nodiscard]] inline std::optional<GraphPlan> compile_execution_graph(
    const GraphPlan& discovered,
    ExecutionGraphSelection selection) {
    if (!selection.video && !selection.audio) {
        return std::nullopt;
    }

    const auto has_node = [&](std::string_view id, GraphNodeKind kind) {
        return std::any_of(discovered.nodes().begin(), discovered.nodes().end(),
            [&](const GraphNode& node) {
                return node.id == id && node.kind == kind;
            });
    };
    const auto has_edge = [&](std::string_view from, std::string_view to) {
        return std::any_of(discovered.edges().begin(), discovered.edges().end(),
            [&](const GraphEdge& edge) {
                return edge.from == from && edge.to == to;
            });
    };

    // A selected stream must actually be reachable from the declared source.
    // Do not synthesize an executable decoder just because a boolean says so.
    if (!has_node("source", GraphNodeKind::Source) ||
        !has_node("demux", GraphNodeKind::Demuxer) ||
        !has_edge("source", "demux") ||
        (selection.video &&
            (!has_node("video-decoder", GraphNodeKind::VideoDecoder) ||
             !has_edge("demux", "video-decoder"))) ||
        (selection.audio &&
            (!has_node("audio-decoder", GraphNodeKind::AudioDecoder) ||
             !has_edge("demux", "audio-decoder")))) {
        return std::nullopt;
    }

    GraphBuilder builder;
    builder
        .add_node("source", GraphNodeKind::Source)
        .add_node("demux", GraphNodeKind::Demuxer)
        .connect("source", "demux");

    if (selection.video) {
        builder
            .add_node("video-decoder", GraphNodeKind::VideoDecoder)
            .add_node("video-process", GraphNodeKind::VideoProcessor)
            .add_node("video-scheduler", GraphNodeKind::VideoScheduler)
            .add_node("video-sink", GraphNodeKind::VideoSink)
            .connect("demux", "video-decoder")
            .connect("video-decoder", "video-process")
            .connect("video-process", "video-scheduler")
            .connect("video-scheduler", "video-sink");
    }

    if (selection.audio) {
        builder
            .add_node("audio-decoder", GraphNodeKind::AudioDecoder)
            .add_node("audio-process", GraphNodeKind::AudioProcessor)
            .add_node("audio-sink", GraphNodeKind::AudioSink)
            .connect("demux", "audio-decoder")
            .connect("audio-decoder", "audio-process")
            .connect("audio-process", "audio-sink");
    }

    return builder.build();
}

} // namespace lapis::media
