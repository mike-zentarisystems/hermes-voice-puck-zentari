#!/usr/bin/env python3
"""Export only the static scripted demo; never the authenticated client or configuration."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def build(output):
    if output.exists(): raise ValueError('site output must be new')
    output.mkdir(parents=True)
    for name in ('index.html', 'demo.css', 'demo.js'):
        shutil.copyfile(ROOT / 'site' / name, output / name)
    (output / 'avatars').mkdir()
    for name in ('zenti', 'orbit'):
        for state in ('idle', 'thinking'):
            shutil.copyfile(ROOT / f'pwa/client/avatars/{name}-{state}.png', output / f'avatars/{name}-{state}.png')
    (output / '.nojekyll').write_text('')
    print('Static demo built; no backend, telemetry, credentials or private infrastructure.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/site')
    build(parser.parse_args().output)
