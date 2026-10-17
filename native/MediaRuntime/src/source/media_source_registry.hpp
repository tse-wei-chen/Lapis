#pragma once

#include "source/media_source.hpp"

#include <memory>
#include <vector>

namespace lapis::media {

class MediaSourceRegistry final {
public:
    void add(std::unique_ptr<IMediaSourceProvider> provider);
    [[nodiscard]] SourceOpenResult open(const MediaLocator& locator) const;

private:
    std::vector<std::unique_ptr<IMediaSourceProvider>> providers_;
};

} // namespace lapis::media
