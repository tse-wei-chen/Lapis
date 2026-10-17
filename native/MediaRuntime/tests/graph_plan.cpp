#include "core/graph_builder.hpp"

#include <cassert>
#include <stdexcept>

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

    assert(plan.nodes().size() == 3);
    assert(plan.edges().size() == 2);

    bool duplicate_rejected = false;
    try {
        GraphBuilder invalid;
        invalid
            .add_node("source", GraphNodeKind::Source)
            .add_node("source", GraphNodeKind::Demuxer);
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    assert(duplicate_rejected);

    bool missing_endpoint_rejected = false;
    try {
        GraphBuilder invalid;
        invalid
            .add_node("source", GraphNodeKind::Source)
            .connect("source", "missing");
    } catch (const std::invalid_argument&) {
        missing_endpoint_rejected = true;
    }
    assert(missing_endpoint_rejected);

    return 0;
}
