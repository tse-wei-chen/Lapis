namespace Lapis.Contracts.Playback;

public enum TransportState
{
    Closed,
    Opening,
    Ready,
    Playing,
    Paused,
    Seeking,
    Ended,
    Faulted
}

public enum NavigationState
{
    None,
    Feature,
    Menu,
    Popup,
    Still
}

public enum OutputState
{
    Ready,
    Reconfiguring,
    DeviceLost
}

public enum AiState
{
    Off,
    Warming,
    Active,
    Degraded
}
