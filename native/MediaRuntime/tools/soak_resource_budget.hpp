#pragma once

#include <algorithm>
#include <cstdint>

namespace lapis::soak {

struct ProcessMetrics final {
    std::uint64_t handles{};
    std::uint64_t gdi_objects{};
    std::uint64_t user_objects{};
    std::uint64_t private_bytes{};
    std::uint64_t working_set_bytes{};
};

// Zero disables a particular metric; a CLI user explicitly provides one or
// more nonzero limits to opt into this acceptance gate.
struct GrowthLimits final {
    std::uint64_t handles{};
    std::uint64_t gdi_objects{};
    std::uint64_t user_objects{};
    std::uint64_t private_bytes{};
};

[[nodiscard]] constexpr std::uint64_t positive_growth(
    std::uint64_t later, std::uint64_t earlier) noexcept {
    return later > earlier ? later - earlier : 0;
}

// Records EVERY post-Close cycle, even if the JSON trace retains only 128
// checkpoints. Using the final sample alone would miss intermediate growth
// that returned below the budget just before the last cycle.
class ResourceGrowthEnvelope final {
public:
    void observe(
        std::uint32_t completed_cycle,
        std::uint32_t warmup_cycles,
        ProcessMetrics sample) noexcept {
        if (completed_cycle == warmup_cycles) {
            warmup_ = sample;
            post_warmup_peak_ = sample;
            captured_ = true;
            post_warmup_samples_ = 0;
        } else if (captured_ && completed_cycle > warmup_cycles) {
            post_warmup_peak_.handles =
                std::max(post_warmup_peak_.handles, sample.handles);
            post_warmup_peak_.gdi_objects =
                std::max(post_warmup_peak_.gdi_objects, sample.gdi_objects);
            post_warmup_peak_.user_objects =
                std::max(post_warmup_peak_.user_objects, sample.user_objects);
            post_warmup_peak_.private_bytes =
                std::max(post_warmup_peak_.private_bytes, sample.private_bytes);
            post_warmup_peak_.working_set_bytes =
                std::max(post_warmup_peak_.working_set_bytes, sample.working_set_bytes);
            ++post_warmup_samples_;
        }
    }

    [[nodiscard]] bool has_warmup() const noexcept { return captured_; }
    [[nodiscard]] std::uint32_t sample_count() const noexcept {
        return post_warmup_samples_;
    }
    [[nodiscard]] ProcessMetrics warmup() const noexcept { return warmup_; }
    [[nodiscard]] ProcessMetrics peak() const noexcept { return post_warmup_peak_; }

    // Stable, user-media-path-free failure identifiers for the JSON reporter.
    [[nodiscard]] const char* evaluate(GrowthLimits limits) const noexcept {
        if (!captured_) {
            return "resource_warmup_not_reached";
        }
        if (post_warmup_samples_ == 0) {
            return "resource_post_warmup_sample_missing";
        }
        if (limits.handles != 0 &&
            positive_growth(post_warmup_peak_.handles, warmup_.handles) > limits.handles) {
            return "handle_growth_limit_exceeded";
        }
        if (limits.gdi_objects != 0 &&
            positive_growth(post_warmup_peak_.gdi_objects, warmup_.gdi_objects) >
                limits.gdi_objects) {
            return "gdi_growth_limit_exceeded";
        }
        if (limits.user_objects != 0 &&
            positive_growth(post_warmup_peak_.user_objects, warmup_.user_objects) >
                limits.user_objects) {
            return "user_growth_limit_exceeded";
        }
        if (limits.private_bytes != 0 &&
            positive_growth(post_warmup_peak_.private_bytes, warmup_.private_bytes) >
                limits.private_bytes) {
            return "private_bytes_growth_limit_exceeded";
        }
        return "";
    }

private:
    ProcessMetrics warmup_{};
    ProcessMetrics post_warmup_peak_{};
    bool captured_{};
    std::uint32_t post_warmup_samples_{};
};

} // namespace lapis::soak
