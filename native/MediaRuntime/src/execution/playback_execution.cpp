#include "execution/playback_execution.hpp"

#include "core/execution_graph_plan.hpp"
#include "core/monotonic_time.hpp"
#include "session/playback_media_session.hpp"

#include <stdexcept>
#include <utility>

namespace lapis::media {
namespace {

bool same_audio_format(const AudioFormat& left, const AudioFormat& right) {
    return left.sample_format == right.sample_format &&
        left.packing == right.packing &&
        left.sample_rate == right.sample_rate &&
        left.channels == right.channels;
}

} // namespace

PlaybackExecution::PlaybackExecution(
    PlaybackMediaSession& media,
    std::unique_ptr<IVideoFrameProcessor> processor,
    std::unique_ptr<IVideoPresenter> presenter,
    PlaybackExecutionConfig config,
    std::shared_ptr<DiagnosticEventJournal> journal)
    : PlaybackExecution(
          media,
          std::move(processor),
          std::move(presenter),
          nullptr,
          {},
          config,
          std::move(journal)) {}

PlaybackExecution::PlaybackExecution(
    PlaybackMediaSession& media,
    std::unique_ptr<IVideoFrameProcessor> processor,
    std::unique_ptr<IVideoPresenter> presenter,
    std::unique_ptr<IAudioOutputProvider> audio_output_provider,
    AudioFrameProcessorFactory audio_processor_factory,
    PlaybackExecutionConfig config,
    std::shared_ptr<DiagnosticEventJournal> journal)
    : media_(media),
      decoder_(media_, config.decode_queue_capacity),
      processor_(std::move(processor), config.process_queue_capacity),
      scheduler_(
          clock_,
          config.scheduler,
          config.presentation_queue_capacity),
      renderer_(clock_, std::move(presenter), std::move(journal)),
      audio_decoder_(media_, config.audio_decode_queue_capacity),
      audio_output_provider_(std::move(audio_output_provider)),
      audio_processor_factory_(std::move(audio_processor_factory)),
      audio_process_queue_capacity_(config.audio_process_queue_capacity),
      audio_clock_sync_config_(config.audio_clock_sync) {
    const bool has_provider = audio_output_provider_ != nullptr;
    const bool has_factory = static_cast<bool>(audio_processor_factory_);
    if (has_provider != has_factory) {
        throw std::invalid_argument(
            "PlaybackExecution audio output provider and processor factory must be configured together.");
    }
    if (audio_process_queue_capacity_ == 0) {
        throw std::invalid_argument(
            "PlaybackExecution audio process queue capacity must be greater than zero.");
    }
    if (audio_clock_sync_config_.sample_interval.nanoseconds <= 0) {
        throw std::invalid_argument(
            "PlaybackExecution audio clock sample interval must be greater than zero.");
    }
    // Validate discipline tolerances at composition time rather than failing
    // asynchronously when a playback generation is started.
    (void)AudioClockDiscipline(audio_clock_sync_config_.discipline);
}

PlaybackExecution::~PlaybackExecution() {
    try {
        std::scoped_lock lock(control_mutex_);
        const auto now = SteadyClockSource::now();
        const auto clock_snapshot = clock_.snapshot(now);
        if (clock_snapshot.state != PlaybackClockState::Stopped) {
            (void)clock_.stop(now);
        }
        stop_generation_unlocked();
    } catch (...) {
        // Public owners perform explicit close and can surface lifecycle errors.
    }
}

PlaybackExecutionResult PlaybackExecution::start(
    PlaybackEpoch epoch,
    MediaTime position) {
    if (position.nanoseconds < 0) {
        return {PlaybackExecutionError::InvalidArgument};
    }

    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Stopped) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (!start_epoch_valid_unlocked(epoch, position)) {
        return {PlaybackExecutionError::StaleEpoch};
    }

    const auto rebase_time = SteadyClockSource::now();
    if (!clock_.rebase(epoch, position, rebase_time)) {
        return {PlaybackExecutionError::ClockFailure};
    }

