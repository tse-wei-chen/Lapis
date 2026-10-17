#pragma once

#include "core/graph_plan.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lapis::media {

class GraphBuilder final {
public:
    GraphBuilder& add_node(std::string id, GraphNodeKind kind);
    GraphBuilder& connect(std::string_view from, std::string_view to);
    [[nodiscard]] GraphPlan build() const;

private:
    std::vector<GraphNode> nodes_;
    std::unordered_set<std::string> node_ids_;
    std::vector<GraphEdge> edges_;
};

} // namespace lapis::media
