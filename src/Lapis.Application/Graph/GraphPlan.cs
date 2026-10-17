using System.Collections.Immutable;
using Lapis.Contracts.Capabilities;

namespace Lapis.Application.Graph;

public enum GraphNodeKind
{
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
}

public sealed record GraphNode(
    string Id,
    GraphNodeKind Kind,
    MediaCapability RequiredCapabilities = MediaCapability.None);

public sealed record GraphEdge(string FromNodeId, string ToNodeId);

/// <summary>
/// Immutable description of a media graph. Runtime graph replacement is modeled
/// by building and preparing a new plan, then switching plans atomically.
/// </summary>
public sealed class GraphPlan
{
    internal GraphPlan(
        Guid id,
        ImmutableArray<GraphNode> nodes,
        ImmutableArray<GraphEdge> edges,
        MediaCapability requiredCapabilities)
    {
        Id = id;
        Nodes = nodes;
        Edges = edges;
        RequiredCapabilities = requiredCapabilities;
    }

    public Guid Id { get; }

    public ImmutableArray<GraphNode> Nodes { get; }

    public ImmutableArray<GraphEdge> Edges { get; }

    public MediaCapability RequiredCapabilities { get; }
}
