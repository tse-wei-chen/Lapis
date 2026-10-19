#pragma once

#include <algorithm>
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
    Custom,
    // Append rather than re-number existing kinds; it represents the
    // real frame scheduler between processing and the video render sink.
    VideoScheduler
};

struct GraphNode final {
    std::string id;
    GraphNodeKind kind;
};

struct GraphEdge final {
    std::string from;
    std::string to;
};

class GraphBuilder;

class GraphPlan final {
public:
    GraphPlan(const GraphPlan&) = default;
    GraphPlan(GraphPlan&&) noexcept = default;
    GraphPlan& operator=(const GraphPlan&) = delete;
    GraphPlan& operator=(GraphPlan&&) = delete;

    [[nodiscard]] const std::vector<GraphNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<GraphEdge>& edges() const noexcept { return edges_; }
    [[nodiscard]] bool contains(GraphNodeKind kind) const noexcept {
        return std::any_of(nodes_.begin(), nodes_.end(),
            [kind](const GraphNode& node) { return node.kind == kind; });
    }

private:
    friend class GraphBuilder;

    GraphPlan(std::vector<GraphNode> nodes, std::vector<GraphEdge> edges)
        : nodes_(std::move(nodes)), edges_(std::move(edges)) {}

    std::vector<GraphNode> nodes_;
    std::vector<GraphEdge> edges_;
};

} // namespace lapis::media