    if (!start_generation_unlocked(epoch)) {
        clock_.reset(epoch, SteadyClockSource::now());
        stopped_timeline_prepared_ = false;
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerStartFailed};
    }

    if (!clock_.start(position, SteadyClockSource::now())) {
        stop_generation_unlocked();
        clock_.reset(epoch, SteadyClockSource::now());
        stopped_timeline_prepared_ = false;
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    if (!resume_audio_unlocked()) {
        (void)clock_.stop(SteadyClockSource::now());
        stop_generation_unlocked();
        clock_.reset(epoch, SteadyClockSource::now());
        stopped_timeline_prepared_ = false;
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerStartFailed};
    }

    stopped_timeline_prepared_ = false;
    logical_state_ = PlaybackExecutionState::Running;
    return {};
}

PlaybackExecutionResult PlaybackExecution::pause() {
    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Running) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (workers_failed_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    // Freeze the device before freezing logical time so no queued hardware
    // presentation can advance after PlaybackClock enters Paused. The clock sync
    // worker can race with this transition, but its compare-and-apply rebase is
    // rejected if Pause changes the clock revision first.
    if (!pause_audio_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    if (!clock_.pause(SteadyClockSource::now())) {
        return fault_unlocked(PlaybackExecutionError::ClockFailure);
    }

    if (temporary_fast_forward_) {
        const auto clock_snapshot = clock_.snapshot(SteadyClockSource::now());
        const auto position = clock_snapshot.position;
        const auto epoch = clock_snapshot.epoch;
        stop_generation_unlocked();
        if (!media_.seek(position)) {
            temporary_fast_forward_ = false;
            (void)clock_.set_rate(1.0, SteadyClockSource::now());
            clock_.reset(epoch, SteadyClockSource::now());
            logical_state_ = PlaybackExecutionState::Faulted;
            return {PlaybackExecutionError::MediaSeekFailed};
        }
        temporary_fast_forward_ = false;
        if (!clock_.set_rate(1.0, SteadyClockSource::now()) ||
            !start_generation_unlocked(epoch)) {
            clock_.reset(epoch, SteadyClockSource::now());
            logical_state_ = PlaybackExecutionState::Faulted;
            return {PlaybackExecutionError::WorkerStartFailed};
        }
    }

    logical_state_ = PlaybackExecutionState::Paused;
    return {};
}

PlaybackExecutionResult PlaybackExecution::resume() {
    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Paused) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (workers_failed_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    // Resume logical time before allowing the output worker to start or resume
    // hardware presentation. While the renderer is still Paused, its position
    // reader reports Unavailable, so the sync worker cannot steer from a frozen
    // device sample in this short transition window.
    if (!clock_.resume(SteadyClockSource::now())) {
        return fault_unlocked(PlaybackExecutionError::ClockFailure);
    }

    if (!resume_audio_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    logical_state_ = PlaybackExecutionState::Running;
    return {};
}

PlaybackExecutionResult PlaybackExecution::seek(
    PlaybackEpoch epoch,
    MediaTime position) {
    if (position.nanoseconds < 0) {
        return {PlaybackExecutionError::InvalidArgument};
    }

    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Running &&
        logical_state_ != PlaybackExecutionState::Paused &&
        logical_state_ != PlaybackExecutionState::Stopped) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (!epoch_advances_unlocked(epoch)) {
        return {PlaybackExecutionError::StaleEpoch};
    }
    if (workers_failed_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    temporary_fast_forward_ = false;
    (void)clock_.set_rate(1.0, SteadyClockSource::now());
    const auto prior_state = logical_state_;
    if (prior_state == PlaybackExecutionState::Running) {
        if (!pause_audio_unlocked()) {
            return fault_unlocked(PlaybackExecutionError::WorkerFailed);
        }
        if (!clock_.pause(SteadyClockSource::now())) {
            return fault_unlocked(PlaybackExecutionError::ClockFailure);
        }
    }

    stop_generation_unlocked();

    if (!media_.seek(position)) {
        clock_.reset(epoch, SteadyClockSource::now());
        stopped_timeline_prepared_ = false;
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::MediaSeekFailed};
    }

    if (!clock_.rebase(epoch, position, SteadyClockSource::now())) {
        clock_.reset(epoch, SteadyClockSource::now());
        stopped_timeline_prepared_ = false;
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    // A stopped seek prepares the new timeline without starting workers. The
    // next Start is therefore allowed to use this exact epoch + position instead
    // of inventing a second discontinuity solely to begin presentation.
    stopped_timeline_prepared_ = prior_state == PlaybackExecutionState::Stopped;

    if (prior_state != PlaybackExecutionState::Stopped &&
        !start_generation_unlocked(epoch)) {
        clock_.reset(epoch, SteadyClockSource::now());
        stopped_timeline_prepared_ = false;
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerStartFailed};
    }

    if (prior_state == PlaybackExecutionState::Running) {
        if (!clock_.resume(SteadyClockSource::now())) {
            stop_generation_unlocked();
            clock_.reset(epoch, SteadyClockSource::now());
            stopped_timeline_prepared_ = false;
            logical_state_ = PlaybackExecutionState::Faulted;
            return {PlaybackExecutionError::ClockFailure};
        }
        if (!resume_audio_unlocked()) {
            return fault_unlocked(PlaybackExecutionError::WorkerFailed);
        }
    }

    logical_state_ = prior_state;
    return {};
}

PlaybackExecutionResult PlaybackExecution::set_fast_forward(
    PlaybackEpoch epoch,
    bool enabled) {
    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Running ||
        !media_.has_video() ||
        clock_.snapshot(SteadyClockSource::now()).epoch.value() != epoch.value()) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (temporary_fast_forward_ == enabled) {
        return {};
    }
    if (workers_failed_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    const auto position = clock_.snapshot(SteadyClockSource::now()).position;
    const auto now = SteadyClockSource::now();
    if (!clock_.pause(now)) {
        return fault_unlocked(PlaybackExecutionError::ClockFailure);
    }
    stop_generation_unlocked();
    if (!media_.seek(position)) {
        temporary_fast_forward_ = false;
        (void)clock_.set_rate(1.0, SteadyClockSource::now());
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::MediaSeekFailed};
    }

    temporary_fast_forward_ = enabled;
    if (!clock_.rebase(epoch, position, SteadyClockSource::now()) ||
        !clock_.set_rate(enabled ? 2.0 : 1.0, SteadyClockSource::now())) {
        temporary_fast_forward_ = false;
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }
    if (!start_generation_unlocked(epoch)) {
        temporary_fast_forward_ = false;
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerStartFailed};
    }
    if (!clock_.resume(SteadyClockSource::now())) {
        stop_generation_unlocked();
        temporary_fast_forward_ = false;
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }
    if (!resume_audio_unlocked()) {
        return fault_unlocked(PlaybackExecutionError::WorkerFailed);
    }

    logical_state_ = PlaybackExecutionState::Running;
    return {};
}

