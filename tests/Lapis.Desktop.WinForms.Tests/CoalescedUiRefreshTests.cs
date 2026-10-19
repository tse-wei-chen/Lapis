using Lapis.Desktop.WinForms;
using Xunit;

namespace Lapis.Desktop.WinForms.Tests;

public sealed class CoalescedUiRefreshTests
{
    [Fact]
    public void BurstUsesSinglePendingUiPostAndReadsLatestValue()
    {
        var posted = new Queue<Action>();
        var displayed = new List<int>();
        var current = 0;
        var dispatcher = new CoalescedUiRefresh(
            callback => posted.Enqueue(callback),
            () => displayed.Add(current));

        for (var i = 1; i <= 1000; i++)
        {
            current = i;
            dispatcher.Signal();
        }

        Assert.Single(posted);
        Assert.Empty(displayed);
        posted.Dequeue()();
        Assert.Equal(new[] { 1000 }, displayed);
        Assert.Empty(posted);
    }

    [Fact]
    public void NotificationsDuringUiRefreshScheduleOneFollowUp()
    {
        var posted = new Queue<Action>();
        var displayed = new List<int>();
        var current = 1;
        CoalescedUiRefresh? dispatcher = null;
        dispatcher = new CoalescedUiRefresh(
            callback => posted.Enqueue(callback),
            () =>
            {
                displayed.Add(current);
                if (displayed.Count == 1)
                {
                    for (var i = 2; i <= 100; i++)
                    {
                        current = i;
                        dispatcher!.Signal();
                    }
                }
            });

        dispatcher.Signal();
        Assert.Single(posted);
        posted.Dequeue()();
        Assert.Single(posted);
        posted.Dequeue()();
        Assert.Equal(new[] { 1, 100 }, displayed);
        Assert.Empty(posted);
    }

    [Fact]
    public void FailedUiPostCanBeRetriedByNextObservation()
    {
        var posted = new Queue<Action>();
        var attempts = 0;
        var delivered = 0;
        var dispatcher = new CoalescedUiRefresh(
            callback =>
            {
                if (++attempts == 1)
                {
                    throw new InvalidOperationException("UI handle unavailable");
                }
                posted.Enqueue(callback);
            },
            () => delivered++);

        Assert.Throws<InvalidOperationException>(() => dispatcher.Signal());
        dispatcher.Signal();
        Assert.Single(posted);
        posted.Dequeue()();
        Assert.Equal(1, delivered);
    }
}
