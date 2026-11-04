from __future__ import annotations

import asyncio
import base64
import importlib.util
import json
import sys
import tempfile
import types
import unittest
import uuid
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "services" / "voice-gateway" / "voice_gateway.py"
sys.path.insert(0, str(MODULE_PATH.parent))

# Protocol unit tests do not open a socket. Keep repository validation usable
# on clean hosts while the image build remains responsible for installing the
# pinned runtime dependency.
if importlib.util.find_spec("websockets") is None:
    websockets = types.ModuleType("websockets")
    exceptions = types.ModuleType("websockets.exceptions")
    legacy = types.ModuleType("websockets.legacy")
    server = types.ModuleType("websockets.legacy.server")

    class ConnectionClosed(Exception):
        pass

    class WebSocketServerProtocol:
        pass

    async def serve(*_args, **_kwargs):
        raise RuntimeError("websockets is required to run the gateway")

    exceptions.ConnectionClosed = ConnectionClosed
    server.WebSocketServerProtocol = WebSocketServerProtocol
    server.serve = serve
    sys.modules.update({
        "websockets": websockets,
        "websockets.exceptions": exceptions,
        "websockets.legacy": legacy,
        "websockets.legacy.server": server,
    })

SPEC = importlib.util.spec_from_file_location("voice_gateway", MODULE_PATH)
gateway = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = gateway
SPEC.loader.exec_module(gateway)

SECRET = bytes(range(32))


class FakeWebSocket:
    def __init__(self, incoming, subprotocol=gateway.PROTOCOL, events=()):
        self.incoming = iter(incoming)
        self.events = iter(events)
        self.subprotocol = subprotocol
        self.sent = []
        self.closed = False
        self.close_code = None
        self.close_rcvd = None
        self.close_sent = None
        self.event_delay = 0

    async def recv(self):
        return next(self.incoming)

    async def send(self, raw):
        self.sent.append(json.loads(raw))

    async def close(self, code=1000, reason=""):
        self.closed = True
        self.close_code = code
        self.close_sent = types.SimpleNamespace(code=code, reason=reason)

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self.event_delay:
            await asyncio.sleep(self.event_delay)
        try:
            value = next(self.events)
        except StopIteration:
            raise StopAsyncIteration from None
        if isinstance(value, Exception):
            raise value
        return value


class VoiceGatewayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.registry_path = Path(self.temp.name) / "devices.json"
        self.write_registry(status="active")
        self.registry = gateway.Registry(self.registry_path)
        self.registry.reload(force=True)

    def tearDown(self):
        self.temp.cleanup()

    def write_registry(self, status="active"):
        self.registry_path.write_text(json.dumps({
            "version": 1,
            "devices": [{
                "device_id": "puck-lab-0001",
                "generation": 1,
                "status": status,
                "secret": "base64:" + base64.b64encode(SECRET).decode(),
            }],
        }), encoding="utf-8")

    def hello(self):
        return {"v": 1, "type": "device.hello", "id": str(uuid.uuid4()),
                "device_id": "puck-lab-0001", "board_id": "waveshare-esp32-s3-touch-lcd-1.46b",
                "firmware": "0.1.0"}

    def authenticated_socket(self, events=()):
        hello = self.hello()
        ws = FakeWebSocket([json.dumps(hello)], events=events)
        original_recv = ws.recv
        calls = 0

        async def recv():
            nonlocal calls
            calls += 1
            if calls == 1:
                return await original_recv()
            challenge = ws.sent[0]
            credential = self.registry.get("puck-lab-0001", 1)
            proof = gateway.expected_proof(credential, challenge["challenge_id"], challenge["nonce"], hello["id"])
            return json.dumps({"v": 1, "type": "auth.prove", "challenge_id": challenge["challenge_id"],
                               "hello_id": hello["id"], "device_id": "puck-lab-0001",
                               "generation": 1, "proof": proof})
        ws.recv = recv
        return ws

    async def test_peer_close_precedes_cleanup_without_logging_reason(self):
        service = gateway.VoiceGateway(self.registry)
        ws = self.authenticated_socket()
        ws.close_rcvd = types.SimpleNamespace(code=1000, reason="PRIVATE_SENTINEL")
        order = []
        emit = service.log.emit

        def record(event, **fields):
            order.append(event)
            emit(event, **fields)

        async def cleanup(_session):
            order.append("cleanup")
            await asyncio.sleep(0.01)

        service.log.emit = record
        with patch.object(gateway.VoiceTurnSession, "close", cleanup), self.assertLogs("voice_gateway", level="INFO") as logs:
            await service.handle(ws)
        records = [json.loads(x.getMessage()) for x in logs.records]
        ended = next(x for x in records if x['event'] == 'session_ended')
        closed = next(x for x in records if x['event'] == 'session_closed')
        self.assertEqual(ended['close_origin'], 'peer_close')
        self.assertEqual(ended['close_code_received'], 1000)
        self.assertGreaterEqual(ended['session_age_ms'], 0)
        self.assertGreaterEqual(closed['cleanup_ms'], 0)
        self.assertLess(order.index('session_ended'), order.index('cleanup'))
        self.assertLess(order.index('cleanup'), order.index('session_closed'))
        self.assertNotIn('PRIVATE_SENTINEL', '\n'.join(logs.output))

    async def test_abrupt_transport_loss_is_distinct_from_expiry(self):
        ws = self.authenticated_socket([gateway.ConnectionClosed(None, None)])
        service = gateway.VoiceGateway(self.registry)
        with self.assertLogs("voice_gateway", level="INFO") as logs:
            await service.handle(ws)
        ended = next(json.loads(x.getMessage()) for x in logs.records if json.loads(x.getMessage())['event'] == 'session_ended')
        self.assertEqual(ended['close_origin'], 'transport_lost')
        self.assertNotIn('close_code_received', ended)
        self.assertFalse(ws.closed)

    async def test_session_expiry_keeps_timeout_contract_and_numeric_close(self):
        ws = self.authenticated_socket()
        ws.event_delay = 0.1
        service = gateway.VoiceGateway(self.registry, session_ttl=0.01)
        with self.assertLogs("voice_gateway", level="INFO") as logs:
            await service.handle(ws)
        records = [json.loads(x.getMessage()) for x in logs.records]
        ended = next(x for x in records if x['event'] == 'session_ended')
        self.assertEqual(ended['close_origin'], 'session_expired')
        self.assertEqual(ended['close_code_sent'], 1008)
        self.assertEqual(ws.sent[-1]['reason'], 'timeout')
        self.assertTrue(any(x['event'] == 'session_rejected' and x['category'] == 'timeout' for x in records))

    async def test_client_close_is_explicit(self):
        ws = self.authenticated_socket([json.dumps({'type': 'session.close'})])
        service = gateway.VoiceGateway(self.registry)
        with self.assertLogs("voice_gateway", level="INFO") as logs:
            await service.handle(ws)
        ended = next(json.loads(x.getMessage()) for x in logs.records if json.loads(x.getMessage())['event'] == 'session_ended')
        self.assertEqual(ended['close_origin'], 'client_close')
        self.assertEqual(ended['close_code_sent'], 1000)

    async def test_exception_close_retains_codes_but_never_peer_reason(self):
        error = gateway.ConnectionClosed(None, None)
        error.rcvd = types.SimpleNamespace(code=1011, reason='PRIVATE_SENTINEL')
        error.sent = types.SimpleNamespace(code=1011, reason='PRIVATE_SENTINEL')
        ws = self.authenticated_socket([error])
        with self.assertLogs('voice_gateway', level='INFO') as logs:
            await gateway.VoiceGateway(self.registry).handle(ws)
        ended = next(json.loads(x.getMessage()) for x in logs.records if json.loads(x.getMessage())['event'] == 'session_ended')
        self.assertEqual(ended['close_origin'], 'peer_close')
        self.assertEqual(ended['close_code_received'], 1011)
        self.assertEqual(ended['close_code_sent'], 1011)
        self.assertNotIn('PRIVATE_SENTINEL', '\n'.join(logs.output))

    async def test_shutdown_cancellation_retains_cleanup_and_cancellation(self):
        ws = self.authenticated_socket()
        ws.event_delay = 1
        service = gateway.VoiceGateway(self.registry)
        accepted = asyncio.Event()
        original_send = ws.send

        async def send(raw):
            await original_send(raw)
            if json.loads(raw)['type'] == 'session.accepted':
                accepted.set()

        ws.send = send
        with self.assertLogs('voice_gateway', level='INFO') as logs:
            task = asyncio.create_task(service.handle(ws))
            await asyncio.wait_for(accepted.wait(), 1)
            service.shutting_down = True
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        records = [json.loads(x.getMessage()) for x in logs.records]
        self.assertEqual(next(x for x in records if x['event'] == 'session_ended')['close_origin'], 'server_shutdown')
        self.assertTrue(any(x['event'] == 'session_closed' for x in records))

    async def test_cleanup_failure_still_emits_finished_timing(self):
        ws = self.authenticated_socket()

        async def cleanup(_session):
            raise RuntimeError('synthetic cleanup failure')

        with patch.object(gateway.VoiceTurnSession, 'close', cleanup), self.assertLogs('voice_gateway', level='INFO') as logs:
            with self.assertRaises(RuntimeError):
                await gateway.VoiceGateway(self.registry).handle(ws)
        records = [json.loads(x.getMessage()) for x in logs.records]
        self.assertEqual([x['event'] for x in records][-2:], ['session_ended', 'session_closed'])

    async def test_close_metadata_rejects_peer_text_and_non_integer_codes(self):
        for value in (None, True, '1000 PRIVATE_SENTINEL', 1000.5, 999, 5000):
            self.assertIsNone(gateway._close_code(types.SimpleNamespace(code=value, reason='PRIVATE_SENTINEL')))
        self.assertEqual(gateway._close_code(types.SimpleNamespace(code=1008, reason='PRIVATE_SENTINEL')), 1008)
        with self.assertLogs("voice_gateway", level="INFO") as logs:
            gateway.JsonLogger().emit('session_ended', close_origin='peer_close', close_reason='PRIVATE_SENTINEL')
        self.assertNotIn('PRIVATE_SENTINEL', '\n'.join(logs.output))

    async def test_voice_readiness_is_separate_from_m2_readiness(self):
        service = gateway.VoiceGateway(self.registry)
        class Writer:
            def __init__(self): self.data = bytearray()
            def write(self, data): self.data.extend(data)
            async def drain(self): pass
            def close(self): pass
            async def wait_closed(self): pass
        async def probe(path):
            reader = asyncio.StreamReader()
            reader.feed_data(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
            reader.feed_eof()
            writer = Writer()
            await gateway.health_handler(reader, writer, service)
            return bytes(writer.data)
        self.assertIn(b"200 OK", await probe("/readyz"))
        self.assertIn(b"503 Service Unavailable", await probe("/voice-readyz"))

    async def test_voice_assets_readiness_requires_complete_local_files(self):
        from local_adapters import LocalStt, LocalTts
        stt = LocalStt(self.temp.name)
        voice = Path(self.temp.name) / "voice.onnx"
        tts = LocalTts(str(voice))
        self.assertFalse(stt.ready)
        self.assertFalse(tts.ready)
        for name in ("model.bin", "config.json", "tokenizer.json", "vocabulary.txt"):
            (Path(self.temp.name) / name).touch()
        voice.touch()
        self.assertFalse(tts.ready)
        Path(str(voice) + ".json").touch()
        self.assertTrue(stt.ready)
        self.assertTrue(tts.ready)

    async def test_valid_proof_accepts_session(self):
        hello = self.hello()
        ws = FakeWebSocket([json.dumps(hello)])
        service = gateway.VoiceGateway(self.registry)

        original_recv = ws.recv
        calls = 0

        async def recv():
            nonlocal calls
            calls += 1
            if calls == 1:
                return await original_recv()
            challenge = ws.sent[0]
            credential = self.registry.get("puck-lab-0001", 1)
            proof = gateway.expected_proof(credential, challenge["challenge_id"], challenge["nonce"], hello["id"])
            return json.dumps({"v": 1, "type": "auth.prove", "challenge_id": challenge["challenge_id"],
                               "hello_id": hello["id"], "device_id": "puck-lab-0001",
                               "generation": 1, "proof": proof})

        ws.recv = recv
        with self.assertLogs("voice_gateway", level="INFO") as logs:
            await service.handle(ws)
        self.assertEqual([item["type"] for item in ws.sent], ["auth.challenge", "session.accepted"])
        accepted = [json.loads(r.getMessage()) for r in logs.records if "session_accepted" in r.getMessage()]
        self.assertEqual(accepted[0]["generation"], 1)
        self.assertEqual(accepted[0]["device_id"], "puck-lab-0001")

    async def test_wrong_proof_fails_closed(self):
        hello = self.hello()
        service = gateway.VoiceGateway(self.registry)
        ws = FakeWebSocket([json.dumps(hello)])
        original_recv = ws.recv
        calls = 0

        async def recv():
            nonlocal calls
            calls += 1
            if calls == 1:
                return await original_recv()
            challenge = ws.sent[0]
            return json.dumps({"v": 1, "type": "auth.prove", "challenge_id": challenge["challenge_id"],
                               "hello_id": hello["id"], "device_id": "puck-lab-0001",
                               "generation": 1, "proof": "invalid"})

        ws.recv = recv
        await service.handle(ws)
        self.assertEqual(ws.sent[-1], {"v": 1, "type": "session.rejected", "reason": "auth_invalid"})
        self.assertEqual(ws.close_code, 1008)

    async def test_authenticated_non_object_event_is_bounded(self):
        hello = self.hello()
        ws = FakeWebSocket([json.dumps(hello)], events=["[]", '"scalar"',
                            json.dumps({"type": "session.close"})])
        service = gateway.VoiceGateway(self.registry)
        original_recv = ws.recv
        calls = 0

        async def recv():
            nonlocal calls
            calls += 1
            if calls == 1:
                return await original_recv()
            challenge = ws.sent[0]
            credential = self.registry.get("puck-lab-0001", 1)
            proof = gateway.expected_proof(credential, challenge["challenge_id"], challenge["nonce"], hello["id"])
            return json.dumps({"v": 1, "type": "auth.prove", "challenge_id": challenge["challenge_id"],
                               "hello_id": hello["id"], "device_id": "puck-lab-0001",
                               "generation": 1, "proof": proof})

        ws.recv = recv
        await service.handle(ws)
        self.assertEqual([item.get("code") for item in ws.sent[2:]],
                         ["event_invalid", "event_invalid"])
        self.assertEqual(ws.close_code, 1000)

    async def test_proof_from_previous_challenge_is_rejected(self):
        hello = self.hello()
        service = gateway.VoiceGateway(self.registry)
        credential = self.registry.get("puck-lab-0001", 1)
        stale = gateway.expected_proof(credential, str(uuid.uuid4()), gateway._b64url(bytes(32)), hello["id"])
        ws = FakeWebSocket([json.dumps(hello)])
        original_recv = ws.recv
        calls = 0

        async def recv():
            nonlocal calls
            calls += 1
            if calls == 1:
                return await original_recv()
            challenge = ws.sent[0]
            return json.dumps({"v": 1, "type": "auth.prove", "challenge_id": challenge["challenge_id"],
                               "hello_id": hello["id"], "device_id": "puck-lab-0001",
                               "generation": 1, "proof": stale})

        ws.recv = recv
        await service.handle(ws)
        self.assertEqual(ws.sent[-1]["reason"], "auth_invalid")

    async def test_revoked_generation_fails_closed(self):
        self.write_registry(status="revoked")
        self.registry.reload(force=True)
        self.assertFalse(self.registry.ready)
        hello = self.hello()
        service = gateway.VoiceGateway(self.registry)
        ws = FakeWebSocket([json.dumps(hello)])
        original_recv = ws.recv
        calls = 0

        async def recv():
            nonlocal calls
            calls += 1
            if calls == 1:
                return await original_recv()
            challenge = ws.sent[0]
            credential = self.registry.get("puck-lab-0001", 1)
            proof = gateway.expected_proof(credential, challenge["challenge_id"], challenge["nonce"], hello["id"])
            return json.dumps({"v": 1, "type": "auth.prove", "challenge_id": challenge["challenge_id"],
                               "hello_id": hello["id"], "device_id": "puck-lab-0001",
                               "generation": 1, "proof": proof})

        ws.recv = recv
        await service.handle(ws)
        self.assertEqual(ws.sent[-1]["type"], "session.rejected")

    async def test_malformed_hello_and_wrong_subprotocol_reject(self):
        service = gateway.VoiceGateway(self.registry)
        malformed = FakeWebSocket(["{}"])
        await service.handle(malformed)
        self.assertEqual(malformed.close_code, 1008)
        wrong = FakeWebSocket([], subprotocol=None)
        await service.handle(wrong)
        self.assertEqual(wrong.sent[-1]["reason"], "subprotocol")

    def test_registry_rejects_short_secret(self):
        self.registry_path.write_text(json.dumps({"version": 1, "devices": [{
            "device_id": "puck-lab-0001", "generation": 1, "status": "active",
            "secret": "base64:" + base64.b64encode(b"short").decode(),
        }]}), encoding="utf-8")
        with self.assertRaises(ValueError):
            gateway.Registry(self.registry_path).reload(force=True)

    def test_registry_supports_rotation_without_rebuild(self):
        second = bytes(reversed(range(32)))
        self.registry_path.write_text(json.dumps({"version": 1, "devices": [
            {"device_id": "puck-lab-0001", "generation": 1, "status": "revoked",
             "secret": "base64:" + base64.b64encode(SECRET).decode()},
            {"device_id": "puck-lab-0001", "generation": 2, "status": "active",
             "secret": "base64:" + base64.b64encode(second).decode()},
        ]}), encoding="utf-8")
        self.registry.reload(force=True)
        self.assertTrue(self.registry.get("puck-lab-0001", 1).revoked)
        self.assertFalse(self.registry.get("puck-lab-0001", 2).revoked)
        self.assertTrue(self.registry.ready)


if __name__ == "__main__":
    unittest.main()
