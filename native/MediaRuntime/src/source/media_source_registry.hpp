#pragma once

#include "source/media_source.hpp"

#include <memory>
#include <vector>

namespace lapis::media {

class MediaSourceRegistry final {
public:
    // Built-in providers keep declaration order. Application-installed
    // providers take precedence, newest first, so test doubles and future
    // configurable transports may override a built-in locator.
    void add(std::unique_ptr<IMediaSourceProvider> provider);
    void add_override(std::unique_ptr<IMediaSourceProvider> provider);
    [[nodiscard]] SourceOpenResult open(const MediaLocator& locator) const;

private:
    std::vector<std::unique_ptr<IMediaSourceProvider>> providers_;
};

} // namespace lapis::media
