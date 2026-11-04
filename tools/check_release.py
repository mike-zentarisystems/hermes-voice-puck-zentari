#!/usr/bin/env python3
"""Scan and fingerprint the exact source tree, excluding documented local outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SKIP = {'.git', '.runtime', 'models', 'node_modules', '__pycache__', 'build', 'out', 'dist', '.venv', '.pytest_cache'}
FORBIDDEN = [
    ('private-key', re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')),
    ('provider-key', re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|sk-proj-[A-Za-z0-9_-]{20,}|AKIA[A-Z0-9]{16})')),
    ('private-host', re.compile(rb'(?:tail[a-z0-9]+\.ts\.net|[a-z0-9-]+\.lan|(?<![\d.])(?:10(?:\.\d{1,3}){3}|(?:192\.168|172\.(?:1[6-9]|2\d|3[01])|100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7]))(?:\.\d{1,3}){2})(?![\d.]))')),
    ('private-path', re.compile(rb'''(?:[A-Za-z]:[/\\]Users[/\\][^/\\\r\n"']+|/home/[a-z0-9_-]+/|/(?:opt|srv)/[a-z0-9_-]+/|libfile_[a-f0-9]{20,})''')),
]


def inventory(root):
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part in SKIP for part in relative.parts): continue
        if path.is_symlink(): raise ValueError('symlink in release tree: ' + relative.as_posix())
        if path.is_file() and relative.as_posix() != 'RELEASE-MANIFEST.json':
            yield path, relative.as_posix()


def check(root):
    errors, entries = [], []
    for path, name in inventory(root):
        if ((path.name.startswith('.env') and path.name != '.env.example')
                or name.endswith(('.pem', '.key', '.p12', '.pfx', '.flashdump.bin'))):
            errors.append(f'{name}: forbidden credential/runtime file')
        raw = path.read_bytes()
        for kind, pattern in FORBIDDEN:
            if pattern.search(raw): errors.append(f'{name}: {kind}')
        entries.append({'path': name, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
    return errors, entries


def manifest(root, entries):
    return {'schema_version': 1, 'release_tier': 'preview-candidate',
            'files': entries, 'notice': 'Integrity inventory, not a signature or independent security clearance.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--write-manifest', action='store_true')
    parser.add_argument('--verify-manifest', action='store_true')
    args = parser.parse_args()
    errors, entries = check(args.root)
    data = manifest(args.root, entries)
    if args.verify_manifest:
        try:
            if json.loads((args.root / 'RELEASE-MANIFEST.json').read_text()) != data: errors.append('manifest drift')
        except (OSError, ValueError): errors.append('manifest missing or invalid')
    if errors:
        print('\n'.join(errors)); raise SystemExit(1)
    if args.write_manifest:
        (args.root / 'RELEASE-MANIFEST.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'PASS: {len(entries)} files scanned and fingerprinted. No private-history or independent-review claim.')
