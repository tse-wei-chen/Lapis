#include "lapis/media_runtime.h"

#include "execution/playback_execution.hpp"
#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"
#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"
#include "session/playback_media_session.hpp"
#include "source/media_source.hpp"
#include "video/video_presenter.hpp"
#include "video/video_output_surface.hpp"

#if defined(_WIN32)
#include "audio/wasapi/wasapi_shared_audio_output_provider.hpp"
#include "video/d3d11/d3d11_video_presenter.hpp"
#endif

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

struct lapis_runtime final {};

namespace {

#if !defined(_WIN32)
class HeadlessVideoPresenter final : public lapis::media::IVideoPresenter {
public:
    [[nodiscard]] lapis::media::VideoPresenterBeginStatus begin(
        lapis::media::PlaybackEpoch) override {
        return lapis::media::VideoPresenterBeginStatus::Ready;
    }

    [[nodiscard]] lapis::media::VideoPresentStatus present(
        const lapis::media::VideoFrame&,
        const lapis::media::VideoPresentContext&) override {
        return lapis::media::VideoPresentStatus::Presented;
    }

    void end() noexcept override {}
};
#endif

struct RuntimePlaybackPipeline final {
    std::shared_ptr<lapis::media::PlaybackMediaSession> media;
    std::unique_ptr<lapis::media::PlaybackExecution> execution;
};

} // namespace

struct lapis_session final {
    std::mutex mutex;
    // Result-returning control commands are serialized at the C ABI boundary.
    // PlaybackExecution remains independently synchronized, while snapshots only
    // take the short-lived state mutex below.
    std::mutex command_mutex;
    lapis_playback_session_id session_id{};
    uint64_t epoch{0};
    int64_t position_ns{0};
    lapis_transport_state transport{LAPIS_TRANSPORT_CLOSED};
    std::shared_ptr<RuntimePlaybackPipeline> pipeline;
    lapis::media::VideoOutputSurfaceDescriptor desired_output_surface{};
};

namespace {

template <typename TAction>
lapis_result guarded_result(TAction&& action) noexcept {
    try {
        return std::forward<TAction>(action)();
    } catch (const std::bad_alloc&) {
        return LAPIS_E_OUT_OF_MEMORY;
    } catch (...) {
        return LAPIS_E_INTERNAL;
    }
}

bool advances_epoch(const lapis_session& session, lapis_playback_epoch epoch) noexcept {
    return epoch.value > session.epoch;
}

bool matches_epoch(const lapis_session& session, lapis_playback_epoch epoch) noexcept {
    return epoch.value == session.epoch;
}

void mark_faulted_if_current(
    lapis_session* session,
    lapis_playback_epoch epoch) noexcept {
    if (session == nullptr) {
        return;
    }

    try {
        std::scoped_lock lock(session->mutex);
        if (matches_epoch(*session, epoch)) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
        }
    } catch (...) {
        // This helper is used while translating an exception across the C ABI.
        // Never allow a secondary synchronization failure to escape.
    }
}

bool valid_session_id(const lapis_playback_session_id& session_id) noexcept {
    return std::any_of(
        std::begin(session_id.bytes),
        std::end(session_id.bytes),
        [](uint8_t value) { return value != 0; });
}

std::optional<lapis::media::MediaLocatorKind> to_core_locator_kind(
    lapis_media_locator_kind kind) noexcept {
    using lapis::media::MediaLocatorKind;

    switch (kind) {
        case LAPIS_LOCATOR_FILE:
            return MediaLocatorKind::File;
        case LAPIS_LOCATOR_DIRECTORY:
            return MediaLocatorKind::Directory;
        case LAPIS_LOCATOR_URI:
            return MediaLocatorKind::Uri;
        case LAPIS_LOCATOR_OPTICAL_DISC:
            return MediaLocatorKind::OpticalDisc;
    }

    return std::nullopt;
}

