#include "source/media_source_registry.hpp"

#include <stdexcept>

namespace lapis::media {

void MediaSourceRegistry::add(std::unique_ptr<IMediaSourceProvider> provider) {
    if (provider == nullptr) {
        throw std::invalid_argument("Media source provider cannot be null.");
    }

    providers_.push_back(std::move(provider));
}

SourceOpenResult MediaSourceRegistry::open(const MediaLocator& locator) const {
    for (const auto& provider : providers_) {
        if (provider->can_open(locator)) {
            return provider->open(locator);
        }
    }

    return {SourceError::UnsupportedLocator, nullptr};
}

} // namespace lapis::media
