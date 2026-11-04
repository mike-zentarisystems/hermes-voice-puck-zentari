#!/usr/bin/env python3
"""Exercise a dedicated test owner's real local-model stack; resets its RAM context.

No transcript or audio is written. Password input is a private file, never argv.
Run only against a dedicated test configuration, not an active user's conversation.
"""
import argparse
import asyncio
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import ssl
import stat
import sys
import time
import uuid

from aiohttp import ClientSession

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'pwa'), str(ROOT / 'services/voice-gateway')]
from session_bridge import private_json, PROTOCOL
from local_adapters import LocalTts


def require(condition, category):
    if not condition: raise ValueError(category)


def private_password(path):
    with path.open('rb') as stream:
        mode = os.fstat(stream.fileno()).st_mode
        require(not path.is_symlink() and stat.S_ISREG(mode) and not mode & 0o077, 'password_file_permissions')
        raw = stream.read(257)
    require(0 < len(raw) <= 256, 'password_file_size')
    return raw.decode().strip()


async def exercise(directory, password_file, tts_model):
    settings = private_json(directory / 'settings.json')
    origin = settings['origin']
    require(origin.startswith('https://'), 'https_required')
    context = ssl.create_default_context(cafile=str(directory / 'ca.pem'))
    turns, checks = [], {}
    async with ClientSession(trust_env=False) as http:
        async with http.post(origin + '/api/login', json={'password': private_password(password_file)},
                             headers={'Origin': origin}, ssl=context) as response:
            require(response.status == 200, 'login_failed')
            session_info = await response.json()
        async with http.ws_connect(origin + '/gateway', protocols=[PROTOCOL],
                                   headers={'Origin': origin}, ssl=context) as ws:
            await ws.send_json(dict(v=1, type='device.hello', id=str(uuid.uuid4()), firmware='0.3.0'))
            accepted = await ws.receive_json(timeout=10)
            require(accepted.get('type') == 'session.accepted', 'gateway_authentication')
            seq, output_seq = 0, 0
            async def send(kind, correlation, **fields):
                nonlocal seq
                seq += 1
                await ws.send_json(dict(v=1, type=kind, device_id=accepted['device_id'],
                                        session_id=accepted['session_id'], correlation_id=correlation, seq=seq, **fields))
            async def receive():
                nonlocal output_seq
                event = await ws.receive_json(timeout=160)
                require(event.get('seq') == output_seq + 1, 'gateway_sequence')
                output_seq += 1
                require(event.get('type') != 'error', 'gateway_turn_failed')
                return event
            async def control(kind):
                correlation = str(uuid.uuid4()); await send(kind, correlation)
                event = await receive()
                require(event.get('type') == 'conversation.ready' and event.get('correlation_id') == correlation, 'conversation_control')
                return event['conversation_id']
            async def turn(text=None, pcm=None, canceled=None):
                import base64
                correlation = str(uuid.uuid4()); started = time.monotonic()
                if text is not None: await send('text.input', correlation, text=text)
                else:
                    await send('audio.input.start', correlation, format={'encoding':'pcm_s16le','sample_rate':16000,'channels':1})
                    for offset in range(0, len(pcm), 1024):
                        await send('audio.input.chunk', correlation, data=base64.b64encode(pcm[offset:offset+1024]).decode())
                    await send('audio.input.stop', correlation)
                audio, answer, transcript, first_audio = bytearray(), '', '', None
                async with asyncio.timeout(160):
                    while True:
                        event = await receive()
                        if event.get('correlation_id') != correlation:
                            require(event.get('correlation_id') == canceled and not event['type'].startswith('audio.output'), 'late_audio_after_cancel')
                            continue
                        if event['type'] == 'assistant.text': answer = event['text']
                        if event['type'] == 'transcript.final': transcript = event['text']
                        if event['type'] == 'audio.output.chunk':
                            if first_audio is None: first_audio = time.monotonic() - started
                            audio.extend(base64.b64decode(event['data'], validate=True))
                        if event['type'] == 'audio.output.stop': break
                require(answer.strip() and 0 < len(audio) <= 1920000 and not len(audio) % 2, 'real_reply_missing')
                samples = array('h', audio)
                rms = math.sqrt(sum(x*x for x in samples) / len(samples))
                require(rms > 10, 'silent_output')
                turns.append(dict(input='text' if text is not None else 'audio',
                                  first_audio_ms=round(first_audio*1000), total_ms=round((time.monotonic()-started)*1000),
                                  pcm_bytes=len(audio), pcm_sha256=hashlib.sha256(audio).hexdigest(), rms=round(rms,1)))
                return answer, transcript
            first = await control('conversation.reset')
            answer, _ = await turn('Our project code word is canyon. Reply with only canyon.')
            require('canyon' in answer.lower(), 'text_reply')
            checks['text_reply'] = True
            answer, _ = await turn('What is our project code word? Reply with only the word.')
            require('canyon' in answer.lower(), 'context_recall'); checks['context_recall'] = True
            second = await control('conversation.reset')
            require(first != second, 'reset_identity')
            answer, _ = await turn('What is our project code word? If none was supplied, say unknown.')
            require('canyon' not in answer.lower(), 'reset_context'); checks['context_reset'] = True
            canceled = str(uuid.uuid4())
            await send('text.input', canceled, text='Explain the solar system in five sentences.')
            while (await receive())['type'] != 'assistant.thinking': pass
            await send('cancel', canceled)
            await turn('Reply with only hello.', canceled=canceled); checks['cancellation_recovery'] = True
            tts = LocalTts(str(tts_model))
            try:
                pcm = await tts.synthesize('Please say hello.')
            finally:
                await tts.close()
            require(len(pcm) <= 480000, 'speech_input_limit')
            _, transcript = await turn(pcm=pcm)
            require('hello' in transcript.lower(), 'real_stt_transcript'); checks['real_speech_roundtrip'] = True
            await control('conversation.reset')
        async with http.post(origin + '/api/logout', headers={'Origin':origin,'X-Zenti-CSRF':session_info['csrf']}, ssl=context) as response:
            require(response.status == 204, 'logout')
    return {'checks':checks, 'turns':turns, 'speech_input':'locally synthesized public phrase; no physical microphone',
            'note':'Real STT/chat/TTS inference and verified HTTPS/WSS. No audio or transcript retained.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--password-file', type=Path, required=True)
    parser.add_argument('--tts-model', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(asyncio.run(exercise(args.config, args.password_file, args.tts_model)), indent=2))
    except Exception:
        raise SystemExit('Local acceptance failed; inspect sanitized gateway diagnostics privately.') from None