PlaybackExecutionResult PlaybackExecution::close(PlaybackEpoch epoch) {
    std::scoped_lock lock(control_mutex_);
    if (!epoch_advances_unlocked(epoch)) {
        return {PlaybackExecutionError::StaleEpoch};
    }

    const auto now = SteadyClockSource::now();
    const auto clock_snapshot = clock_.snapshot(now);
    if (clock_snapshot.state != PlaybackClockState::Stopped) {
        (void)clock_.stop(now);
    }

    stop_generation_unlocked();
    temporary_fast_forward_ = false;
    clock_.reset(epoch, SteadyClockSource::now());
    stopped_timeline_prepared_ = false;
    logical_state_ = PlaybackExecutionState::Stopped;
    return {};
}

PlaybackExecutionSnapshot PlaybackExecution::snapshot() const {
    std::scoped_lock lock(control_mutex_);

    auto state = logical_state_;
    if (workers_failed_unlocked()) {
        state = PlaybackExecutionState::Faulted;
    } else if ((state == PlaybackExecutionState::Running ||
                state == PlaybackExecutionState::Paused) &&
               workers_drained_unlocked()) {
        state = PlaybackExecutionState::Ended;
    }

    PlaybackExecutionSnapshot result;
    result.state = state;
    result.clock = clock_.snapshot(SteadyClockSource::now());
    result.video_active = video_generation_active_;
    result.audio_active = audio_generation_active_;
    result.decode_state = decoder_.state();
    result.process_state = processor_.state();
    result.process_status = processor_.last_status();
    result.scheduler_state = scheduler_.state();
    result.render_state = renderer_.state();
    result.scheduling = scheduler_.stats();
    result.rendering = renderer_.stats();
    result.presenter_diagnostics = renderer_.presenter_diagnostics();
    result.audio_decode_state = audio_decoder_.state();
    if (audio_processor_ != nullptr) {
        result.audio_process_state = audio_processor_->state();
        result.audio_process_status = audio_processor_->last_status();
    }
    if (audio_renderer_ != nullptr) {
        result.audio_render_state = audio_renderer_->state();
        result.audio_rendering = audio_renderer_->stats();
        result.audio_position = audio_renderer_->position();
    }
    if (audio_clock_sync_ != nullptr) {
        result.audio_clock_sync_state = audio_clock_sync_->state();
        result.audio_clock_sync = audio_clock_sync_->stats();
    }
    return result;
}

