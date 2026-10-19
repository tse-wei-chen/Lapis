#include "soak_resource_budget.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main() {
    using lapis::soak::GrowthLimits;
    using lapis::soak::ProcessMetrics;
    using lapis::soak::ResourceGrowthEnvelope;
    using lapis::soak::positive_growth;

    CHECK(positive_growth(100, 110) == 0);
    CHECK(positive_growth(110, 100) == 10);

    ResourceGrowthEnvelope early{};
    CHECK(std::string_view(early.evaluate({64, 32, 32, 128})) ==
          "resource_warmup_not_reached");
    early.observe(4, 4, ProcessMetrics{100, 10, 20, 200, 250});
    CHECK(early.has_warmup());
    CHECK(early.sample_count() == 0);
    CHECK(std::string_view(early.evaluate({64, 32, 32, 128})) ==
          "resource_post_warmup_sample_missing");

    // A mid-run spike beyond the budget must fail even if the final
    // measurement has dropped below the warmup baseline.
    early.observe(5, 4, ProcessMetrics{180, 50, 60, 500, 550});
    early.observe(6, 4, ProcessMetrics{90, 8, 19, 190, 240});
    CHECK(early.sample_count() == 2);
    CHECK(early.peak().handles == 180);
    CHECK(early.peak().gdi_objects == 50);
    CHECK(early.peak().user_objects == 60);
    CHECK(early.peak().private_bytes == 500);
    CHECK(std::string_view(early.evaluate({64, 32, 32, 128})) ==
          "handle_growth_limit_exceeded");
    CHECK(std::string_view(early.evaluate({0, 32, 32, 128})) ==
          "gdi_growth_limit_exceeded");
    CHECK(std::string_view(early.evaluate({0, 0, 32, 128})) ==
          "user_growth_limit_exceeded");
    CHECK(std::string_view(early.evaluate({0, 0, 0, 128})) ==
          "private_bytes_growth_limit_exceeded");
    CHECK(std::string_view(early.evaluate({80, 40, 40, 300})).empty());
    CHECK(std::string_view(early.evaluate({})).empty());

    // Warmup data is excluded: giant initialization allocations cannot
    // accidentally become the baseline or post-warmup peak.
    ResourceGrowthEnvelope stable{};
    stable.observe(1, 3, ProcessMetrics{400, 300, 200, 2000, 2500});
    stable.observe(2, 3, ProcessMetrics{300, 300, 300, 2000, 2500});
    stable.observe(3, 3, ProcessMetrics{100, 20, 15, 1000, 1100});
    stable.observe(4, 3, ProcessMetrics{103, 20, 16, 1010, 1200});
    stable.observe(5, 3, ProcessMetrics{99, 19, 15, 900, 950});
    CHECK(stable.sample_count() == 2);
    CHECK(stable.peak().handles == 103);
    CHECK(stable.peak().private_bytes == 1010);
    CHECK(stable.warmup().handles == 100);
    CHECK(std::string_view(stable.evaluate({3, 1, 1, 10})).empty());
    CHECK(std::string_view(stable.evaluate({2, 1, 1, 10})) ==
          "handle_growth_limit_exceeded");

    // The 128-entry report cap must not limit the monitored cycle count.
    ResourceGrowthEnvelope many{};
    many.observe(4, 4, ProcessMetrics{10, 1, 1, 100, 100});
    for (std::uint32_t cycle = 5; cycle <= 5000; ++cycle) {
        many.observe(cycle, 4, ProcessMetrics{
            static_cast<std::uint64_t>(10 + (cycle == 3000 ? 99 : 1)),
            1, 1, 100, 100
        });
    }
    CHECK(many.sample_count() == 4996);
    CHECK(many.peak().handles == 109);
    CHECK(std::string_view(many.evaluate({64, 0, 0, 0})) ==
          "handle_growth_limit_exceeded");

    return 0;
}
