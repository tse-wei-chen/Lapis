#include "core/graph_builder.hpp"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_map>

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

    const auto duplicate = std::any_of(
        edges_.begin(),
        edges_.end(),
        [&](const GraphEdge& edge) {
            return edge.from == from && edge.to == to;
        });
    if (duplicate) {
        throw std::invalid_argument("A graph edge must be unique.");
    }

    edges_.push_back(GraphEdge{std::string(from), std::string(to)});
    return *this;
}

GraphPlan GraphBuilder::build() const {
    if (nodes_.empty()) {
        throw std::invalid_argument("A graph plan must contain at least one node.");
    }

    std::unordered_map<std::string, std::size_t> indegree;
    std::unordered_map<std::string, std::vector<std::string>> outgoing;
    indegree.reserve(nodes_.size());
    outgoing.reserve(nodes_.size());

    for (const auto& node : nodes_) {
        indegree.emplace(node.id, 0);
        outgoing.emplace(node.id, std::vector<std::string>{});
    }

    for (const auto& edge : edges_) {
        ++indegree.at(edge.to);
        outgoing.at(edge.from).push_back(edge.to);
    }

    std::queue<std::string> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) {
            ready.push(id);
        }
    }

    std::size_t visited = 0;
    while (!ready.empty()) {
        auto id = std::move(ready.front());
        ready.pop();
        ++visited;

        for (const auto& next : outgoing.at(id)) {
            auto& degree = indegree.at(next);
            if (--degree == 0) {
                ready.push(next);
            }
        }
    }

    if (visited != nodes_.size()) {
        throw std::invalid_argument("A media graph must be acyclic.");
    }

    return GraphPlan(nodes_, edges_);
}

} // namespace lapis::media