std::optional<lapis::media::VideoOutputSurfaceDescriptor> to_core_output_surface(
    lapis_video_output_surface surface) noexcept {
    using lapis::media::VideoOutputSurfaceDescriptor;
    using lapis::media::VideoOutputSurfaceKind;
    VideoOutputSurfaceDescriptor descriptor{};
    switch (surface.kind) {
        case LAPIS_VIDEO_OUTPUT_SURFACE_NONE:
            descriptor.kind = VideoOutputSurfaceKind::None;
            break;
        case LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW:
            descriptor.kind = VideoOutputSurfaceKind::Win32Window;
            break;
        default:
            return std::nullopt;
    }
    descriptor.native_handle = surface.native_handle;
    if (!lapis::media::valid_video_output_surface(descriptor)) {
        return std::nullopt;
    }
    return descriptor;
}

lapis_result map_surface_control_error(
    lapis::media::VideoOutputSurfaceControlStatus status) noexcept {
    using lapis::media::VideoOutputSurfaceControlStatus;
    switch (status) {
        case VideoOutputSurfaceControlStatus::Applied:
        case VideoOutputSurfaceControlStatus::Deferred:
            return LAPIS_OK;
        case VideoOutputSurfaceControlStatus::Invalid:
            return LAPIS_E_INVALID_ARGUMENT;
        case VideoOutputSurfaceControlStatus::Unsupported:
            return LAPIS_E_UNSUPPORTED;
        case VideoOutputSurfaceControlStatus::Unavailable:
            return LAPIS_E_OUTPUT_UNAVAILABLE;
        case VideoOutputSurfaceControlStatus::Failed:
            return LAPIS_E_INTERNAL;
    }
    return LAPIS_E_INTERNAL;
}

lapis_result map_open_error(lapis::media::MediaOpenError error) noexcept {
    using lapis::media::MediaOpenError;
    switch (error) {
        case MediaOpenError::None:
            return LAPIS_OK;
        case MediaOpenError::UnsupportedSource:
            return LAPIS_E_UNSUPPORTED;
        case MediaOpenError::SourceOpenFailed:
        case MediaOpenError::DemuxFailed:
        case MediaOpenError::DecoderFailed:
        case MediaOpenError::PacketRoutingFailed:
            return LAPIS_E_INTERNAL;
    }

    return LAPIS_E_INTERNAL;
}

lapis_result map_execution_error(
    lapis::media::PlaybackExecutionError error) noexcept {
    using lapis::media::PlaybackExecutionError;
    switch (error) {
        case PlaybackExecutionError::None:
            return LAPIS_OK;
        case PlaybackExecutionError::InvalidArgument:
            return LAPIS_E_INVALID_ARGUMENT;
        case PlaybackExecutionError::InvalidState:
            return LAPIS_E_INVALID_STATE;
        case PlaybackExecutionError::StaleEpoch:
            return LAPIS_E_STALE_EPOCH;
        case PlaybackExecutionError::MediaSeekFailed:
        case PlaybackExecutionError::ClockFailure:
        case PlaybackExecutionError::WorkerStartFailed:
        case PlaybackExecutionError::WorkerFailed:
            return LAPIS_E_INTERNAL;
    }

    return LAPIS_E_INTERNAL;
}

std::unique_ptr<lapis::media::PlaybackExecution> create_execution(
    lapis::media::PlaybackMediaSession& media) {
    auto video_processor =
        std::make_unique<lapis::media::ffmpeg::FfmpegVideoFrameConverter>();
    #if defined(_WIN32)
    auto presenter = std::make_unique<lapis::media::d3d11::D3D11VideoPresenter>();
    #else
    auto presenter = std::make_unique<HeadlessVideoPresenter>();
    #endif

#if defined(_WIN32)
    auto audio_provider = std::make_unique<
        lapis::media::wasapi::WasapiSharedAudioOutputProvider>();
    lapis::media::AudioFrameProcessorFactory audio_processor_factory =
        [](const lapis::media::AudioFormat& target_format)
            -> std::unique_ptr<lapis::media::IAudioFrameProcessor> {
            return std::make_unique<
                lapis::media::ffmpeg::FfmpegAudioFrameConverter>(
                target_format);
        };

    return std::make_unique<lapis::media::PlaybackExecution>(
        media,
        std::move(video_processor),
        std::move(presenter),
        std::move(audio_provider),
        std::move(audio_processor_factory));
#else
    return std::make_unique<lapis::media::PlaybackExecution>(
        media,
        std::move(video_processor),
        std::move(presenter));
#endif
}

