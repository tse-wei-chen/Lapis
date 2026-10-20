#!/usr/bin/env python3
"""Check output bytes created through MediaRuntime C ABI, not the FFmpeg CLI."""
import pathlib
import subprocess
import sys

exe, folder = sys.argv[1:]
prefix = pathlib.Path(folder) / "m1c-frame-dump-fixture"
result = subprocess.run([exe, "test://frames", str(prefix)], capture_output=True,
                        text=True, timeout=20, check=True)
assert "video_frames=8 audio_blocks=8 end_of_stream=yes" in result.stdout, result.stdout
assert prefix.with_suffix(".ppm").read_bytes().startswith(b"P6\n2 2\n255\n")
assert prefix.with_suffix(".s16le").stat().st_size == 8 * 480 * 4
prefix.with_suffix(".ppm").unlink()
prefix.with_suffix(".s16le").unlink()
print("Synthetic CPU frame/PCM dump passed")
