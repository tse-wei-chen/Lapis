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

    return 0;
}
