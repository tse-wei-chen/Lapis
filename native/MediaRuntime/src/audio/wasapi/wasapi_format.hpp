#pragma once

#if defined(_WIN32)

#include "audio/audio_frame.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <optional>

namespace lapis::media::wasapi {

// Converts a Lapis-owned interleaved PCM format into an exact WASAPI format.
// Returning nullopt means the shape cannot be represented without remapping,
// repacking, or changing sample representation.
[[nodiscard]] std::optional<WAVEFORMATEXTENSIBLE> build_wave_format(
    const AudioFormat& format) noexcept;

} // namespace lapis::media::wasapi

#endif