std::shared_ptr<RuntimePlaybackPipeline> create_pipeline(
    std::shared_ptr<lapis::media::PlaybackMediaSession> media) {
    auto pipeline = std::make_shared<RuntimePlaybackPipeline>();
    pipeline->media = std::move(media);
    pipeline->execution = create_execution(*pipeline->media);
    return pipeline;
}

lapis_transport_state transport_from_execution_state(
    lapis::media::PlaybackExecutionState state,
    lapis_transport_state fallback) noexcept {
    using lapis::media::PlaybackExecutionState;
    switch (state) {
        case PlaybackExecutionState::Running:
            return LAPIS_TRANSPORT_PLAYING;
        case PlaybackExecutionState::Paused:
            return LAPIS_TRANSPORT_PAUSED;
        case PlaybackExecutionState::Ended:
            return LAPIS_TRANSPORT_ENDED;
        case PlaybackExecutionState::Faulted:
            return LAPIS_TRANSPORT_FAULTED;
        case PlaybackExecutionState::Stopped:
            return fallback;
    }

    return fallback;
}

bool terminal_transport(lapis_transport_state transport) noexcept {
    return transport == LAPIS_TRANSPORT_ENDED ||
        transport == LAPIS_TRANSPORT_FAULTED;
}

bool active_transport(lapis_transport_state transport) noexcept {
    return transport == LAPIS_TRANSPORT_PLAYING ||
        transport == LAPIS_TRANSPORT_PAUSED;
}

// Control commands call this while holding command_mutex. Natural completion is
// asynchronous, so publish it through the same single-writer command path before
// validating the next mutation. Snapshot reads stay observational and never write
// session transport behind a command's back.
void refresh_terminal_transport(lapis_session* session) {
    std::shared_ptr<RuntimePlaybackPipeline> pipeline;
    lapis_transport_state observed_transport{};
    uint64_t observed_epoch{};

    {
        std::scoped_lock lock(session->mutex);
        if (!active_transport(session->transport) ||
            session->pipeline == nullptr ||
            session->pipeline->execution == nullptr) {
            return;
        }

        pipeline = session->pipeline;
        observed_transport = session->transport;
        observed_epoch = session->epoch;
    }

    const auto execution_snapshot = pipeline->execution->snapshot();
    const auto effective_transport = transport_from_execution_state(
        execution_snapshot.state,
        observed_transport);
    if (!terminal_transport(effective_transport)) {
        return;
    }

    std::scoped_lock lock(session->mutex);
    if (session->epoch != observed_epoch ||
        session->pipeline != pipeline ||
        !active_transport(session->transport)) {
        return;
    }

    session->position_ns = execution_snapshot.clock.position.nanoseconds;
    session->transport = effective_transport;
}

} // namespace

uint32_t lapis_runtime_get_abi_version(void) {
    return LAPIS_MEDIA_RUNTIME_ABI_VERSION;
}

