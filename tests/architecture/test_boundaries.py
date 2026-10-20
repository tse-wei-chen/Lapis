#!/usr/bin/env python3
"""Fast dependency tripwire. Additional compiler dependency audits follow in phase 2."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
core = root / 'native' / 'MediaRuntime' / 'src' / 'core'
contracts = root / 'src' / 'Player.Contracts'
restricted = re.compile(
    r'#\s*include\s*[<"](?:windows\.h|d3d\w*\.h|dxgi\w*\.h|libav\w*/|libbluray/|pulse/|pipewire/|wayland-|X11/)|'
    r'\b(?:AVFormatContext|AVCodecContext|AVFrame|AVPacket|IDXGISwapChain|IWASAPIClient)\b',
    re.IGNORECASE,
)
violations = []
for directory in [core, contracts]:
    for path in directory.rglob('*'):
        if path.suffix not in {'.cpp', '.hpp', '.h', '.cs'}:
            continue
        for i, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            if restricted.search(line) and not line.lstrip().startswith('//'):
                violations.append(f'{path.relative_to(root)}:{i}: {line.strip()}')
if violations:
    raise SystemExit('Core boundary violations:\n' + '\n'.join(violations))
print('Architecture boundary checks passed')
