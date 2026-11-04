#!/usr/bin/env python3
"""Create deterministic source ZIP from the verified manifest, never from a directory glob."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from check_release import check, manifest

ROOT = Path(__file__).resolve().parents[1]


def package(root, output):
    errors, entries = check(root)
    manifest_bytes = (root / 'RELEASE-MANIFEST.json').read_bytes()
    recorded = json.loads(manifest_bytes)
    if errors or recorded != manifest(root, entries): raise ValueError('source scan or manifest verification failed')
    output.parent.mkdir(parents=True, exist_ok=True)
    sidecar = output.with_suffix(output.suffix + '.sha256')
    if sidecar.exists() or sidecar.is_symlink(): raise FileExistsError('checksum file already exists')
    with output.open('xb') as stream, zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted([item['path'] for item in entries] + ['RELEASE-MANIFEST.json']):
            path = root / name
            if not path.resolve().is_relative_to(root.resolve()) or path.is_symlink():
                raise ValueError('redirected manifest entry')
            data = manifest_bytes if name == 'RELEASE-MANIFEST.json' else path.read_bytes()
            expected = next((item['sha256'] for item in entries if item['path'] == name), None)
            if expected and hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('source changed during packaging')
            info = zipfile.ZipInfo('zenti-preview/' + name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    with sidecar.open('x', encoding='ascii', newline='\n') as stream:
        stream.write(digest + '  ' + output.name + '\n')
    print(f'Packaged {len(entries)+1} files; SHA256 {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    package(ROOT, args.output)
