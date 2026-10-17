#pragma once

#include "core/bounded_queue.hpp"

struct AVPacket;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lapis::media::ffmpeg {

class IFfmpegPacketSource {
public:
    virtual ~IFfmpegPacketSource() = default;
    virtual int read_packet(AVPacket& packet) = 0;
};

class FfmpegPacket final {
public:
    FfmpegPacket();
    ~FfmpegPacket();

    FfmpegPacket(const FfmpegPacket&) = delete;
    FfmpegPacket& operator=(const FfmpegPacket&) = delete;
    FfmpegPacket(FfmpegPacket&& other) noexcept;
    FfmpegPacket& operator=(FfmpegPacket&& other) noexcept;

    [[nodiscard]] AVPacket& get() noexcept { return *packet_; }
    [[nodiscard]] const AVPacket& get() const noexcept { return *packet_; }
    [[nodiscard]] int stream_index() const noexcept;

private:
    AVPacket* packet_{};
};

enum class FfmpegPacketRouterState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    Faulted
};

enum class FfmpegPacketReadStatus : std::uint8_t {
    Packet,
    Drained,
    Stopped,
    Faulted,
    UnknownStream
};

struct FfmpegPacketReadResult final {
    FfmpegPacketReadStatus status{FfmpegPacketReadStatus::Stopped};
    std::optional<FfmpegPacket> packet;

    [[nodiscard]] bool has_packet() const noexcept {
        return status == FfmpegPacketReadStatus::Packet && packet.has_value();
    }
};

class FfmpegPacketRouter final {
public:
    FfmpegPacketRouter(
        IFfmpegPacketSource& source,
        std::vector<int> stream_indexes,
        std::size_t queue_capacity = 32);
    ~FfmpegPacketRouter();

    FfmpegPacketRouter(const FfmpegPacketRouter&) = delete;
    FfmpegPacketRouter& operator=(const FfmpegPacketRouter&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] FfmpegPacketReadResult read(
        int stream_index,
        std::stop_token stop_token = {});

    [[nodiscard]] FfmpegPacketRouterState state() const;

private:
    struct Generation;

    void run(
        std::stop_token stop_token,
        const std::shared_ptr<Generation>& generation) noexcept;

    IFfmpegPacketSource& source_;
    const std::vector<int> stream_indexes_;
    const std::size_t queue_capacity_;
    mutable std::mutex control_mutex_;
    std::shared_ptr<Generation> generation_;
    std::jthread worker_;
};

} // namespace lapis::media::ffmpeg
