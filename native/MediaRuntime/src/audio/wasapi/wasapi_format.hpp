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

// Converts a WASAPI PCM/float descriptor into the portable Lapis format model.
// Reduced-valid-bit PCM containers and channel layouts that Lapis cannot name
// exactly are rejected instead of being silently reinterpreted. This allocates
// the portable channel vector, so allocation failures propagate to the caller.
[[nodiscard]] std::optional<AudioFormat> parse_wave_format(
    const WAVEFORMATEX& format);

} // namespace lapis::media::wasapi

#endif