std::optional<GraphPlan> PlaybackExecution::active_graph_plan() const {
    std::scoped_lock lock(control_mutex_);
    return active_graph_plan_;
}

PlaybackExecutionResult PlaybackExecution::fault_unlocked(
    PlaybackExecutionError error) {
    const auto now = SteadyClockSource::now();
    if (clock_.snapshot(now).state != PlaybackClockState::Stopped) {
        (void)clock_.stop(now);
    }
    stop_generation_unlocked();
    stopped_timeline_prepared_ = false;
    logical_state_ = PlaybackExecutionState::Faulted;
    return {error};
}

bool PlaybackExecution::epoch_advances_unlocked(PlaybackEpoch epoch) const {
    return epoch.value() >
        clock_.snapshot(SteadyClockSource::now()).epoch.value();
}

bool PlaybackExecution::start_epoch_valid_unlocked(
    PlaybackEpoch epoch,
    MediaTime position) const {
    const auto snapshot = clock_.snapshot(SteadyClockSource::now());
    if (epoch.value() > snapshot.epoch.value()) {
        return true;
    }

    return stopped_timeline_prepared_ &&
        epoch.value() == snapshot.epoch.value() &&
        position == snapshot.position;
}

bool PlaybackExecution::workers_failed_unlocked() const noexcept {
    if (video_generation_active_) {
        const auto decode_state = decoder_.state();
        const auto process_state = processor_.state();
        const auto scheduler_state = scheduler_.state();
        const auto render_state = renderer_.state();

        if (decode_state == VideoDecodeWorkerState::Faulted ||
            process_state == VideoProcessWorkerState::UnsupportedInput ||
            process_state == VideoProcessWorkerState::Faulted ||
            scheduler_state == VideoSchedulerWorkerState::Faulted ||
            render_state == VideoRenderWorkerState::PresenterUnavailable ||
            render_state == VideoRenderWorkerState::Faulted) {
            return true;
        }
    }

    if (audio_generation_active_) {
        if (audio_processor_ == nullptr || audio_renderer_ == nullptr) {
            return true;
        }

        const auto decode_state = audio_decoder_.state();
        const auto process_state = audio_processor_->state();
        const auto render_state = audio_renderer_->state();
        if (decode_state == AudioDecodeWorkerState::Faulted ||
            process_state == AudioProcessWorkerState::UnsupportedInput ||
            process_state == AudioProcessWorkerState::Faulted ||
            render_state == AudioRenderWorkerState::UnsupportedFormat ||
            render_state == AudioRenderWorkerState::OutputUnavailable ||
            render_state == AudioRenderWorkerState::Faulted ||
            (audio_clock_sync_ != nullptr &&
             audio_clock_sync_->state() == AudioClockSyncWorkerState::Faulted)) {
            return true;
        }
    }

    return false;
}

