#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"

#include "audio/audio_format_validation.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lapis::media::ffmpeg {
namespace {

enum class ConfigureStatus {
    Ready,
    Unsupported,
    Failed
};

bool same_audio_format(const AudioFormat& left, const AudioFormat& right) {
    return left.sample_format == right.sample_format &&
        left.packing == right.packing &&
        left.sample_rate == right.sample_rate &&
        left.channels == right.channels;
}

AVSampleFormat to_av_sample_format(const AudioFormat& format) noexcept {
    const bool planar = format.packing == AudioPacking::Planar;
    switch (format.sample_format) {
        case AudioSampleFormat::U8:
            return planar ? AV_SAMPLE_FMT_U8P : AV_SAMPLE_FMT_U8;
        case AudioSampleFormat::S16:
            return planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
        case AudioSampleFormat::S32:
            return planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
        case AudioSampleFormat::S64:
            return planar ? AV_SAMPLE_FMT_S64P : AV_SAMPLE_FMT_S64;
        case AudioSampleFormat::F32:
            return planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
        case AudioSampleFormat::F64:
            return planar ? AV_SAMPLE_FMT_DBLP : AV_SAMPLE_FMT_DBL;
        case AudioSampleFormat::Unknown:
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

AVChannel to_av_channel(AudioChannel channel) noexcept {
    switch (channel) {
        case AudioChannel::FrontLeft: return AV_CHAN_FRONT_LEFT;
        case AudioChannel::FrontRight: return AV_CHAN_FRONT_RIGHT;
        case AudioChannel::FrontCenter: return AV_CHAN_FRONT_CENTER;
        case AudioChannel::LowFrequency: return AV_CHAN_LOW_FREQUENCY;
        case AudioChannel::BackLeft: return AV_CHAN_BACK_LEFT;
        case AudioChannel::BackRight: return AV_CHAN_BACK_RIGHT;
        case AudioChannel::FrontLeftOfCenter: return AV_CHAN_FRONT_LEFT_OF_CENTER;
        case AudioChannel::FrontRightOfCenter: return AV_CHAN_FRONT_RIGHT_OF_CENTER;
        case AudioChannel::BackCenter: return AV_CHAN_BACK_CENTER;
        case AudioChannel::SideLeft: return AV_CHAN_SIDE_LEFT;
        case AudioChannel::SideRight: return AV_CHAN_SIDE_RIGHT;
        case AudioChannel::TopCenter: return AV_CHAN_TOP_CENTER;
        case AudioChannel::TopFrontLeft: return AV_CHAN_TOP_FRONT_LEFT;
        case AudioChannel::TopFrontCenter: return AV_CHAN_TOP_FRONT_CENTER;
        case AudioChannel::TopFrontRight: return AV_CHAN_TOP_FRONT_RIGHT;
        case AudioChannel::TopBackLeft: return AV_CHAN_TOP_BACK_LEFT;
        case AudioChannel::TopBackCenter: return AV_CHAN_TOP_BACK_CENTER;
        case AudioChannel::TopBackRight: return AV_CHAN_TOP_BACK_RIGHT;
        case AudioChannel::Unknown:
        default:
            return AV_CHAN_UNKNOWN;
    }
}

bool make_channel_layout(
    const AudioFormat& format,
    AVChannelLayout& layout) noexcept {
    if (format.channels.empty() ||
        format.channels.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    // Build the custom-order channel map through the public AVChannelLayout
    // struct and FFmpeg allocator. Unlike a channel-mask layout, this
    // preserves explicit surround-channel order on every supported FFmpeg
    // target (including the Ubuntu system package).
    layout.order = AV_CHANNEL_ORDER_CUSTOM;
    layout.nb_channels = static_cast<int>(format.channels.size());
    layout.u.map = static_cast<AVChannelCustom*>(
        av_calloc(format.channels.size(), sizeof(AVChannelCustom)));
    if (layout.u.map == nullptr) {
        av_channel_layout_uninit(&layout);
        return false;
    }

    for (std::size_t index = 0; index < format.channels.size(); ++index) {
        const auto channel = to_av_channel(format.channels[index]);
        if (channel == AV_CHAN_UNKNOWN) {
            av_channel_layout_uninit(&layout);
            return false;
        }
        layout.u.map[index].id = channel;
    }

    if (av_channel_layout_check(&layout) == 0) {
        av_channel_layout_uninit(&layout);
        return false;
    }
    return true;
}

bool valid_processor_format(const AudioFormat& format) noexcept {
    if (format.sample_rate == 0 || format.channels.empty() ||
        to_av_sample_format(format) == AV_SAMPLE_FMT_NONE) {
        return false;
    }
    AVChannelLayout layout{};
    const bool valid = make_channel_layout(format, layout);
    av_channel_layout_uninit(&layout);
    return valid;
}

std::uint32_t sample_bytes(const AudioFormat& format) noexcept {
    const int bytes = av_get_bytes_per_sample(to_av_sample_format(format));
    return bytes > 0 ? static_cast<std::uint32_t>(bytes) : 0U;
}

class OwnedAudioBuffer final : public IAudioBuffer {
public:
    OwnedAudioBuffer(
        AudioFormat format,
        std::uint32_t sample_frame_count,
        std::uint32_t bytes_per_sample,
        std::vector<std::vector<std::byte>> planes) noexcept
        : format_(std::move(format)),
          sample_frame_count_(sample_frame_count),
          bytes_per_sample_(bytes_per_sample),
          planes_(std::move(planes)) {}

    [[nodiscard]] const AudioFormat& format() const noexcept override {
        return format_;
    }

    [[nodiscard]] std::uint32_t sample_frame_count() const noexcept override {
        return sample_frame_count_;
    }

    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept override {
        return bytes_per_sample_;
    }

    [[nodiscard]] std::uint32_t plane_count() const noexcept override {
        return static_cast<std::uint32_t>(planes_.size());
    }

    [[nodiscard]] AudioPlaneView plane(std::uint32_t index) const noexcept override {
        if (index >= planes_.size() || planes_[index].empty()) {
            return {};
        }
        return {planes_[index].data(), planes_[index].size()};
    }

private:
    AudioFormat format_;
    std::uint32_t sample_frame_count_{};
    std::uint32_t bytes_per_sample_{};
    std::vector<std::vector<std::byte>> planes_;
};

struct OutputStorage final {
    std::vector<std::vector<std::byte>> planes;
    std::vector<std::uint8_t*> pointers;
};

std::optional<OutputStorage> allocate_output_storage(
    const AudioFormat& format,
    int sample_capacity) {
    if (sample_capacity <= 0) {
        return std::nullopt;
    }

    const auto bytes = static_cast<std::size_t>(sample_bytes(format));
    const auto channels = format.channels.size();
    if (bytes == 0 || channels == 0) {
        return std::nullopt;
    }

    const auto samples = static_cast<std::size_t>(sample_capacity);
    if (samples > std::numeric_limits<std::size_t>::max() / bytes) {
        return std::nullopt;
    }
    const auto bytes_per_channel = samples * bytes;

    const bool planar = format.packing == AudioPacking::Planar;
    if (!planar &&
        channels > std::numeric_limits<std::size_t>::max() / bytes_per_channel) {
        return std::nullopt;
    }

    const auto plane_count = planar ? channels : std::size_t{1};
    const auto plane_bytes = planar
        ? bytes_per_channel
        : bytes_per_channel * channels;

    OutputStorage storage;
    storage.planes.resize(plane_count);
    storage.pointers.reserve(plane_count);
    for (auto& plane : storage.planes) {
        plane.resize(plane_bytes);
        storage.pointers.push_back(
            reinterpret_cast<std::uint8_t*>(plane.data()));
    }
    return storage;
}

MediaTime samples_to_time(std::int64_t samples, std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0) {
        return {};
    }
    return MediaTime{
        av_rescale_q(
            samples,
            AVRational{1, static_cast<int>(sample_rate)},
            AVRational{1, 1'000'000'000})
    };
}

} // namespace

FfmpegAudioFrameConverter::FfmpegAudioFrameConverter(AudioFormat output_format)
    : output_format_(std::move(output_format)) {
    if (!valid_processor_format(output_format_)) {
        throw std::invalid_argument(
            "FfmpegAudioFrameConverter requires a supported output format.");
    }
}

FfmpegAudioFrameConverter::~FfmpegAudioFrameConverter() {
    swr_free(&context_);
}

AudioProcessResult FfmpegAudioFrameConverter::process(const AudioFrame& frame) {
    if (frame.buffer == nullptr || !valid_audio_buffer(*frame.buffer)) {
        return {AudioProcessStatus::UnsupportedInput, std::nullopt};
    }

    const auto& input_format = frame.buffer->format();
    if (!valid_processor_format(input_format)) {
        return {AudioProcessStatus::UnsupportedInput, std::nullopt};
    }

    ConfigureStatus configure_status = ConfigureStatus::Ready;
    if (context_ == nullptr) {
        AVChannelLayout input_layout{};
        AVChannelLayout output_layout{};
        if (!make_channel_layout(input_format, input_layout) ||
            !make_channel_layout(output_format_, output_layout)) {
            av_channel_layout_uninit(&input_layout);
            av_channel_layout_uninit(&output_layout);
            return {AudioProcessStatus::UnsupportedInput, std::nullopt};
        }

        SwrContext* context = nullptr;
        const int allocate_result = swr_alloc_set_opts2(
            &context,
            &output_layout,
            to_av_sample_format(output_format_),
            static_cast<int>(output_format_.sample_rate),
            &input_layout,
            to_av_sample_format(input_format),
            static_cast<int>(input_format.sample_rate),
            0,
            nullptr);
        av_channel_layout_uninit(&input_layout);
        av_channel_layout_uninit(&output_layout);

        if (allocate_result < 0 || context == nullptr) {
            swr_free(&context);
            configure_status = ConfigureStatus::Failed;
        } else if (swr_init(context) < 0) {
            swr_free(&context);
            configure_status = ConfigureStatus::Failed;
        } else {
            context_ = context;
            input_format_ = input_format;
            input_sample_rate_ = input_format.sample_rate;
        }
    } else if (!input_format_.has_value() ||
               !same_audio_format(*input_format_, input_format)) {
        configure_status = ConfigureStatus::Unsupported;
    }

    if (configure_status == ConfigureStatus::Unsupported) {
        return {AudioProcessStatus::UnsupportedInput, std::nullopt};
    }
    if (configure_status == ConfigureStatus::Failed || context_ == nullptr) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }

    const auto input_samples = frame.buffer->sample_frame_count();
    if (input_samples > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return {AudioProcessStatus::UnsupportedInput, std::nullopt};
    }

    const auto delay_before = swr_get_delay(
        context_,
        static_cast<std::int64_t>(input_sample_rate_));
    if (delay_before < 0 ||
        delay_before > std::numeric_limits<std::int64_t>::max() - input_samples) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }

    const auto capacity64 = av_rescale_rnd(
        delay_before + static_cast<std::int64_t>(input_samples),
        output_format_.sample_rate,
        input_sample_rate_,
        AV_ROUND_UP);
    if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }

    auto storage = allocate_output_storage(
        output_format_,
        static_cast<int>(capacity64));
    if (!storage.has_value()) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }

