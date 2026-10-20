#!/usr/bin/env python3
"""Optional M1 media-fixture diagnostic: exercises installed FFmpeg CLI only.

This is NOT the BDMV player decoder, not a runtime backend and not UI playback.
It proves real MP4/MKV files can be demuxed/decoded into RGB frames + PCM on
this machine, while the in-process libav* adapter is still outstanding.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def execute(args, *, timeout=60):
    result = subprocess.run(args, capture_output=True, timeout=timeout, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, args[:5]))} failed ({result.returncode}): "
                           + result.stderr.decode(errors="replace")[-2000:])
    return result.stdout


def check_input(path, native_probe=None):
    if native_probe is not None:
        result = json.loads(execute([str(native_probe), str(path)]))
        if result.get("file_size", 0) < 32 or result.get("tail_bytes", 0) == 0:
            raise RuntimeError("Native FileSource did not read the file")
        print(json.dumps({"native_file_reader": result}, ensure_ascii=False))
    payload = execute(["ffprobe", "-v", "error", "-show_entries",
                       "stream=codec_type,codec_name,width,height", "-of", "json", str(path)])
    streams = json.loads(payload).get("streams", [])
    if not any(s.get("codec_type") == "video" for s in streams):
        raise RuntimeError(f"No video stream in {path}")
    video = execute(["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error",
                     "-i", str(path), "-map", "0:v:0", "-frames:v", "3",
                     "-vf", "scale=160:90", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"])
    frame_bytes = 160 * 90 * 3
    if len(video) < frame_bytes or len(video) % frame_bytes:
        raise RuntimeError("Invalid RGB24 frame count or byte boundaries")
    audio_bytes = 0
    if any(s.get("codec_type") == "audio" for s in streams):
        audio = execute(["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error",
                         "-i", str(path), "-map", "0:a:0", "-t", "0.25",
                         "-ac", "2", "-ar", "48000", "-f", "s16le", "-"])
        audio_bytes = len(audio)
        if not audio_bytes or audio_bytes % 4:
            raise RuntimeError("Invalid stereo PCM byte boundaries")
    summary = {"container": path.suffix.lower(), "video_frames_decoded": len(video)//frame_bytes,
               "rgb_bytes": len(video), "pcm_s16le_bytes": audio_bytes,
               "streams": [{"type": s.get("codec_type"), "codec": s.get("codec_name")}
                           for s in streams if s.get("codec_type") in ("video", "audio")]}
    print(json.dumps(summary, ensure_ascii=False))


def fixture_test(directory, native_probe=None):
    mp4 = directory / "fixture.mp4"
    mkv = directory / "fixture.mkv"
    execute(["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
             "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25",
             "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
             "-t", "1", "-c:v", "mpeg4", "-q:v", "4", "-c:a", "aac", "-shortest", str(mp4)])
    execute(["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
             "-i", str(mp4), "-map", "0", "-c", "copy", str(mkv)])
    check_input(mp4, native_probe)
    check_input(mkv, native_probe)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-test", action="store_true")
    group.add_argument("--input", type=Path)
    parser.add_argument("--native-probe", type=Path, help="Optional built native bdmv_file_probe executable")
    args = parser.parse_args()
    if args.native_probe is not None and not args.native_probe.is_file():
        parser.error("--native-probe does not exist")
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        parser.error("FFmpeg/ffprobe executables are required in PATH")
    if args.self_test:
        with tempfile.TemporaryDirectory(prefix="bdmv-ffmpeg-") as directory:
            fixture_test(Path(directory), args.native_probe)
    else:
        if not args.input.is_file():
            parser.error("--input must point to a local media file")
        check_input(args.input, args.native_probe)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"diagnostic failed: {exc}", file=sys.stderr)
        sys.exit(1)
