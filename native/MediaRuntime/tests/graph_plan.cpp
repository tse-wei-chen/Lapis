#include "core/execution_graph_plan.hpp"
#include "core/graph_builder.hpp"

#include <iostream>
#include <stdexcept>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main() {
    using namespace lapis::media;

    GraphBuilder builder;
    const auto plan = builder
        .add_node("source", GraphNodeKind::Source)
        .add_node("demux", GraphNodeKind::Demuxer)
        .add_node("video-decoder", GraphNodeKind::VideoDecoder)
        .connect("source", "demux")
        .connect("demux", "video-decoder")
        .build();

    CHECK(plan.nodes().size() == 3);
    CHECK(plan.edges().size() == 2);

    bool duplicate_node_rejected = false;
    try {
        GraphBuilder invalid;
        invalid
            .add_node("source", GraphNodeKind::Source)
            .add_node("source", GraphNodeKind::Demuxer);
    } catch (const std::invalid_argument&) {
        duplicate_node_rejected = true;
    }
    CHECK(duplicate_node_rejected);

    bool duplicate_edge_rejected = false;
    try {
        GraphBuilder invalid;
        invalid
            .add_node("source", GraphNodeKind::Source)
            .add_node("demux", GraphNodeKind::Demuxer)
            .connect("source", "demux")
            .connect("source", "demux");
    } catch (const std::invalid_argument&) {
        duplicate_edge_rejected = true;
    }
    CHECK(duplicate_edge_rejected);

    bool missing_endpoint_rejected = false;
    try {
        GraphBuilder invalid;
        invalid
            .add_node("source", GraphNodeKind::Source)
            .connect("source", "missing");
    } catch (const std::invalid_argument&) {
        missing_endpoint_rejected = true;
    }
    CHECK(missing_endpoint_rejected);

    bool cycle_rejected = false;
    try {
        GraphBuilder invalid;
        [[maybe_unused]] const auto invalid_plan = invalid
            .add_node("source", GraphNodeKind::Source)
            .add_node("demux", GraphNodeKind::Demuxer)
            .add_node("decoder", GraphNodeKind::VideoDecoder)
            .connect("source", "demux")
            .connect("demux", "decoder")
            .connect("decoder", "source")
            .build();
    } catch (const std::invalid_argument&) {
        cycle_rejected = true;
    }
    CHECK(cycle_rejected);

    // Discovery is not execution: compile selected, reachable branches.
    GraphBuilder both_builder;
    const auto discovered = both_builder
        .add_node("source", GraphNodeKind::Source)
        .add_node("demux", GraphNodeKind::Demuxer)
        .add_node("video-decoder", GraphNodeKind::VideoDecoder)
        .add_node("audio-decoder", GraphNodeKind::AudioDecoder)
        .connect("source", "demux")
        .connect("demux", "video-decoder")
        .connect("demux", "audio-decoder")
        .build();

    const auto video = compile_execution_graph(discovered, {true, false});
    CHECK(video.has_value());
    CHECK(video->nodes().size() == 6);
    CHECK(video->edges().size() == 5);
    CHECK(video->contains(GraphNodeKind::VideoScheduler));
    CHECK(video->contains(GraphNodeKind::VideoSink));
    CHECK(!video->contains(GraphNodeKind::AudioDecoder));
    CHECK(!video->contains(GraphNodeKind::AudioSink));
    CHECK(video->edges().back().from == "video-scheduler");
    CHECK(video->edges().back().to == "video-sink");

    const auto audio = compile_execution_graph(discovered, {false, true});
    CHECK(audio.has_value());
    CHECK(audio->nodes().size() == 5);
    CHECK(audio->edges().size() == 4);
    CHECK(audio->contains(GraphNodeKind::AudioProcessor));
    CHECK(audio->contains(GraphNodeKind::AudioSink));
    CHECK(!audio->contains(GraphNodeKind::VideoDecoder));
    CHECK(!audio->contains(GraphNodeKind::VideoSink));

    const auto both = compile_execution_graph(discovered, {true, true});
    CHECK(both.has_value());
    CHECK(both->nodes().size() == 9);
    CHECK(both->edges().size() == 8);
    CHECK(both->contains(GraphNodeKind::AudioSink));
    CHECK(both->contains(GraphNodeKind::VideoSink));
    CHECK(!compile_execution_graph(discovered, {false, false}).has_value());

    // A decoder that is missing, mislabeled, or unreachable is not runnable.
    CHECK(!compile_execution_graph(plan, {false, true}).has_value());
    GraphBuilder disconnected_builder;
    const auto disconnected = disconnected_builder
        .add_node("source", GraphNodeKind::Source)
        .add_node("demux", GraphNodeKind::Demuxer)
        .add_node("audio-decoder", GraphNodeKind::AudioDecoder)
        .connect("source", "demux")
        .build();
    CHECK(!compile_execution_graph(disconnected, {false, true}).has_value());
    GraphBuilder wrong_type_builder;
    const auto wrong_type = wrong_type_builder
        .add_node("source", GraphNodeKind::Source)
        .add_node("demux", GraphNodeKind::Demuxer)
        .add_node("video-decoder", GraphNodeKind::AudioDecoder)
        .connect("source", "demux")
        .connect("demux", "video-decoder")
        .build();
    CHECK(!compile_execution_graph(wrong_type, {true, false}).has_value());

    return 0;
}
