using Lapis.Application.Graph;
using Lapis.Contracts.Capabilities;
using Xunit;

namespace Lapis.Application.Tests;

public sealed class GraphValidationTests
{
    [Fact]
    public void RejectsCycles()
    {
        var graph = new GraphBuilder()
            .AddNode(new GraphNode("a", GraphNodeKind.Source))
            .AddNode(new GraphNode("b", GraphNodeKind.Demuxer))
            .AddNode(new GraphNode("c", GraphNodeKind.VideoDecoder))
            .Connect("a", "b").Connect("b", "c").Connect("c", "a");
        Assert.Throws<InvalidOperationException>(() => graph.Build(MediaCapability.None));
    }

    [Fact]
    public void RejectsDuplicateEdges()
    {
        var graph = new GraphBuilder()
            .AddNode(new GraphNode("a", GraphNodeKind.Source))
            .AddNode(new GraphNode("b", GraphNodeKind.Demuxer))
            .Connect("a", "b");
        Assert.Throws<InvalidOperationException>(() => graph.Connect("a", "b"));
    }

    [Fact]
    public void AllowsBranchedDags()
    {
        var graph = new GraphBuilder()
            .AddNode(new GraphNode("source", GraphNodeKind.Source))
            .AddNode(new GraphNode("video", GraphNodeKind.VideoDecoder))
            .AddNode(new GraphNode("audio", GraphNodeKind.AudioDecoder))
            .Connect("source", "video").Connect("source", "audio");
        var plan = graph.Build(MediaCapability.None);
        Assert.Equal(3, plan.Nodes.Length);
        Assert.Equal(2, plan.Edges.Length);
    }
}
