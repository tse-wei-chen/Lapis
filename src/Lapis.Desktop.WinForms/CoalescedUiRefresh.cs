namespace Lapis.Desktop.WinForms;

/// <summary>
/// Coalesces bursty background notifications into at most one pending UI post.
/// The refresh delegate must read current state when invoked, rather than
/// capturing an already stale snapshot at signal time.
/// </summary>
internal sealed class CoalescedUiRefresh
{
    private readonly Action<Action> _post;
    private readonly Action _refresh;
    private int _scheduled;
    private int _dirty;

    public CoalescedUiRefresh(Action<Action> post, Action refresh)
    {
        _post = post ?? throw new ArgumentNullException(nameof(post));
        _refresh = refresh ?? throw new ArgumentNullException(nameof(refresh));
    }

    public void Signal()
    {
        Volatile.Write(ref _dirty, 1);
        Schedule();
    }

    private void Schedule()
    {
        if (Interlocked.CompareExchange(ref _scheduled, 1, 0) != 0)
        {
            return;
        }

        try
        {
            _post(Deliver);
        }
        catch
        {
            // BeginInvoke may fail during HWND teardown. A later signal
            // must be able to retry instead of permanently wedging the gate.
            Volatile.Write(ref _scheduled, 0);
            throw;
        }
    }

    private void Deliver()
    {
        Interlocked.Exchange(ref _dirty, 0);
        try
        {
            _refresh();
        }
        finally
        {
            Volatile.Write(ref _scheduled, 0);
            // A signal that arrived while refreshing must not be lost.
            // Schedule exactly one more post, not one per observation.
            if (Volatile.Read(ref _dirty) != 0)
            {
                Schedule();
            }
        }
    }
}
