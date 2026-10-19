#include "source/media_source_registry.hpp"

#include <stdexcept>

namespace lapis::media {

void MediaSourceRegistry::add(std::unique_ptr<IMediaSourceProvider> provider) {
    if (provider == nullptr) {
        throw std::invalid_argument("Media source provider cannot be null.");
    }

    providers_.push_back(std::move(provider));
}

void MediaSourceRegistry::add_override(
    std::unique_ptr<IMediaSourceProvider> provider) {
    if (provider == nullptr) {
        throw std::invalid_argument("Media source provider cannot be null.");
    }

    providers_.insert(providers_.begin(), std::move(provider));
}

SourceOpenResult MediaSourceRegistry::open(const MediaLocator& locator) const {
    for (const auto& provider : providers_) {
        if (provider->can_open(locator)) {
            // First claiming provider owns the result. A failed open is not
            // silently retried through another provider: that could cross
            // a trust/protection boundary or bypass an explicit override.
            return provider->open(locator);
        }
    }

    return {SourceError::UnsupportedLocator, nullptr};
}

} // namespace lapis::media
