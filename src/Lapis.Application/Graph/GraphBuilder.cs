using System.Collections.Immutable;
using Lapis.Contracts.Capabilities;

namespace Lapis.Application.Graph;

public sealed class GraphBuilder
{
    private readonly List<GraphNode> _nodes = [];
    private readonly HashSet<string> _nodeIds = new(StringComparer.Ordinal);
    private readonly List<GraphEdge> _edges = [];

    public GraphBuilder AddNode(GraphNode node)
    {
        ArgumentNullException.ThrowIfNull(node);
        if (string.IsNullOrWhiteSpace(node.Id))
        {
            throw new ArgumentException("Graph node id is required.", nameof(node));
        }

        if (!_nodeIds.Add(node.Id))
        {
            throw new InvalidOperationException($"Graph node '{node.Id}' already exists.");
        }

        _nodes.Add(node);
        return this;
    }

    public GraphBuilder Connect(string fromNodeId, string toNodeId)
    {
        if (!_nodeIds.Contains(fromNodeId) || !_nodeIds.Contains(toNodeId))
        {
            throw new InvalidOperationException("Both graph edge endpoints must already exist.");
        }

        if (string.Equals(fromNodeId, toNodeId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException("A graph node cannot connect to itself.");
        }

        _edges.Add(new GraphEdge(fromNodeId, toNodeId));
        return this;
    }

    public GraphPlan Build(MediaCapability availableCapabilities)
    {
        var required = MediaCapability.None;
        foreach (var node in _nodes)
        {
            required |= node.RequiredCapabilities;
        }

        if ((availableCapabilities & required) != required)
        {
            var missing = required & ~availableCapabilities;
            throw new InvalidOperationException($"Graph requires unavailable capabilities: {missing}.");
        }

        return new GraphPlan(
            Guid.NewGuid(),
            _nodes.ToImmutableArray(),
            _edges.ToImmutableArray(),
            required);
    }
}
