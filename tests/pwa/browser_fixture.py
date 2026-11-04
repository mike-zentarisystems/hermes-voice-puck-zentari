#!/usr/bin/env python3
"""Loopback-only browser test server. All credentials and speech adapters are FIXTURES.
ZV_CITADEL_GATEWAY_ROOT selects the actual Citadel gateway; otherwise a protocol fixture.
"""
import asyncio
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import uuid

from websockets.legacy.server import serve
from aiohttp import web
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'pwa'))
from session_bridge import Config, create_app, password_record

ROOT = Path(__file__).resolve().parents[2]
SECRET = b'x' * 32
DEVICE = 'pwa-test-0001'
PCM = {'encoding': 'pcm_s16le', 'sample_rate': 16000, 'channels': 1}
stats = {'auth': 0, 'stt': 0, 'input_bytes': 0, 'cancel': 0, 'output_chunks': 0}
mode = 'normal'
connections = set()
loop = None
fixture_context = {'id': str(uuid.uuid4()), 'history': []}




class Stt:
    ready = True
    async def transcribe(self, pcm):
        stats['stt'] += 1; stats['input_bytes'] = len(pcm)
        await asyncio.sleep(2 if mode == 'slow' else 0.1)
        return 'Synthetic browser microphone fixture'
class Hermes:
    ready = True
    async def answer(self, _text):
        if _text.endswith('What is my name?'):
            profile = json.loads(_text.split('Approved profile: ', 1)[1].split('\nConversation:', 1)[0])
            return 'Your name is ' + profile['preferred_name']
        if _text.endswith('What project?'):
            return 'Juniper' if 'Juniper' in _text.split('\nUser:')[0] else 'No project in context'
        return 'Synthetic browser response fixture'
class Tts:
    ready = True
    async def synthesize(self, _text): return b'\x00\x10' * (32000 if mode == 'long' else 16000)


async def fixture(ws):
    """Small CI protocol peer; actual-gateway mode is separate evidence."""
    hello = json.loads(await ws.recv()); hello_id = hello['id']
    challenge, nonce = str(uuid.uuid4()), 'a' * 43
    await ws.send(json.dumps({'v': 1, 'type': 'auth.challenge', 'challenge_id': challenge,
                             'hello_id': hello_id, 'nonce': nonce, 'expires_ms': int(time.time()*1000)+10000}))
    proof = json.loads(await ws.recv())
    canonical = f'zv1\n{challenge}\n{nonce}\n{DEVICE}\n1\n{hello_id}'.encode()
    expected = base64.urlsafe_b64encode(hmac.new(SECRET, canonical, hashlib.sha256).digest()).decode().rstrip('=')
    if proof.get('proof') != expected or hello.get('device_id') != DEVICE or proof.get('generation') != 1:
        await ws.send(json.dumps({'v': 1, 'type': 'session.rejected'})); await ws.close(1008); return
    session = str(uuid.uuid4()); stats['auth'] += 1
    await ws.send(json.dumps({'v': 1, 'type': 'session.accepted', 'device_id': DEVICE,
        'session_id': session, 'correlation_id': hello_id, 'expires_ms': int(time.time()*1000)+900000}))
    input_seq = 0; output_seq = 0; turn = None; task = None; samples = bytearray()
    async def emit(kind, correlation, **fields):
        nonlocal output_seq
        output_seq += 1
        await ws.send(json.dumps({'v': 1, 'type': kind, 'device_id': DEVICE, 'session_id': session,
            'correlation_id': correlation, 'seq': output_seq, **fields}))
    conversation = fixture_context['id']; history = fixture_context['history']
    async def respond(correlation, pcm=None, text=None):
        if text is None: text = await Stt().transcribe(pcm)
        await emit('transcript.final', correlation, text=text); await emit('assistant.thinking', correlation)
        prompt = 'Approved profile: {"preferred_name":"Alex"}\nConversation: ' + json.dumps(history) + '\nUser: ' + text
        answer = await Hermes().answer(prompt)
        history.extend([text, answer]); del history[:-16]
        await emit('assistant.text', correlation, text=answer)
        audio = await Tts().synthesize(text); await emit('audio.output.start', correlation, format=PCM)
        for offset in range(0, len(audio), 1024):
            await emit('audio.output.chunk', correlation, data=base64.b64encode(audio[offset:offset+1024]).decode())
            await asyncio.sleep(0.032)
        await emit('audio.output.stop', correlation)
    try:
        async for raw in ws:
            msg = json.loads(raw)
            assert msg['device_id'] == DEVICE and msg['session_id'] == session and msg['seq'] == input_seq + 1
            input_seq += 1
            if msg['type'] in ('conversation.open', 'conversation.reset'):
                if msg['type'] == 'conversation.reset':
                    conversation = str(uuid.uuid4()); fixture_context['id'] = conversation; history.clear()
                await emit('conversation.ready', msg['correlation_id'], conversation_id=conversation, principal='owner', capabilities=['text', 'context', 'reset'])
            elif msg['type'] == 'text.input':
                task = asyncio.create_task(respond(msg['correlation_id'], text=msg['text']))
            elif msg['type'] == 'audio.input.start':
                assert not task or task.done(); assert msg['format'] == PCM
                turn = msg['correlation_id']; samples.clear()
            elif msg['type'] == 'audio.input.chunk':
                assert msg['correlation_id'] == turn
                chunk = base64.b64decode(msg['data'], validate=True); assert 0 < len(chunk) <= 1024 and len(chunk) % 2 == 0
                samples.extend(chunk); assert len(samples) <= 480000
            elif msg['type'] == 'audio.input.stop': task = asyncio.create_task(respond(turn, bytes(samples)))
            elif msg['type'] == 'cancel':
                stats['cancel'] += 1
                if task: task.cancel(); await asyncio.gather(task, return_exceptions=True)
                task = None; turn = None; samples.clear()
    finally:
        if task: task.cancel(); await asyncio.gather(task, return_exceptions=True)


