# Native bitmap subtitle composition contract (PGS prerequisite)

## Implemented scope

The native MediaRuntime Core now has an **owned bitmap cue** and a
CPU-testable, allocation-free RGBA compositor under `src/subtitle`.
This is the first compositing/geometry slice for future PGS, DVD bitmap
and other image subtitles. It **does not** yet decode PGS from a disc,
expose bitmap subtitle cues over the C ABI, or display them in Watch.

```
source / demux / codec adapter (future; FFmpeg/libbluray)
    -> owned BitmapSubtitleCue with canvas + timed RGBA rectangles
    -> BitmapSubtitleCompositor (pure CPU reference, RGBA)
    -> native presenter/overlay surface (future)
```

- Every cue specifies nanosecond start/end and an explicit display canvas.
- Each rectangle owns contiguous 8-bit **straight-alpha RGBA** bytes
  at integer canvas coordinates. All FFmpeg palette, pointer and stride
  details must be consumed inside a decoder adapter; none enter Core.
- `composite` validates the **whole** cue and destination *before* any
  writes, including frame stride, dimensions, exact byte sizes and
  overflow. Invalid input returns `false` without partially modifying
  the destination.
- Memory is bounded to **64 MiB of owned rectangle pixels per cue** and
  at most **64 rectangles**. Invalid/zero-sized rectangles are rejected;
  a zero-region cue is a legal timed clear event.
- Rectangle coordinates are clipped to the display canvas. Source-over
  blending works on transparent or opaque RGBA destinations; row padding
  remains untouched. The function never allocates or blocks on I/O.
- Cue and destination canvas dimensions must match. Deliberate scaling,
  color conversion and HDR/SDR blending are responsibilities of the
  native renderer, not this color-unmanaged reference compositor.

## Verification

`bitmap_subtitle_compositor_contract` is a portable native CTest target
with no FFmpeg, libbluray, D3D11, Avalonia or managed runtime dependency.
It covers alpha math on opaque/transparent surfaces, clipping of negative
coordinates, row padding, malformed buffers, integer overflow bounds,
off-canvas/transparent rectangles, and no partial writes.

The test has also been compiled locally with C++20 and
`-Wall -Wextra -Wpedantic -Werror`. Windows native CI must pass before
merging this PR.

## Next independently reviewable slices

1. FFmpeg PGS `AVSubtitleRect` bitmap/palette conversion into a bounded
   `BitmapSubtitleCue` without borrowing codec-owned buffers; real
   clear fixture and malformed-palette tests.
2. Epoch-scoped asynchronous bitmap cue timeline and optional versioned
   Native C ABI; cancellation/seek and CPU/GPU budget tests.
3. Explicit native presenter-side composition with deterministic
   video/audio/PGS timing and HDR color-space conversion; Windows
   hardware/BDMV acceptance before claiming playback support.

The existing text cue API and text subtitle cancellation extension are
unchanged. Text and bitmap paths must not be confused, and neither
blocks the live playback data plane.
