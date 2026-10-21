#include "session/async_text_subtitle_timeline.hpp"

#include "session/playback_media_session.hpp"

#include <algorithm>
#include <utility>

namespace lapis::media {

AsyncTextSubtitleTimeline::AsyncTextSubtitleTimeline(
    MediaLocator media, int stream_index, MediaTime start_at)
    : locator_(std::move(media)),
      stream_index_(stream_index),
      start_at_(start_at),
      worker_([this](std::stop_token token) { run(token); }) {}

AsyncTextSubtitleTimeline::~AsyncTextSubtitleTimeline() {
    cancel();
    // jthread stops and joins before mutex_, result_, locator_ are destroyed.
}

void AsyncTextSubtitleTimeline::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
    worker_.request_stop();
    std::scoped_lock lock(mutex_);
    result_.state = TextSubtitleTimelineState::Cancelled;
    result_.cues.clear();
}

TextSubtitleTimelineSnapshot AsyncTextSubtitleTimeline::snapshot() const {
    std::scoped_lock lock(mutex_);
    return result_;
}

TextSubtitleTimelinePage AsyncTextSubtitleTimeline::read_page(
    std::size_t start, std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    TextSubtitleTimelinePage page{};
    page.state = result_.state;
    if (page.state != TextSubtitleTimelineState::Ready) return page;
    page.total = result_.cues.size();
    if (start >= page.total || limit == 0) return page;

    const auto count = std::min(limit, page.total - start);
    page.cues.insert(page.cues.end(),
        result_.cues.begin() + static_cast<std::ptrdiff_t>(start),
        result_.cues.begin() + static_cast<std::ptrdiff_t>(start + count));
    return page;
}

void AsyncTextSubtitleTimeline::publish(
    TextSubtitleTimelineState state, std::vector<TextSubtitleCue> cues) noexcept {
    std::scoped_lock lock(mutex_);
    if (result_.state == TextSubtitleTimelineState::Cancelled ||
        cancelled_.load(std::memory_order_acquire)) {
        return;
    }
    result_.state = state;
    result_.cues.swap(cues);
}

void AsyncTextSubtitleTimeline::run(std::stop_token stop) noexcept {
    try {
        if (stop.stop_requested()) return;
        if (locator_.kind != MediaLocatorKind::File ||
            stream_index_ < 0 || start_at_.nanoseconds < 0) {
            publish(TextSubtitleTimelineState::Failed);
            return;
        }

        // This entire PlaybackMediaSession is owned by this worker. Live
        // video/audio execution never sees or mutates its FFmpeg state.
        PlaybackMediaSession media;
        if (!media.open(locator_).ok() || stop.stop_requested()) {
            if (!stop.stop_requested())
                publish(TextSubtitleTimelineState::Failed);
            return;
        }
        if (start_at_.nanoseconds != 0 &&
            !media.seek(start_at_)) {
            publish(TextSubtitleTimelineState::Failed);
            return;
        }
        if (stop.stop_requested()) return;
        if (!media.start_decode_generation(
                MediaDecodeSelection{false, false, stream_index_})) {
            publish(TextSubtitleTimelineState::Failed);
            return;
        }

        std::vector<TextSubtitleCue> cues;
        std::size_t text_bytes = 0;
        while (!stop.stop_requested()) {
            auto cue = media.decode_next_text_subtitle_cue(stop);
            if (!cue.has_value()) break;
            if (!cue->valid() || cue->end <= start_at_) continue;
            if (cues.size() >= MaxCues ||
                cue->text.size() > MaxTextBytes - text_bytes) {
                publish(TextSubtitleTimelineState::LimitExceeded);
                return;
            }
            text_bytes += cue->text.size();
            cues.push_back(std::move(*cue));
        }

        if (stop.stop_requested()) return;
        std::stable_sort(cues.begin(), cues.end(),
            [](const TextSubtitleCue& left, const TextSubtitleCue& right) {
                return left.start < right.start;
            });
        publish(TextSubtitleTimelineState::Ready, std::move(cues));
    } catch (...) {
        // Errors must not leak across the thread boundary or into the UI.
        if (!stop.stop_requested())
            publish(TextSubtitleTimelineState::Failed);
    }
}

} // namespace lapis::media
