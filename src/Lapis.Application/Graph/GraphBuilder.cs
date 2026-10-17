using System.Collections.Immutable;
using Lapis.Contracts.Capabilities;

namespace Lapis.Application.Graph;

public sealed class GraphBuilder
{
    private readonly List<GraphNode> _nodes = [];
    private readonly HashSet<string> _nodeIds = new(StringComparer.Ordinal);
    private readonly List<GraphEdge> _edges = [];
    private readonly HashSet<(string From, string To)> _edgeIds = [];

    public GraphBuilder AddNode(GraphNode node)
    {
        ArgumentNullException.ThrowIfNull(node);
        if (string.IsNullOrWhiteSpace(node.Id))
            throw new ArgumentException("Graph node id is required.", nameof(node));
        if (!_nodeIds.Add(node.Id))
            throw new InvalidOperationException($"Graph node '{node.Id}' already exists.");
        _nodes.Add(node);
        return this;
    }

    public GraphBuilder Connect(string fromNodeId, string toNodeId)
    {
        if (!_nodeIds.Contains(fromNodeId) || !_nodeIds.Contains(toNodeId))
            throw new InvalidOperationException("Both graph edge endpoints must already exist.");
        if (string.Equals(fromNodeId, toNodeId, StringComparison.Ordinal))
            throw new InvalidOperationException("A graph node cannot connect to itself.");
        if (!_edgeIds.Add((fromNodeId, toNodeId)))
            throw new InvalidOperationException("Duplicate graph edge.");
        _edges.Add(new GraphEdge(fromNodeId, toNodeId));
        return this;
    }

    public GraphPlan Build(MediaCapability availableCapabilities)
    {
        var required = MediaCapability.None;
        foreach (var node in _nodes)
            required |= node.RequiredCapabilities;
        if ((availableCapabilities & required) != required)
            throw new InvalidOperationException($"Graph requires unavailable capabilities: {required & ~availableCapabilities}.");

        // Kahn's algorithm: a media processing graph must be acyclic.
        var degrees = _nodes.ToDictionary(node => node.Id, _ => 0, StringComparer.Ordinal);
        var outgoing = _nodes.ToDictionary(node => node.Id, _ => new List<string>(), StringComparer.Ordinal);
        foreach (var edge in _edges)
        {
            degrees[edge.ToNodeId]++;
            outgoing[edge.FromNodeId].Add(edge.ToNodeId);
        }

        var ready = new Queue<string>(degrees.Where(pair => pair.Value == 0).Select(pair => pair.Key));
        var visited = 0;
        while (ready.TryDequeue(out var id))
        {
            visited++;
            foreach (var next in outgoing[id])
                if (--degrees[next] == 0)
                    ready.Enqueue(next);
        }
        if (visited != _nodes.Count)
            throw new InvalidOperationException("Media graph contains a cycle.");

        return new GraphPlan(Guid.NewGuid(), _nodes.ToImmutableArray(), _edges.ToImmutableArray(), required);
    }
}
