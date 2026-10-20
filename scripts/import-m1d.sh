#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: bash scripts/import-m1d.sh /path/to/bdmv-player-v0.2-m1d.zip" >&2
  exit 2
fi
repo_root="$(git rev-parse --show-toplevel)"
branch="$(git branch --show-current)"
if [[ "$branch" != "chore/bootstrap-m1d-baseline" ]]; then
  echo "Expected chore/bootstrap-m1d-baseline; got: $branch" >&2
  exit 2
fi
if [[ -n "$(git status --porcelain)" ]]; then
  echo 'Working tree is not clean. Commit/stash changes first.' >&2
  exit 2
fi

python3 - "$1" "$repo_root" <<'PY'
import hashlib
from pathlib import Path, PurePosixPath
import stat
import sys
import zipfile

archive = Path(sys.argv[1]).expanduser().resolve()
root = Path(sys.argv[2]).resolve()
prefix = 'bdmv-player-v0.2/'
expected_sha256 = '06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932'
expected_file_count = 67

if not archive.is_file():
    raise SystemExit(f'Archive not found: {archive}')
with archive.open('rb') as stream:
    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
if digest != expected_sha256:
    raise SystemExit('M1d archive SHA-256 mismatch. Refusing to import.')

entries = []
seen = set()
with zipfile.ZipFile(archive) as z:
    for info in z.infolist():
        if info.is_dir():
            continue
        name = info.filename
        if '\\' in name or not name.startswith(prefix):
            raise SystemExit(f'Unexpected archive path: {name!r}')
        raw = name[len(prefix):]
        relative = PurePosixPath(raw)
        if (not raw or raw.startswith('/') or relative.is_absolute()
                or any(p in {'', '.', '..', '.git'} for p in relative.parts)):
            raise SystemExit(f'Unsafe archive path: {name!r}')
        mode = info.external_attr >> 16
        if stat.S_IFMT(mode) == stat.S_IFLNK:
            raise SystemExit(f'Symlink rejected: {name!r}')
        case_key = raw.casefold()
        if case_key in seen:
            raise SystemExit(f'Duplicate / case-colliding path: {name!r}')
        seen.add(case_key)
        if info.file_size > 5_000_000:
            raise SystemExit(f'Oversized entry: {name!r}')
        entries.append((info, raw))
    if len(entries) != expected_file_count:
        raise SystemExit(f'Expected {expected_file_count} source files, found {len(entries)}')
    total = sum(info.file_size for info, _ in entries)
    if total > 10_000_000:
        raise SystemExit(f'Oversized extracted content: {total}')
    for info, rel in entries:
        target_rel = ('docs/historical/m1d-README.md'
                      if rel == 'README.md' else rel)
        if rel == '.gitignore':
            continue
        target = (root / target_rel).resolve()
        if not target.is_relative_to(root):
            raise SystemExit(f'Path escape: {rel!r}')
        if target.exists():
            raise SystemExit(f'Refusing to overwrite existing file: {target_rel}')
        target.parent.mkdir(parents=True, exist_ok=True)
        with z.open(info) as src, target.open('wb') as dst:
            while chunk := src.read(65536):
                dst.write(chunk)
print(f'Imported {len(entries)} entries: README preserved historically; existing .gitignore retained.')
PY

echo 'Review imported files and run the local test matrix before staging.'
git status --short