lapis_result lapis_runtime_create(lapis_runtime** out_runtime) {
    return guarded_result([&]() -> lapis_result {
        if (out_runtime == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        *out_runtime = new (std::nothrow) lapis_runtime{};
        return *out_runtime == nullptr ? LAPIS_E_OUT_OF_MEMORY : LAPIS_OK;
    });
}

void lapis_runtime_destroy(lapis_runtime* runtime) {
    delete runtime;
}

lapis_result lapis_session_create(
    lapis_runtime* runtime,
    lapis_playback_session_id session_id,
    lapis_session** out_session) {
    return guarded_result([&]() -> lapis_result {
        if (runtime == nullptr || out_session == nullptr || !valid_session_id(session_id)) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        auto* session = new (std::nothrow) lapis_session{};
        if (session == nullptr) {
            return LAPIS_E_OUT_OF_MEMORY;
        }

        session->session_id = session_id;
        *out_session = session;
        return LAPIS_OK;
    });
}

void lapis_session_destroy(lapis_session* session) {
    delete session;
}

lapis_result lapis_session_open(
    lapis_session* session,
    const lapis_media_locator* locator,
    lapis_playback_epoch epoch,
    lapis_open_result* out_result) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || locator == nullptr || out_result == nullptr ||
            locator->value_utf8 == nullptr || locator->value_utf8[0] == '\0') {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        const auto locator_kind = to_core_locator_kind(locator->kind);
        if (!locator_kind.has_value()) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);

        std::shared_ptr<RuntimePlaybackPipeline> retired_pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            session->epoch = epoch.value;
            session->position_ns = 0;
            session->transport = LAPIS_TRANSPORT_OPENING;
            retired_pipeline = std::move(session->pipeline);
            *out_result = {};
        }
        // Stop/join an old execution outside the state mutex. Snapshot readers
        // can still observe OPENING instead of being blocked behind teardown.
        retired_pipeline.reset();

        std::shared_ptr<lapis::media::PlaybackMediaSession> media;
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        lapis::media::MediaOpenInfo media_open_result{};
        try {
            media = std::make_shared<lapis::media::PlaybackMediaSession>();
            const lapis::media::MediaLocator core_locator{
                *locator_kind,
                std::string(locator->value_utf8)
            };
            media_open_result = media->open(core_locator);
            if (media_open_result.ok()) {
                pipeline = create_pipeline(media);
                // The desired host window belongs to the logical session,
                // not to a particular media-open or decode generation.
                const auto surface_status = pipeline->execution->set_video_output_surface(
                    session->desired_output_surface);
                if (surface_status != lapis::media::VideoOutputSurfaceControlStatus::Applied &&
                    surface_status != lapis::media::VideoOutputSurfaceControlStatus::Deferred) {
                    mark_faulted_if_current(session, epoch);
                    return map_surface_control_error(surface_status);
                }
            }
        } catch (...) {
            mark_faulted_if_current(session, epoch);
            throw;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }

        if (!media_open_result.ok()) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return map_open_error(media_open_result.error);
        }
        if (pipeline == nullptr || pipeline->execution == nullptr) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return LAPIS_E_INTERNAL;
        }

        out_result->capabilities = 0;
        if (media_open_result.duration.has_value()) {
            out_result->duration.nanoseconds = media_open_result.duration->nanoseconds;
            out_result->has_duration = 1;
        }

        session->pipeline = std::move(pipeline);
        session->transport = LAPIS_TRANSPORT_READY;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_play(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        refresh_terminal_transport(session);

        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        lapis_transport_state prior_transport{};
        int64_t position_ns{};
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PAUSED) {
                return LAPIS_E_INVALID_STATE;
            }
            if (session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return LAPIS_E_INTERNAL;
            }

            prior_transport = session->transport;
            position_ns = session->position_ns;
            pipeline = session->pipeline;
        }

        const auto result = prior_transport == LAPIS_TRANSPORT_READY
            ? pipeline->execution->start(
                lapis::media::PlaybackEpoch{epoch.value},
                lapis::media::MediaTime{position_ns})
            : pipeline->execution->resume();
        if (!result.ok()) {
            const auto mapped = map_execution_error(result.error);
            mark_faulted_if_current(session, epoch);
            return mapped;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }
        session->transport = LAPIS_TRANSPORT_PLAYING;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_pause(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        refresh_terminal_transport(session);

        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            if (session->transport != LAPIS_TRANSPORT_PLAYING) {
                return LAPIS_E_INVALID_STATE;
            }
            if (session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return LAPIS_E_INTERNAL;
            }
            pipeline = session->pipeline;
        }

        const auto result = pipeline->execution->pause();
        if (!result.ok()) {
            const auto mapped = map_execution_error(result.error);
            mark_faulted_if_current(session, epoch);
            return mapped;
        }
        const auto execution_snapshot = pipeline->execution->snapshot();

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }
        session->position_ns = execution_snapshot.clock.position.nanoseconds;
        session->transport = LAPIS_TRANSPORT_PAUSED;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_seek(
    lapis_session* session,
    lapis_media_time position,
    lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || position.nanoseconds < 0) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        refresh_terminal_transport(session);

        lapis_transport_state return_state{};
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED) {
                return LAPIS_E_INVALID_STATE;
            }
            if (session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return LAPIS_E_INTERNAL;
            }

            return_state = session->transport;
            session->epoch = epoch.value;
            session->transport = LAPIS_TRANSPORT_SEEKING;
            pipeline = session->pipeline;
        }

        lapis::media::PlaybackExecutionResult seek_result{};
        try {
            seek_result = pipeline->execution->seek(
                lapis::media::PlaybackEpoch{epoch.value},
                lapis::media::MediaTime{position.nanoseconds});
        } catch (...) {
            mark_faulted_if_current(session, epoch);
            throw;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }

        if (!seek_result.ok()) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return map_execution_error(seek_result.error);
        }

        session->position_ns = position.nanoseconds;
        session->transport = return_state;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_close(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);

        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }
            pipeline = session->pipeline;
        }

        if (pipeline != nullptr && pipeline->execution != nullptr) {
            const auto result = pipeline->execution->close(
                lapis::media::PlaybackEpoch{epoch.value});
            if (!result.ok()) {
                const auto mapped = map_execution_error(result.error);
                std::scoped_lock lock(session->mutex);
                session->epoch = epoch.value;
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return mapped;
            }
        }

        std::shared_ptr<RuntimePlaybackPipeline> retired_pipeline;
        {
            std::scoped_lock lock(session->mutex);
            session->epoch = epoch.value;
            session->position_ns = 0;
            session->transport = LAPIS_TRANSPORT_CLOSED;
            retired_pipeline = std::move(session->pipeline);
        }
        retired_pipeline.reset();
        return LAPIS_OK;
    });
}

