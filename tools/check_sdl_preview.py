"""Optional headless SDL2 smoke; tests real SDL2 API calls with virtual sinks only."""
import os
import subprocess
import sys

if __name__ == '__main__':
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    try:
        proc = subprocess.run([sys.argv[1], 'test://frames', '--max-seconds', '6'],
                              env=env, text=True, capture_output=True, timeout=12)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise SystemExit(f'SDL dummy execution failed: {e}')
    if proc.returncode != 0:
        print(proc.stdout, proc.stderr, file=sys.stderr)
        raise SystemExit(proc.returncode)
    print(proc.stdout.strip())
    if 'video_presented=8 ' not in proc.stdout or 'queued_pcm_bytes=15360 ' not in proc.stdout or 'eof=1' not in proc.stdout:
        raise SystemExit('Preview did not present all synthetic video and PCM blocks')
