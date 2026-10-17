#include "core/graph_builder.hpp"

#include <stdexcept>

namespace lapis::media {

GraphBuilder& GraphBuilder::add_node(std::string id, GraphNodeKind kind) {
    if (id.empty()) {
        throw std::invalid_argument("Graph node id cannot be empty.");
    }

    if (!node_ids_.insert(id).second) {
        throw std::invalid_argument("Graph node id must be unique.");
    }

    nodes_.push_back(GraphNode{std::move(id), kind});
    return *this;
}

GraphBuilder& GraphBuilder::connect(std::string_view from, std::string_view to) {
    if (from == to) {
        throw std::invalid_argument("A graph node cannot connect to itself.");
    }

    if (!node_ids_.contains(std::string(from)) || !node_ids_.contains(std::string(to))) {
        throw std::invalid_argument("Both graph edge endpoints must exist before connecting them.");
    }

    edges_.push_back(GraphEdge{std::string(from), std::string(to)});
    return *this;
}

GraphPlan GraphBuilder::build() const {
    if (nodes_.empty()) {
        throw std::invalid_argument("A graph plan must contain at least one node.");
    }

    return GraphPlan(nodes_, edges_);
}

} // namespace lapis::media
