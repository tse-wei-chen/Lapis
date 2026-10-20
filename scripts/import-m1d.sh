#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 1 ]]; then
  echo "Usage: bash scripts/import-m1d.sh /path/to/bdmv-player-v0.2-m1d.zip" >&2
  exit 2
fi
if ! command -v python3 >/dev/null; then
  echo "python3 is required to safely extract the source archive" >&2
  exit 2
fi
repo_root="$(git rev-parse --show-toplevel)"
branch="$(git -C "$repo_root" branch --show-current)"
if [[ "$branch" != "chore/bootstrap-m1d-baseline" ]]; then
  echo "Expected chore/bootstrap-m1d-baseline; current branch: $branch" >&2
  exit 2
fi
if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
  echo "Working tree is not clean. Stash or commit your changes first." >&2
  exit 2
fi

python3 - "$1" "$repo_root" <<'PY'
import pathlib
import sys
import zipfile

archive = pathlib.Path(sys.argv[1]).expanduser().resolve()
root = pathlib.Path(sys.argv[2]).resolve()
prefix = "bdmv-player-v0.2/"
if not archive.is_file():
    raise SystemExit(f"Archive not found: {archive}")
count = 0
with zipfile.ZipFile(archive) as z:
    for info in z.infolist():
        if info.is_dir():
            continue
        name = info.filename.replace("\\", "/")
        if not name.startswith(prefix):
            raise SystemExit(f"Unexpected archive entry: {name!r}")
        relative = pathlib.PurePosixPath(name[len(prefix):])
        if not relative.parts or ".." in relative.parts or ".git" in relative.parts:
            raise SystemExit(f"Unsafe archive path: {name!r}")
        if relative.parts[0] in {"build", ".build"} or "bin" in relative.parts or "obj" in relative.parts:
            continue
        target = (root.joinpath(*relative.parts)).resolve()
        if not target.is_relative_to(root):
            raise SystemExit(f"Path escape detected: {name!r}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(z.read(info))
        count += 1
print(f"Imported {count} source files into {root}")
PY
echo "Review git status and local tests before committing. This script does not push or merge."
git -C "$repo_root" status --short
