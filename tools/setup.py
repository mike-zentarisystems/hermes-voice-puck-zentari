#!/usr/bin/env python3
"""Interactive local setup. Writes new private files, never overwrites or enrolls production."""
import argparse
import base64
import getpass
import ipaddress
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time
import tempfile
from urllib.parse import urlsplit
import warnings

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'pwa'))
from passwords import password_record


def initialize(directory, password, origin, backend, model, name, days, allow_remote=False):
    if os.name != 'posix': raise ValueError('Run setup in Linux/WSL for enforceable private file modes')
    url = urlsplit(origin)
    if (url.scheme != 'https' or not url.hostname or url.username or url.password or url.path
            or url.query or url.fragment or not 1 <= days <= 365 or not 0 < len(name) <= 80):
        raise ValueError('invalid origin, name or lifetime')
    # Validate backend before creating any credentials. Reuse the runtime boundary.
    sys.path.insert(0, str(ROOT / 'services/voice-gateway'))
    from openai_adapter import OpenAIChat
    OpenAIChat(None, backend, model, allow_remote=allow_remote)
    record = password_record(password)
    if not shutil.which('openssl'): raise ValueError('OpenSSL is required')
    directory = directory.absolute()
    if directory.exists() or directory.is_symlink(): raise ValueError('configuration directory must be new')
    sys.path.insert(0, str(ROOT / 'tools'))
    from provision_pwa_password import private_directory
    descriptor = private_directory(directory.parent)
    os.close(descriptor)
    directory.mkdir(mode=0o700, parents=False)
    os.chmod(directory, 0o700)
    def write(name, value):
        with os.fdopen(os.open(directory / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
            json.dump(value, stream, indent=2); stream.write('\n')
    secret = 'base64:' + base64.b64encode(secrets.token_bytes(32)).decode()
    expires = int(time.time()) + days * 86400
    write('devices.json', {'version': 1, 'devices': [{'device_id': 'browser-local-0001', 'generation': 1,
          'status': 'active', 'secret': secret, 'expires_at': expires}]})
    write('browser.json', {'v': 1, 'type': 'device.credential', 'device_id': 'browser-local-0001',
          'generation': 1, 'gateway_origin': 'wss://localhost:8444/', 'credential': secret,
          'scope': ['session:connect', 'audio:stream', 'events:receive'], 'expires_at': expires})
    write('password.json', record)
    write('profiles.json', {'devices': {'browser-local-0001': 'local-owner'},
                           'profiles': {'local-owner': {'preferred_name': name}}})
    write('settings.json', {'origin': origin, 'backend_url': backend, 'model': model, 'allow_remote': allow_remote})
    try:
        ipaddress.ip_address(url.hostname); san = 'IP:' + url.hostname
    except ValueError:
        if not all(c.isalnum() or c in '.-' for c in url.hostname): raise ValueError('invalid TLS hostname')
        san = 'DNS:' + url.hostname
    sans = list(dict.fromkeys(['DNS:localhost', 'IP:127.0.0.1', san]))
    # The mounted server key must not be able to issue certificates for other sites.
    # Issue a leaf with an ephemeral authority key, then discard that signing key.
    with tempfile.TemporaryDirectory(prefix='zenti-local-authority-') as temporary:
        authority = Path(temporary)
        def openssl(*arguments):
            subprocess.run(['openssl', *map(str, arguments)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        openssl('req', '-x509', '-newkey', 'rsa:3072', '-nodes', '-days', days,
                '-keyout', authority / 'ca-key.pem', '-out', directory / 'ca.pem',
                '-subj', '/CN=Zenti local installation authority',
                '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
        openssl('req', '-new', '-newkey', 'rsa:3072', '-nodes',
                '-keyout', directory / 'server-key.pem', '-out', authority / 'server.csr',
                '-subj', '/CN=' + url.hostname)
        extensions = authority / 'extensions.cnf'
        extensions.write_text('basicConstraints=critical,CA:FALSE\n'
                              'keyUsage=critical,digitalSignature,keyEncipherment\n'
                              'extendedKeyUsage=serverAuth\nsubjectAltName=' + ','.join(sans) + '\n')
        openssl('x509', '-req', '-in', authority / 'server.csr', '-CA', directory / 'ca.pem',
                '-CAkey', authority / 'ca-key.pem', '-set_serial', secrets.randbits(128),
                '-days', days, '-extfile', extensions, '-out', directory / 'server.pem')
    os.chmod(directory / 'server-key.pem', 0o600)
    os.chmod(directory / 'server.pem', 0o644)
    os.chmod(directory / 'ca.pem', 0o644)
    print('Created private configuration. Trust only ca.pem after verifying its SHA-256 fingerprint locally.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT / '.runtime')
    parser.add_argument('--origin', default='https://localhost:8443')
    parser.add_argument('--backend', default='http://host.docker.internal:11434/v1')
    parser.add_argument('--model', default='qwen3.5:4b')
    parser.add_argument('--name', default='Friend')
    parser.add_argument('--days', type=int, default=30)
    parser.add_argument('--allow-remote', action='store_true')
    args = parser.parse_args()
    if not sys.stdin.isatty(): parser.error('interactive private terminal required')
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('error', getpass.GetPassWarning)
            password = getpass.getpass('New local sign-in password (16+ characters): ')
            if password != getpass.getpass('Confirm password: '): parser.error('passwords differ')
    except (getpass.GetPassWarning, EOFError):
        parser.error('hidden password input unavailable; use a private interactive Linux terminal')
    try:
        initialize(args.directory, password, args.origin, args.backend, args.model, args.name,
                   args.days, args.allow_remote)
    except (ValueError, OSError, subprocess.CalledProcessError):
        parser.exit(1, 'Setup incomplete; inspect local prerequisites and the private directory. Existing files were not overwritten.\n')
