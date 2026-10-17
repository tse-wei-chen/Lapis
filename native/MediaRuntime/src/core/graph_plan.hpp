#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lapis::media {

enum class GraphNodeKind : std::uint8_t {
    Source,
    Demuxer,
    VideoDecoder,
    AudioDecoder,
    SubtitleDecoder,
    VideoProcessor,
    AudioProcessor,
    SubtitleTimeline,
    Compositor,
    VideoSink,
    AudioSink,
    MediaTap,
    Custom
};

struct GraphNode final {
    std::string id;
    GraphNodeKind kind;
};

struct GraphEdge final {
    std::string from;
    std::string to;
};

class GraphPlan final {
public:
    GraphPlan(std::vector<GraphNode> nodes, std::vector<GraphEdge> edges)
        : nodes_(std::move(nodes)), edges_(std::move(edges)) {}

    [[nodiscard]] const std::vector<GraphNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<GraphEdge>& edges() const noexcept { return edges_; }

private:
    const std::vector<GraphNode> nodes_;
    const std::vector<GraphEdge> edges_;
};

} // namespace lapis::media
