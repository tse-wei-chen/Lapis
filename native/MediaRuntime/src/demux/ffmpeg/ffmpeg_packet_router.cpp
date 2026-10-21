#include "demux/ffmpeg/ffmpeg_packet_router.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace lapis::media::ffmpeg {

FfmpegPacket::FfmpegPacket()
    : packet_(av_packet_alloc()) {
    if (packet_ == nullptr) {
        throw std::bad_alloc();
    }
}

FfmpegPacket::~FfmpegPacket() {
    av_packet_free(&packet_);
}

FfmpegPacket::FfmpegPacket(FfmpegPacket&& other) noexcept
    : packet_(std::exchange(other.packet_, nullptr)) {}

FfmpegPacket& FfmpegPacket::operator=(FfmpegPacket&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    av_packet_free(&packet_);
    packet_ = std::exchange(other.packet_, nullptr);
    return *this;
}

int FfmpegPacket::stream_index() const noexcept {
    return packet_ != nullptr ? packet_->stream_index : -1;
}

struct FfmpegPacketRouter::Generation final {
    std::unordered_map<int, std::shared_ptr<BoundedQueue<FfmpegPacket>>> queues;
    std::atomic<FfmpegPacketRouterState> state{FfmpegPacketRouterState::Idle};
};

FfmpegPacketRouter::FfmpegPacketRouter(
    IFfmpegPacketSource& source,
    std::vector<int> stream_indexes,
    std::size_t queue_capacity)
    : source_(source),
      stream_indexes_(std::move(stream_indexes)),
      queue_capacity_(queue_capacity) {
    if (stream_indexes_.empty()) {
        throw std::invalid_argument(
            "FfmpegPacketRouter requires at least one selected stream.");
    }
    if (queue_capacity_ == 0) {
        throw std::invalid_argument(
            "FfmpegPacketRouter queue capacity must be greater than zero.");
    }

    std::vector<int> sorted = stream_indexes_;
    std::sort(sorted.begin(), sorted.end());
    if (sorted.front() < 0 ||
        std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw std::invalid_argument(
            "FfmpegPacketRouter stream indexes must be unique and non-negative.");
    }
}

FfmpegPacketRouter::~FfmpegPacketRouter() {
    stop();
}

void FfmpegPacketRouter::start() {
    std::scoped_lock lock(control_mutex_);

    if (worker_.joinable()) {
        if (generation_ != nullptr &&
            generation_->state.load(std::memory_order_acquire) ==
                FfmpegPacketRouterState::Running) {
            throw std::logic_error("FfmpegPacketRouter is already running.");
        }
        worker_.join();
    }

    auto generation = std::make_shared<Generation>();
    for (const auto stream_index : stream_indexes_) {
        generation->queues.emplace(
            stream_index,
            std::make_shared<BoundedQueue<FfmpegPacket>>(queue_capacity_));
    }
    generation->state.store(
        FfmpegPacketRouterState::Running,
        std::memory_order_release);
    generation_ = generation;
    source_.reset_interrupt();

    try {
        worker_ = std::jthread(
            [this, generation](std::stop_token stop_token) {
                run(stop_token, generation);
            });
    } catch (...) {
        generation->state.store(
            FfmpegPacketRouterState::Faulted,
            std::memory_order_release);
        for (const auto& entry : generation->queues) {
            entry.second->close();
        }
        throw;
    }
}

void FfmpegPacketRouter::stop() noexcept {
    std::shared_ptr<Generation> generation;
    try {
        std::scoped_lock lock(control_mutex_);
        generation = generation_;
        if (!worker_.joinable()) {
            return;
        }

        worker_.request_stop();
        source_.request_interrupt();
        worker_.join();
    } catch (...) {
        if (generation != nullptr) {
            generation->state.store(
                FfmpegPacketRouterState::Faulted,
                std::memory_order_release);
            for (const auto& entry : generation->queues) {
                try {
                    entry.second->close();
                } catch (...) {
                }
            }
        }
    }
}

FfmpegPacketReadResult FfmpegPacketRouter::read(
    int stream_index,
    std::stop_token stop_token) {
    std::shared_ptr<Generation> generation;
    std::shared_ptr<BoundedQueue<FfmpegPacket>> queue;
    {
        std::scoped_lock lock(control_mutex_);
        generation = generation_;
        if (generation == nullptr) {
            return {FfmpegPacketReadStatus::Stopped, std::nullopt};
        }

        const auto iterator = generation->queues.find(stream_index);
        if (iterator == generation->queues.end()) {
            return {FfmpegPacketReadStatus::UnknownStream, std::nullopt};
        }
        queue = iterator->second;
    }

    auto result = queue->pop(stop_token);
    if (result.has_item()) {
        return {
            FfmpegPacketReadStatus::Packet,
            std::move(result.item)
        };
    }
    if (result.status == QueueReadStatus::Stopped) {
        return {FfmpegPacketReadStatus::Stopped, std::nullopt};
    }

    switch (generation->state.load(std::memory_order_acquire)) {
        case FfmpegPacketRouterState::Drained:
            return {FfmpegPacketReadStatus::Drained, std::nullopt};
        case FfmpegPacketRouterState::Faulted:
            return {FfmpegPacketReadStatus::Faulted, std::nullopt};
        case FfmpegPacketRouterState::Idle:
        case FfmpegPacketRouterState::Running:
        case FfmpegPacketRouterState::Stopped:
            return {FfmpegPacketReadStatus::Stopped, std::nullopt};
    }

    return {FfmpegPacketReadStatus::Faulted, std::nullopt};
}

FfmpegPacketRouterState FfmpegPacketRouter::state() const {
    std::scoped_lock lock(control_mutex_);
    return generation_ != nullptr
        ? generation_->state.load(std::memory_order_acquire)
        : FfmpegPacketRouterState::Idle;
}

void FfmpegPacketRouter::run(
    std::stop_token stop_token,
    const std::shared_ptr<Generation>& generation) noexcept {
    auto final_state = FfmpegPacketRouterState::Stopped;

    try {
        while (!stop_token.stop_requested()) {
            FfmpegPacket packet;
            const auto read_result = source_.read_packet(packet.get());
            // An interrupted av_read_frame() is normal when retiring an Epoch.
            if (stop_token.stop_requested()) {
                break;
            }
            if (read_result == AVERROR_EOF) {
                final_state = FfmpegPacketRouterState::Drained;
                break;
            }
            if (read_result < 0) {
                final_state = FfmpegPacketRouterState::Faulted;
                break;
            }

            const auto iterator = generation->queues.find(packet.stream_index());
            if (iterator == generation->queues.end()) {
                continue;
            }

            const auto write_result = iterator->second->push(
                std::move(packet),
                stop_token);
            if (write_result != QueueWriteStatus::Pushed) {
                final_state = stop_token.stop_requested()
                    ? FfmpegPacketRouterState::Stopped
                    : FfmpegPacketRouterState::Faulted;
                break;
            }
        }
    } catch (...) {
        final_state = FfmpegPacketRouterState::Faulted;
    }

    generation->state.store(final_state, std::memory_order_release);
    for (const auto& entry : generation->queues) {
        try {
            entry.second->close();
        } catch (...) {
            generation->state.store(
                FfmpegPacketRouterState::Faulted,
                std::memory_order_release);
        }
    }
}

} // namespace lapis::media::ffmpeg
