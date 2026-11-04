#!/usr/bin/env python3
"""Standalone HTTPS browser bridge and authenticated WSS device gateway."""
import asyncio
import json
import logging
import os
from pathlib import Path
import signal
import ssl
import stat
import sys
import time
from urllib.parse import urlsplit

from aiohttp import ClientSession, web
from websockets.legacy.server import serve

ROOT = Path(__file__).resolve().parent
sys.path[:0] = [str(ROOT / 'pwa'), str(ROOT / 'services/voice-gateway')]
from session_bridge import Config, create_app, private_json
from voice_gateway import Registry, VoiceGateway, PROTOCOL, MAX_FRAME_BYTES
from openai_adapter import OpenAIChat
from voice_turn import TurnError


class ExpiringRegistry(Registry):
    """Check expiry at authentication and for each established-session event."""
    def get(self, device_id, generation):
        item = super().get(device_id, generation)
        data = json.loads(self.path.read_text())
        row = next((x for x in data['devices'] if x.get('device_id') == device_id
                    and x.get('generation') == generation), None)
        expires = row.get('expires_at') if row else None
        return item if type(expires) is int and expires > time.time() else None

    @property
    def ready(self):
        try:
            self.reload()
            return any(not item.revoked and self.get(item.device_id, item.generation) is not None
                       for item in self._devices.values())
        except (OSError, ValueError, KeyError, TypeError):
            return False


async def main():
    logging.basicConfig(level=logging.INFO, format='%(message)s')
    directory = Path(os.getenv('ZENTI_CONFIG', '/run/zenti'))
    settings = private_json(directory / 'settings.json')
    os.environ['VOICE_APPROVED_PROFILES'] = str(directory / 'profiles.json')
    # Never enable private agent sockets from inherited environment.
    os.environ.pop('VOICE_AGENT_SOCKET', None)
    os.environ.pop('VOICE_HERMES_SOCKET', None)
    os.environ.pop('VOICE_PROFILE_CONTEXTS', None)
    registry = ExpiringRegistry(directory / 'devices.json')
    registry.reload(force=True)
    gateway = VoiceGateway(registry)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(directory / 'server.pem', directory / 'server-key.pem')
    config = Config.load(settings['origin'], directory / 'browser.json', directory / 'password.json',
                         ROOT / 'pwa/client', str(directory / 'ca.pem'))
    async def health(request):
        ready = registry.ready and gateway.stt.ready and gateway.tts.ready
        return web.json_response({'status': 'ok' if ready else 'models_unavailable'}, status=200 if ready else 503)
    # Insert specific health route before the static catch-all.
    app = create_app(config, serve_static=False)
    app.router.add_get('/healthz', health)
    app.router.add_get('/{path:.*}', app['bridge'].static)
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        asyncio.get_running_loop().add_signal_handler(sig, stop.set)
    connections = set()
    async def handle(ws):
        if len(connections) >= 16:
            await ws.close(code=1013, reason='capacity'); return
        connections.add(ws)
        try:
            await gateway.handle(ws)
        finally:
            connections.discard(ws)
    async with ClientSession(trust_env=False) as http:
        key = ''
        key_path = directory / 'backend-key'
        if key_path.exists() or key_path.is_symlink():
            with key_path.open('rb') as stream:
                mode = os.fstat(stream.fileno()).st_mode
                if key_path.is_symlink() or not stat.S_ISREG(mode) or mode & 0o077:
                    raise ValueError('backend key requires private file permissions')
                raw = stream.read(8193)
            if len(raw) > 8192: raise ValueError('backend key too large')
            key = raw.decode('utf-8').strip()
        gateway.hermes = OpenAIChat(http, settings['backend_url'], settings['model'], key,
                                   settings.get('allow_remote', False))
        runner = web.AppRunner(app, access_log=None)
        try:
            await gateway.tts.start()
            await runner.setup()
            site = web.TCPSite(runner, '0.0.0.0', urlsplit(config.origin).port or 443, ssl_context=context)
            await site.start()
            async with serve(handle, '0.0.0.0', urlsplit(config.gateway).port or 443, ssl=context, subprotocols=[PROTOCOL],
                             max_size=MAX_FRAME_BYTES, max_queue=8, ping_interval=20,
                             ping_timeout=10, close_timeout=3):
                logging.info('{"event":"standalone_ready"}')
                await stop.wait()
        finally:
            gateway.shutting_down = True
            await runner.cleanup()
            await gateway.tts.close()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except (OSError, ValueError, KeyError, TurnError):
        raise SystemExit('Startup failed: check private configuration, TLS and model paths with tools/doctor.py') from None
