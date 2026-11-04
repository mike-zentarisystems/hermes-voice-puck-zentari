"""Real HTTPS/WSS standalone entrypoint; synthetic local chat and PCM adapters only."""
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import socket
import ssl
import sys
import tempfile
import subprocess
import unittest
from unittest.mock import patch
import uuid

from aiohttp import ClientSession, CookieJar, web
from aiohttp.test_utils import TestServer

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import server


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


@unittest.skipUnless(os.name == 'posix', 'Linux-only private setup and signal lifecycle')
class StandaloneTLS(unittest.IsolatedAsyncioTestCase):
    async def test_setup_login_real_wss_text_and_logout(self):
        spec = importlib.util.spec_from_file_location('public_setup', ROOT / 'tools/setup.py')
        setup = importlib.util.module_from_spec(spec); spec.loader.exec_module(setup)
        app = web.Application()
        async def answer(request):
            payload = await request.json()
            self.assertEqual(payload['messages'][-1]['content'], 'Hello fixture')
            return web.json_response({'choices': [{'message': {'content': 'Standalone fixture reply.'}}]})
        app.router.add_post('/v1/chat/completions', answer)
        backend = TestServer(app); await backend.start_server()
        class Stt:
            ready = True
            async def transcribe(self, pcm): return 'Hello fixture'
        class Tts:
            ready = True
            async def start(self): pass
            async def close(self): pass
            async def synthesize(self, text): return b'\0\0' * 1024
        class Gateway(server.VoiceGateway):
            def __init__(self, registry):
                super().__init__(registry); self.stt, self.tts = Stt(), Tts()
        try:
            with tempfile.TemporaryDirectory() as temp:
                directory = Path(temp) / 'config'
                origin = f'https://localhost:{free_port()}'
                setup.initialize(directory, 'fixture-password-only-123', origin,
                                 str(backend.make_url('/v1')), 'fixture', 'Friend', 1)
                with self.assertRaises(ValueError):
                    setup.initialize(directory, 'fixture-password-only-123', origin,
                                     str(backend.make_url('/v1')), 'fixture', 'Friend', 1)
                enrollment = directory / 'browser.json'
                data = json.loads(enrollment.read_text())
                data['gateway_origin'] = f'wss://localhost:{free_port()}/'
                enrollment.write_text(json.dumps(data))
                leaf = subprocess.check_output(['openssl', 'x509', '-in', str(directory / 'server.pem'), '-text', '-noout']).decode()
                self.assertIn('CA:FALSE', leaf)
                self.assertNotIn('CA:TRUE', leaf)
                self.assertFalse((directory / 'ca-key.pem').exists())
                context = ssl.create_default_context(cafile=str(directory / 'ca.pem'))
                with patch.dict(os.environ, {'ZENTI_CONFIG': str(directory)}), patch.object(server, 'VoiceGateway', Gateway):
                    task = asyncio.create_task(server.main())
                    try:
                        async with ClientSession(cookie_jar=CookieJar()) as http:
                            for attempt in range(100):
                                if task.done(): await task
                                try:
                                    async with http.get(origin + '/healthz', ssl=context) as health:
                                        self.assertEqual(health.status, 200); break
                                except OSError: await asyncio.sleep(.05)
                            else: self.fail('standalone TLS startup timed out')
                            headers = {'Origin': origin}
                            async with http.post(origin + '/api/login', json={'password': 'fixture-password-only-123'},
                                                 headers=headers, ssl=context) as login:
                                self.assertEqual(login.status, 200)
                                info = await login.json()
                                self.assertIn('HttpOnly', login.headers['Set-Cookie'])
                            async with http.ws_connect(origin + '/gateway', protocols=[server.PROTOCOL], headers=headers, ssl=context) as ws:
                                await ws.send_json({'v': 1, 'type': 'device.hello', 'id': str(uuid.uuid4()), 'firmware': '0.3.0'})
                                accepted = await ws.receive_json(timeout=3)
                                self.assertEqual(accepted['type'], 'session.accepted')
                                turn = str(uuid.uuid4())
                                def event(kind, seq, **fields):
                                    return dict(v=1, type=kind, device_id=data['device_id'],
                                                session_id=accepted['session_id'], correlation_id=turn, seq=seq, **fields)
                                await ws.send_json(event('conversation.open', 1))
                                self.assertEqual((await ws.receive_json(timeout=3))['type'], 'conversation.ready')
                                await ws.send_json(event('text.input', 2, text='Hello fixture'))
                                found = []
                                while True:
                                    value = await ws.receive_json(timeout=3); found.append(value)
                                    if value['type'] == 'audio.output.stop': break
                                    self.assertNotEqual(value['type'], 'error')
                                self.assertEqual(next(x['text'] for x in found if x['type'] == 'assistant.text'), 'Standalone fixture reply.')
                            async with http.post(origin + '/api/logout', headers={**headers, 'X-Zenti-CSRF': info['csrf']}, ssl=context) as logout:
                                self.assertEqual(logout.status, 204)
                            async with http.get(origin + '/api/session', ssl=context) as response:
                                self.assertEqual(response.status, 401)
                    finally:
                        task.cancel()
                        await asyncio.gather(task, return_exceptions=True)
        finally:
            await backend.close()


if __name__ == '__main__':
    unittest.main()