    std::vector<const std::uint8_t*> input_pointers;
    input_pointers.reserve(frame.buffer->plane_count());
    for (std::uint32_t index = 0; index < frame.buffer->plane_count(); ++index) {
        const auto plane = frame.buffer->plane(index);
        input_pointers.push_back(
            reinterpret_cast<const std::uint8_t*>(plane.data));
    }

    const int converted = swr_convert(
        context_,
        storage->pointers.data(),
        static_cast<int>(capacity64),
        input_pointers.data(),
        static_cast<int>(input_samples));
    if (converted < 0) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }
    if (converted == 0) {
        return {AudioProcessStatus::NeedMoreInput, std::nullopt};
    }

    const auto bytes = sample_bytes(output_format_);
    auto buffer = std::make_shared<OwnedAudioBuffer>(
        output_format_,
        static_cast<std::uint32_t>(converted),
        bytes,
        std::move(storage->planes));

    const auto delay_time = samples_to_time(delay_before, input_sample_rate_);
    AudioFrame output{};
    output.buffer = std::move(buffer);
    output.timestamp = MediaTime{
        frame.timestamp.nanoseconds - delay_time.nanoseconds
    };
    output.duration = samples_to_time(converted, output_format_.sample_rate);
    output.source_timestamp = frame.source_timestamp;
    next_output_timestamp_ = MediaTime{
        output.timestamp.nanoseconds + output.duration.nanoseconds
    };
    return {AudioProcessStatus::Processed, std::move(output)};
}