bool PlaybackExecution::workers_drained_unlocked() const noexcept {
    if (!video_generation_active_ && !audio_generation_active_) {
        return false;
    }

    const bool video_drained = !video_generation_active_ ||
        (decoder_.state() == VideoDecodeWorkerState::Drained &&
         processor_.state() == VideoProcessWorkerState::Drained &&
         scheduler_.state() == VideoSchedulerWorkerState::Drained &&
         renderer_.state() == VideoRenderWorkerState::Drained);

    const bool audio_drained = !audio_generation_active_ ||
        (audio_processor_ != nullptr &&
         audio_renderer_ != nullptr &&
         audio_decoder_.state() == AudioDecodeWorkerState::Drained &&
         audio_processor_->state() == AudioProcessWorkerState::Drained &&
         audio_renderer_->state() == AudioRenderWorkerState::Drained);

    return video_drained && audio_drained;
}

bool PlaybackExecution::start_generation_unlocked(PlaybackEpoch epoch) {
    if (video_generation_active_ || audio_generation_active_ ||
        active_graph_plan_.has_value()) {
        return false;
    }

    const bool audio_configured = !temporary_fast_forward_ &&
        audio_output_provider_ != nullptr &&
        static_cast<bool>(audio_processor_factory_);

    // Source discovery is not the executable topology: audio decoding may be
    // available but the product has no configured output consumer. Compile the
    // selected branches before allocating output resources or starting workers.
    const auto* discovered = media_.graph_plan();
    if (discovered == nullptr) {
        return false;
    }
    auto execution_plan = compile_execution_graph(
        *discovered, {media_.has_video(), media_.has_audio() && audio_configured});
    if (!execution_plan.has_value()) {
        return false;
    }
    const bool use_video = execution_plan->contains(GraphNodeKind::VideoSink);
    const bool use_audio = execution_plan->contains(GraphNodeKind::AudioSink);

    try {
        if (use_audio) {
            auto plan = audio_output_provider_->prepare_default_output();
            if (!plan.ok()) {
                return false;
            }

            auto audio_processor = audio_processor_factory_(plan.target_format);
            if (audio_processor == nullptr ||
                !same_audio_format(
                    audio_processor->output_format(),
                    plan.target_format)) {
                return false;
            }

            audio_processor_ = std::make_unique<AudioProcessWorker>(
                std::move(audio_processor),
                audio_process_queue_capacity_);
            audio_renderer_ = std::make_unique<AudioRenderWorker>(
                std::move(plan.output));
        }

        if (!media_.start_decode_generation({use_video, use_audio})) {
            audio_renderer_.reset();
            audio_processor_.reset();
            return false;
        }

        video_generation_active_ = use_video;
        audio_generation_active_ = use_audio;

        std::shared_ptr<VideoFrameStream> video_decoded;
        std::shared_ptr<AudioFrameStream> audio_decoded;
        if (use_video) {
            video_decoded = decoder_.start(epoch);
        }
        if (use_audio) {
            audio_decoded = audio_decoder_.start(epoch);
        }

        std::shared_ptr<VideoFrameStream> video_processed;
        std::shared_ptr<AudioFrameStream> audio_processed;
        if (use_video) {
            video_processed = processor_.start(std::move(video_decoded));
        }
        if (use_audio) {
            audio_processed = audio_processor_->start(std::move(audio_decoded));
        }

        if (use_video) {
            auto presentation = scheduler_.start(std::move(video_processed));
            renderer_.start(std::move(presentation));
        }
        if (use_audio) {
            // Do not start the device-facing worker until the authoritative clock
            // is Running. While paused after a seek, bounded upstream queues may
            // fill, but no endpoint is opened and no sample can be presented.
            pending_audio_render_stream_ = std::move(audio_processed);
        }

        // Commit only after every selected worker generation was prepared.
        // Seek/Close will retire this plan after stopping all its workers.
        active_graph_plan_.emplace(std::move(*execution_plan));
        return true;
    } catch (...) {
        try {
            stop_generation_unlocked();
        } catch (...) {
        }
        return false;
    }
}

