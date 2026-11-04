#!/usr/bin/env python3
"""Safe local prerequisite check. Never prints configuration values or credentials."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import ssl
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def checks(root, live=False):
    results = []
    def record(name, ok, action):
        results.append({'check': name, 'status': 'pass' if ok else 'fail', 'action': '' if ok else action})
    for name in ('docker', 'openssl'):
        record(name, bool(shutil.which(name)), 'Install the documented prerequisite.')
    directory = root / '.runtime'
    record('private_directory', directory.is_dir() and not directory.is_symlink()
           and (os.name != 'posix' or not directory.stat().st_mode & 0o077), 'Run interactive tools/setup.py in Linux.')
    settings = None
    for name in ('settings.json', 'browser.json', 'password.json', 'profiles.json', 'devices.json', 'server-key.pem', 'server.pem', 'ca.pem'):
        path = directory / name
        ok = path.is_file() and not path.is_symlink()
        if ok and name not in ('server.pem', 'ca.pem') and os.name == 'posix': ok = not path.stat().st_mode & 0o077
        if ok and name.endswith('.json'):
            try:
                value = json.loads(path.read_text(encoding='utf-8'))
                if name == 'settings.json': settings = value
                if name == 'devices.json':
                    ok = all(type(x.get('expires_at')) is int and x['expires_at'] > time.time() for x in value['devices'])
            except (OSError, ValueError, KeyError, TypeError): ok = False
        record(name, ok, 'Repair private setup or renew credentials; never paste the file contents.')
    for name in ('stt/model.bin', 'stt/config.json', 'stt/tokenizer.json', 'voice.onnx', 'voice.onnx.json'):
        record('model:' + name, (root / 'models' / name).is_file(), 'Install a licensed model at the documented path.')
    record('model:stt/vocabulary', any((root / 'models/stt' / name).is_file()
           for name in ('vocabulary.txt', 'vocabulary.json')), 'Install the matching CTranslate2 vocabulary file.')
    if live:
        ok = False
        try:
            context = ssl.create_default_context(cafile=str(directory / 'ca.pem'))
            with urllib.request.urlopen(settings['origin'] + '/healthz', context=context, timeout=5) as response:
                ok = response.status == 200
        except Exception: pass
        record('https_health', ok, 'Run docker compose logs locally; verify TLS, mounts and model readiness.')
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--live', action='store_true')
    args = parser.parse_args()
    result = checks(args.root, args.live)
    print(json.dumps(result, indent=2))
    raise SystemExit(any(item['status'] == 'fail' for item in result))