async def main():
    global loop
    loop = asyncio.get_running_loop()
    source = os.getenv('ZV_CITADEL_GATEWAY_ROOT')
    with tempfile.TemporaryDirectory(prefix='zenti-browser-fixture-') as tmp:
        temp = Path(tmp)
        openssl = os.getenv('ZV_TEST_OPENSSL') or shutil.which('openssl') or r'C:\Program Files\Git\usr\bin\openssl.exe'
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', str(temp/'key.pem'), '-out', str(temp/'cert.pem'), '-subj', '/CN=localhost',
            '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(temp/'cert.pem', temp/'key.pem')
        handler = fixture
        if source:
            sys.path.insert(0, source)
            from voice_gateway import VoiceGateway, Registry
            registry_path = temp/'devices.json'
            registry_path.write_text(json.dumps({'version': 1, 'devices': [{'device_id': DEVICE,
                'generation': 1, 'status': 'active', 'secret': 'base64:' + base64.b64encode(SECRET).decode()}]}))
            profiles = temp/'profiles.json'
            profiles.write_text(json.dumps({'devices': {DEVICE: 'owner'}, 'profiles': {'owner': {'preferred_name': 'Alex'}}}))
            os.environ['VOICE_APPROVED_PROFILES'] = str(profiles)
            gateway = VoiceGateway(Registry(registry_path)); gateway.stt = Stt(); gateway.hermes = Hermes(); gateway.tts = Tts()
            handler = gateway.handle
        async def handle(ws):
            connections.add(ws)
            original = ws.send
            async def send(raw):
                event = json.loads(raw)
                if source and event.get('type') == 'session.accepted': stats['auth'] += 1
                if event.get('type') == 'audio.output.chunk':
                    stats['output_chunks'] += 1
                    if mode == 'malformed': event['data'] = 'AA=='; raw = json.dumps(event)
                await original(raw)
            ws.send = send
            try: await handler(ws)
            except Exception: pass  # Negative authorization/disconnect tests intentionally close.
            finally: connections.discard(ws)
        async with serve(handle, '127.0.0.1', 0, ssl=context, subprotocols=['zentari.voice.v1'], max_size=4096, max_queue=8) as server:
            port = server.sockets[0].getsockname()[1]
            record = password_record('fixture-password-only-123')
            # Reserve an ephemeral HTTPS port through the aiohttp runner, then
            # set the exact fixture origin before receiving any browser traffic.
            config = Config('https://127.0.0.1', f'wss://127.0.0.1:{port}/', DEVICE, 1, SECRET,
                            bytes.fromhex(record['salt']), bytes.fromhex(record['hash']),
                            ROOT/'pwa/client', str(temp/'cert.pem'))
            app = create_app(config, serve_static=False)
            async def get_stats(request):
                return web.json_response(stats)
            async def disconnect(request):
                for ws in list(connections):
                    await ws.close(1012, 'fixture restart')
                return web.Response(status=204)
            app.router.add_get('/__test/stats', get_stats)
            app.router.add_post('/__test/disconnect', disconnect)
            app.router.add_get("/{path:.*}", app["bridge"].static)
            runner = web.AppRunner(app, access_log=None)
            await runner.setup()
            site = web.TCPSite(runner, '127.0.0.1', 0, ssl_context=context)
            await site.start()
            https_port = site._server.sockets[0].getsockname()[1]
            object.__setattr__(config, 'origin', f'https://127.0.0.1:{https_port}')
            print(json.dumps({'url': config.origin, 'gateway': config.gateway,
                'implementation': 'actual-citadel' if source else 'protocol-fixture'}), flush=True)
            try:
                await asyncio.Event().wait()
            finally:
                await runner.cleanup()


if __name__ == '__main__':
    asyncio.run(main())
