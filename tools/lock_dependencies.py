#!/usr/bin/env python3
"""Convert a pip --dry-run --report resolution into an exact wheel lock and SBOM."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def lock(report, root):
    data = json.loads(report.read_text(encoding='utf-8'))
    rows, components = [], []
    for item in sorted(data['install'], key=lambda x: x['metadata']['name'].lower()):
        metadata = item['metadata']
        name, version = metadata['name'], metadata['version']
        digest = item['download_info']['archive_info']['hashes']['sha256']
        rows.append(f'{name}=={version} --hash=sha256:{digest}')
        component = {'type': 'library', 'name': name, 'version': version,
                     'purl': f'pkg:pypi/{name.lower().replace("_", "-")}@{version}',
                     'hashes': [{'alg': 'SHA-256', 'content': digest}]}
        license = metadata.get('license_expression') or metadata.get('license')
        if license and len(license) < 256: component['licenses'] = [{'license': {'name': license}}]
        components.append(component)
    (root / 'requirements.lock').write_text('# Linux amd64 / CPython 3.13 wheel lock. Regenerate with tools/lock_dependencies.py.\n' + '\n'.join(rows) + '\n')
    sbom = {'bomFormat': 'CycloneDX', 'specVersion': '1.5', 'version': 1,
            'metadata': {'component': {'type': 'application', 'name': 'Zenti', 'version': '0.1.0-preview'}},
            'components': components}
    (root / 'SBOM.cdx.json').write_text(json.dumps(sbom, indent=2) + '\n', encoding='utf-8')
    print(f'Locked {len(rows)} resolved Python packages; model, OS and firmware licenses are separate.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    lock(args.report, args.root)
