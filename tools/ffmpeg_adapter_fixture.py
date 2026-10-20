#!/usr/bin/env python3
"""Generate short real MP4 + MKV assets and exercise OPTIONAL in-process adapter.
Not a full playback or GUI test. Requires built bdmv_ffmpeg_decode executable.
"""
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*command):
    result = subprocess.run(command, text=True, capture_output=True, timeout=75)
    if result.returncode:
        raise RuntimeError(f"{command[:3]}: {result.returncode}\n{result.stderr[-1000:]}")
    return result.stdout


def main(exe, dumper=None):
    with tempfile.TemporaryDirectory(prefix="bdmv-adapter-") as tmp:
        mp4 = Path(tmp) / "input.mp4"
        mkv = Path(tmp) / "input.mkv"
        run("ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-t", "1", "-c:v", "mpeg4", "-c:a", "aac", "-shortest", str(mp4))
        run("ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
            "-i", str(mp4), "-map", "0", "-c", "copy", str(mkv))
        for path in (mp4, mkv):
            print(path.suffix, run(exe, str(path)).strip())
            if dumper:
                prefix = Path(tmp) / (path.stem + "-dump")
                output = run(dumper, str(path), str(prefix))
                assert "end_of_stream=yes" in output, output
                assert prefix.with_suffix(".ppm").read_bytes().startswith(b"P6\n160 90\n255\n")
                assert prefix.with_suffix(".s16le").stat().st_size > 0
                print("MediaRuntime C ABI:", output.strip())


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit("Usage: ffmpeg_adapter_fixture.py path/to/bdmv_ffmpeg_decode [path/to/bdmv_frame_dump]")
    main(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)