AudioProcessResult FfmpegAudioFrameConverter::drain() {
    if (context_ == nullptr || input_sample_rate_ == 0) {
        return {AudioProcessStatus::Drained, std::nullopt};
    }

    const auto delay = swr_get_delay(
        context_,
        static_cast<std::int64_t>(input_sample_rate_));
    if (delay <= 0) {
        return {AudioProcessStatus::Drained, std::nullopt};
    }

    const auto capacity64 = av_rescale_rnd(
        delay,
        output_format_.sample_rate,
        input_sample_rate_,
        AV_ROUND_UP);
    if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }

    auto storage = allocate_output_storage(
        output_format_,
        static_cast<int>(capacity64));
    if (!storage.has_value()) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }

    const int converted = swr_convert(
        context_,
        storage->pointers.data(),
        static_cast<int>(capacity64),
        nullptr,
        0);
    if (converted < 0) {
        return {AudioProcessStatus::Failed, std::nullopt};
    }
    if (converted == 0) {
        return {AudioProcessStatus::Drained, std::nullopt};
    }

    AudioFrame output{};
    output.buffer = std::make_shared<OwnedAudioBuffer>(
        output_format_,
        static_cast<std::uint32_t>(converted),
        sample_bytes(output_format_),
        std::move(storage->planes));
    output.timestamp = next_output_timestamp_.value_or(MediaTime{});
    output.duration = samples_to_time(converted, output_format_.sample_rate);
    next_output_timestamp_ = MediaTime{
        output.timestamp.nanoseconds + output.duration.nanoseconds
    };
    return {AudioProcessStatus::Processed, std::move(output)};
}

void FfmpegAudioFrameConverter::reset() noexcept {
    swr_free(&context_);
    input_format_.reset();
    next_output_timestamp_.reset();
    input_sample_rate_ = 0;
}

} // namespace lapis::media::ffmpeg