bool PlaybackExecution::pause_audio_unlocked() {
    if (!audio_generation_active_) {
        return true;
    }
    if (audio_renderer_ == nullptr) {
        return false;
    }
    if (pending_audio_render_stream_ != nullptr) {
        return true;
    }

    switch (audio_renderer_->state()) {
        case AudioRenderWorkerState::Paused:
        case AudioRenderWorkerState::Drained:
            return true;
        case AudioRenderWorkerState::Running:
            return audio_renderer_->pause();
        default:
            return false;
    }
}

bool PlaybackExecution::resume_audio_unlocked() {
    if (!audio_generation_active_) {
        return true;
    }
    if (audio_renderer_ == nullptr) {
        return false;
    }

    if (pending_audio_render_stream_ != nullptr) {
        try {
            audio_renderer_->start(pending_audio_render_stream_);
            pending_audio_render_stream_.reset();
            return start_audio_clock_sync_unlocked();
        } catch (...) {
            return false;
        }
    }

    switch (audio_renderer_->state()) {
        case AudioRenderWorkerState::Running:
            return start_audio_clock_sync_unlocked();
        case AudioRenderWorkerState::Drained:
            return true;
        case AudioRenderWorkerState::Paused:
            if (!audio_renderer_->resume()) {
                return false;
            }
            return start_audio_clock_sync_unlocked();
        default:
            return false;
    }
}

bool PlaybackExecution::start_audio_clock_sync_unlocked() {
    if (!audio_generation_active_ || audio_renderer_ == nullptr) {
        return false;
    }
    if (audio_clock_sync_ != nullptr) {
        return audio_clock_sync_->state() == AudioClockSyncWorkerState::Running;
    }

    try {
        auto* renderer = audio_renderer_.get();
        auto sync = std::make_unique<AudioClockSyncWorker>(
            clock_,
            [renderer]() -> AudioOutputPosition {
                // During Pause/Resume transitions the logical clock and device
                // deliberately change state in separate ordered operations. A
                // paused/drained renderer must never provide a steering sample.
                if (renderer->state() != AudioRenderWorkerState::Running) {
                    return {};
                }
                return renderer->position();
            },
            audio_clock_sync_config_);
        sync->start();
        audio_clock_sync_ = std::move(sync);
        return true;
    } catch (...) {
        return false;
    }
}

void PlaybackExecution::stop_generation_unlocked() {
    // Clock synchronization holds a non-owning reader for audio_renderer_. Stop
    // and join it before changing the renderer/output lifetime.
    if (audio_clock_sync_ != nullptr) {
        audio_clock_sync_->stop();
    }
    if (audio_renderer_ != nullptr) {
        audio_renderer_->stop();
    }
    renderer_.stop();
    scheduler_.stop();
    if (audio_processor_ != nullptr) {
        audio_processor_->stop();
    }
    processor_.stop();
    audio_decoder_.stop();
    decoder_.stop();
    media_.stop_decode_generation();

    pending_audio_render_stream_.reset();
    audio_clock_sync_.reset();
    audio_renderer_.reset();
    audio_processor_.reset();
    audio_generation_active_ = false;
    video_generation_active_ = false;
    active_graph_plan_.reset();
}

} // namespace lapis::media