lapis_result lapis_session_set_video_output_surface(
    lapis_session* session,
    lapis_video_output_surface surface,
    lapis_playback_epoch epoch,
    lapis_video_output_surface_binding* out_binding) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || out_binding == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }
        const auto descriptor = to_core_output_surface(surface);
        if (!descriptor.has_value()) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        lapis::media::VideoOutputSurfaceDescriptor previous{};
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }
            pipeline = session->pipeline;
            previous = session->desired_output_surface;
        }

        auto status = lapis::media::VideoOutputSurfaceControlStatus::Deferred;
        if (pipeline != nullptr && pipeline->execution != nullptr) {
            status = pipeline->execution->set_video_output_surface(*descriptor);
            if (status != lapis::media::VideoOutputSurfaceControlStatus::Applied &&
                status != lapis::media::VideoOutputSurfaceControlStatus::Deferred) {
                // A failed request is not committed as the next generation's
                // target. Restore the previous desired binding after the render
                // thread has acknowledged failure/teardown.
                (void)pipeline->execution->set_video_output_surface(previous);
                refresh_terminal_transport(session);
                return map_surface_control_error(status);
            }
        }

        {
            std::scoped_lock lock(session->mutex);
            session->desired_output_surface = *descriptor;
        }
        *out_binding = status == lapis::media::VideoOutputSurfaceControlStatus::Applied
            ? LAPIS_OUTPUT_SURFACE_APPLIED
            : LAPIS_OUTPUT_SURFACE_DEFERRED;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_get_snapshot(
    lapis_session* session,
    lapis_playback_snapshot* out_snapshot) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || out_snapshot == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        lapis_playback_session_id session_id{};
        uint64_t epoch{};
        int64_t position_ns{};
        lapis_transport_state transport{};
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            session_id = session->session_id;
            epoch = session->epoch;
            position_ns = session->position_ns;
            transport = session->transport;
            pipeline = session->pipeline;
        }

        if (pipeline != nullptr &&
            pipeline->execution != nullptr &&
            (active_transport(transport) || terminal_transport(transport))) {
            const auto execution_snapshot = pipeline->execution->snapshot();
            transport = transport_from_execution_state(
                execution_snapshot.state,
                transport);
            position_ns = execution_snapshot.clock.position.nanoseconds;
        }

        out_snapshot->session_id = session_id;
        out_snapshot->epoch.value = epoch;
        out_snapshot->position.nanoseconds = position_ns;
        out_snapshot->transport = transport;
        return LAPIS_OK;
    });
}
