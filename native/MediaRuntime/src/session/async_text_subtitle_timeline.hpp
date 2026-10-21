#pragma once

#include "core/media_time.hpp"
#include "core/text_subtitle_cue.hpp"
#include "source/media_source.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <stop_token>
#include <vector>

namespace lapis::media {

// A separate, finite local-file subtitle decode generation. It has its own
// source/demux/router and therefore never consumes the live A/V packet queue.
// The presentation thread reads completed snapshots; it never calls FFmpeg.
enum class TextSubtitleTimelineState : std::uint8_t {
    Loading,
    Ready,
    Failed,
    LimitExceeded,
    Cancelled
};

struct TextSubtitleTimelineSnapshot final {
    TextSubtitleTimelineState state{TextSubtitleTimelineState::Loading};
    std::vector<TextSubtitleCue> cues;
};

// One-shot asynchronous snapshot. Recreate after a Seek/Open at a new Epoch;
// no old decoder or pending cue may be reused between generations.
//
// Intentionally restrict to local *file* sources until cancellation support
// for slow physical discs, remote sources and BDMV playlists is verified.
struct TextSubtitleTimelinePage final {
    TextSubtitleTimelineState state{TextSubtitleTimelineState::Loading};
    std::size_t total{};
    std::vector<TextSubtitleCue> cues;
};

class AsyncTextSubtitleTimeline final {
public:
    static constexpr std::size_t MaxCues = 4096;
    static constexpr std::size_t MaxTextBytes = 4 * 1024 * 1024;

    AsyncTextSubtitleTimeline(MediaLocator media, int stream_index,
                              MediaTime start_at = {});
    ~AsyncTextSubtitleTimeline();

    AsyncTextSubtitleTimeline(const AsyncTextSubtitleTimeline&) = delete;
    AsyncTextSubtitleTimeline& operator=(const AsyncTextSubtitleTimeline&) = delete;

    void cancel() noexcept;
    [[nodiscard]] TextSubtitleTimelineSnapshot snapshot() const;
    [[nodiscard]] TextSubtitleTimelinePage read_page(
        std::size_t start, std::size_t limit) const;

private:
    void run(std::stop_token stop) noexcept;
    void publish(TextSubtitleTimelineState state,
                 std::vector<TextSubtitleCue> cues = {}) noexcept;

    MediaLocator locator_;
    int stream_index_;
    MediaTime start_at_;
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    TextSubtitleTimelineSnapshot result_{};
    // Must be destroyed FIRST: it stops/joins before the state above is freed.
    std::jthread worker_;
};

} // namespace lapis::media
