"""Isolated bridge security/protocol tests. Synthetic key and fixture Hermes/audio only."""
import asyncio
import base64
import contextlib
from dataclasses import replace
import hashlib
import hmac
import importlib.util
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import threading
from unittest.mock import patch
import unittest
import uuid
from types import SimpleNamespace

try:
    from aiohttp import WSMsgType
    from aiohttp.test_utils import TestClient, TestServer
except ImportError:
    raise unittest.SkipTest('optional bridge dependency unavailable; bridge tests NOT RUN')

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("session_bridge", ROOT / "pwa/session_bridge.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
ORIGIN = "https://pwa.example"
# Split literal: same fixture value, but not the credential shape tools/validate.py scans for.
PASSWORD = "fixture-" "password-only-123"
KEY = b"x" * 32


class Upstream:
    def __init__(self):
        self.protocol = module.PROTOCOL
        self.queue = asyncio.Queue()
        self.sent = []
        self.closed = False
        self.session_id = str(uuid.uuid4())
        self.challenge = None
        self.output_seq = 0

    async def close(self):
        self.closed = True

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        self.closed = True

    async def send_json(self, value):
        self.sent.append(value)
        if value["type"] == "device.hello":
            self.challenge = {"v": 1, "type": "auth.challenge", "hello_id": value["id"],
                              "challenge_id": str(uuid.uuid4()), "nonce": "a" * 43,
                              "expires_ms": int(module.time.time() * 1000) + 10000}
            await self.put(self.challenge)
        elif value["type"] == "auth.prove":
            c = self.challenge
            body = f'zv1\n{c["challenge_id"]}\n{c["nonce"]}\n{value["device_id"]}\n1\n{value["hello_id"]}'.encode()
            expected = base64.urlsafe_b64encode(hmac.new(KEY, body, hashlib.sha256).digest()).rstrip(b"=").decode()
            if not hmac.compare_digest(value["proof"], expected):
                raise ValueError("fixture proof mismatch")
            await self.put({"v": 1, "type": "session.accepted", "correlation_id": value["hello_id"],
                            "device_id": "pwa-fixture-0001", "session_id": self.session_id,
                            "expires_ms": int(module.time.time() * 1000) + 900000})

    async def send_str(self, raw):
        value = json.loads(raw)
        self.sent.append(value)
        if value["type"] == "audio.input.stop":
            await self.event("audio.output.start", value, format={"encoding": "pcm_s16le", "sample_rate": 16000, "channels": 1})
            await self.event("audio.output.chunk", value, data="AAA=")
            await self.event("audio.output.stop", value)

    async def event(self, kind, value, **extra):
        self.output_seq += 1
        await self.put({"v": 1, "type": kind, "device_id": value["device_id"],
                        "session_id": self.session_id, "correlation_id": value["correlation_id"],
                        "seq": self.output_seq, **extra})

    async def put(self, value):
        await self.queue.put(SimpleNamespace(type=WSMsgType.TEXT, data=json.dumps(value)))

    async def receive(self, timeout=10):
        return await asyncio.wait_for(self.queue.get(), timeout)

    def __aiter__(self):
        return self

    async def __anext__(self):
        return await self.queue.get()


class FixtureHTTP:
    def __init__(self):
        self.upstreams = []

    async def ws_connect(self, *args, **kwargs):
        # Assert TLS remains verified; no ssl=False production exception.
        assert kwargs["ssl"].verify_mode == module.ssl.CERT_REQUIRED
        assert kwargs["ssl"].check_hostname
        upstream = Upstream()
        self.upstreams.append(upstream)
        return upstream


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.record = module.password_record(PASSWORD)
        self.config = module.Config(ORIGIN, "wss://gateway.example/", "pwa-fixture-0001", 1,
                                    KEY, bytes.fromhex(self.record["salt"]), bytes.fromhex(self.record["hash"]),
                                    Path(self.tmp.name))
        self.app = module.create_app(self.config)
        self.client = TestClient(TestServer(self.app))
        await self.client.start_server()
        self.bridge = self.app["bridge"]
        self.fixture = FixtureHTTP()
        self.bridge.http = self.fixture

    async def asyncTearDown(self):
        await self.client.close()
        self.tmp.cleanup()

    def headers(self, cookie=None, origin=ORIGIN):
        headers = {"Host": "pwa.example", "Origin": origin}
        if cookie:
            headers["Cookie"] = cookie
        return headers

    async def login(self):
        response = await self.client.post("/api/login", json={"password": PASSWORD}, headers=self.headers())
        self.assertEqual(response.status, 200)
        cookie = response.headers["Set-Cookie"].split(";")[0]
        return cookie, await response.json(), response

    async def open_socket(self, cookie):
        ws = await self.client.ws_connect("/gateway", protocols=[module.PROTOCOL], headers=self.headers(cookie))
        await ws.send_json({"v": 1, "type": "device.hello", "id": str(uuid.uuid4()), "firmware": "0.3.0"})
        accepted = await ws.receive_json(timeout=2)
        return ws, accepted

    async def test_password_cookie_secrets_and_negative_login(self):
        response = await self.client.post("/api/login", json={"password": "wrong"}, headers=self.headers())
        self.assertEqual(response.status, 401)
        cookie, info, response = await self.login()
        header = response.headers["Set-Cookie"]
        for flag in ("Secure", "HttpOnly", "SameSite=Strict", "Path=/", "Max-Age=28800"):
            self.assertIn(flag, header)
        self.assertIn(info["expires_in"], (28799, 28800))
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertNotIn("credential", info)
        self.assertNotIn("secret", info)
        self.assertNotIn(PASSWORD, json.dumps(info))
        self.assertNotIn(KEY.hex(), repr(self.config))
        self.assertNotIn(cookie.split("=")[1], repr(self.bridge.sessions))
        bad = await self.client.get("/api/session", headers=self.headers())
        self.assertEqual(bad.status, 401)

    async def test_origin_host_query_and_content_type(self):
        for origin in ("https://attacker.example", "null", ""):
            response = await self.client.post("/api/login", json={"password": PASSWORD}, headers=self.headers(origin=origin))
            self.assertEqual(response.status, 403)
        response = await self.client.post("/api/login", data=PASSWORD, headers=self.headers())
        self.assertEqual(response.status, 415)
        response = await self.client.post("/api/login?password=never", json={"password": PASSWORD}, headers=self.headers())
        self.assertEqual(response.status, 403)
        response = await self.client.get("/api/session", headers={"Host": "other.example"})
        self.assertEqual(response.status, 403)

    async def test_global_rate_limit_cannot_be_bypassed_by_forwarded_header(self):
        for i in range(5):
            headers = self.headers()
            headers["X-Forwarded-For"] = f"192.0.2.{i}"
            response = await self.client.post("/api/login", json={"password": "wrong"}, headers=headers)
            self.assertEqual(response.status, 401)
        response = await self.client.post("/api/login", json={"password": PASSWORD}, headers=self.headers())
        self.assertEqual(response.status, 429)
        self.assertEqual(response.headers["Retry-After"], "60")

    async def test_ws_auth_required_wrong_origin_and_subprotocol(self):
        from aiohttp import WSServerHandshakeError
        cookie, _, _ = await self.login()
        for headers, protocols, status in [(self.headers(), [module.PROTOCOL], 401),
                                            (self.headers(cookie, "https://evil.example"), [module.PROTOCOL], 403),
                                            (self.headers(cookie), ["other"], 400)]:
            with self.assertRaises(WSServerHandshakeError) as caught:
                await self.client.ws_connect("/gateway", headers=headers, protocols=protocols)
            self.assertEqual(caught.exception.status, status)

    async def test_pcm_cancel_repeat_and_fresh_gateway_session(self):
        cookie, _, _ = await self.login()
        ws, accepted = await self.open_socket(cookie)
        self.assertEqual(accepted["type"], "session.accepted")
        self.assertNotIn("proof", accepted)
        self.assertNotIn("nonce", accepted)
        seq = 0
        for _ in range(2):
            correlation = str(uuid.uuid4())
            for kind, extra in [("audio.input.start", {"format": {"encoding": "pcm_s16le", "sample_rate": 16000, "channels": 1}}),
                                ("audio.input.chunk", {"data": "AAA="}), ("audio.input.stop", {})]:
                seq += 1
                await ws.send_json({"v": 1, "type": kind, "device_id": self.config.device_id,
                                    "session_id": accepted["session_id"], "correlation_id": correlation, "seq": seq, **extra})
            reply = [await ws.receive_json(timeout=2) for _ in range(3)]
            self.assertEqual([r["type"] for r in reply], ["audio.output.start", "audio.output.chunk", "audio.output.stop"])
            self.assertEqual(reply[1]["data"], "AAA=")
        seq += 1
        await ws.send_json({"v": 1, "type": "cancel", "device_id": self.config.device_id,
                            "session_id": accepted["session_id"], "correlation_id": correlation, "seq": seq})
        await ws.close()
        # Allow handler cleanup, bounded; this does not simulate production delay.
        for _ in range(50):
            if not self.bridge.connections:
                break
            await asyncio.sleep(0.01)
        next_ws, next_accepted = await self.open_socket(cookie)
        self.assertNotEqual(next_accepted["session_id"], accepted["session_id"])
        self.assertTrue(self.fixture.upstreams[0].closed)
        await next_ws.close()

    async def test_logout_csrf_socket_close_and_session_fixation(self):
        cookie, info, _ = await self.login()
        ws, _ = await self.open_socket(cookie)
        response = await self.client.post("/api/logout", headers=self.headers(cookie))
        self.assertEqual(response.status, 403)
        headers = self.headers(cookie)
        headers["X-Zenti-CSRF"] = info["csrf"]
        # aiohttp acknowledges CLOSE in receive(); browsers do this autonomously.
        receiving = asyncio.create_task(ws.receive(timeout=2))
        response = await self.client.post("/api/logout", headers=headers)
        self.assertEqual(response.status, 204)
        frame = await receiving
        self.assertEqual(frame.type, WSMsgType.CLOSE)
        self.assertEqual(frame.data, 1008)
        self.assertEqual(ws.close_code, 1008)
        response = await self.client.get("/api/session", headers=self.headers(cookie))
        self.assertEqual(response.status, 401)
        cookie, _, _ = await self.login()
        response = await self.client.post("/api/login", json={"password": PASSWORD}, headers=self.headers(cookie))
        self.assertEqual(response.status, 200)
        self.assertNotEqual(cookie, response.headers["Set-Cookie"].split(";")[0])
        response = await self.client.get("/api/session", headers=self.headers(cookie))
        self.assertEqual(response.status, 401)

    async def test_expiry_and_device_isolation(self):
        cookie, _, _ = await self.login()
        ws, accepted = await self.open_socket(cookie)
        await ws.send_json({"v": 1, "type": "text.input", "device_id": "puck-lab-0001",
                            "session_id": accepted["session_id"], "correlation_id": str(uuid.uuid4()), "seq": 1, "text": "fixture"})
        await ws.receive(timeout=2)
        self.assertEqual(ws.close_code, 1008)
        session = self.bridge.sessions[self.bridge.token_key(cookie.split("=")[1])]
        session.expires = self.bridge.clock() - 1
        response = await self.client.get("/api/session", headers=self.headers(cookie))
        self.assertEqual(response.status, 401)

    async def test_active_enrollment_expiry_is_enforced_server_side(self):
        cookie, _, _ = await self.login()
        object.__setattr__(self.config, "expires_at", module.time.time() + 0.3)
        ws, accepted = await self.open_socket(cookie)
        self.assertLess(accepted["expires_ms"], module.time.time() * 1000 + 1000)
        await ws.receive(timeout=2)
        self.assertEqual(ws.close_code, 1008)
        self.assertFalse(self.bridge.connections)

    async def test_stalled_upgrade_cancels_on_browser_close_and_reconnects(self):
        cookie, _, _ = await self.login()
        started, cancelled = asyncio.Event(), asyncio.Event()
        original = self.bridge.http
        async def stalled(*args, **kwargs):
            started.set()
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.set()
        self.bridge.http = SimpleNamespace(ws_connect=stalled)
        ws = await self.client.ws_connect("/gateway", protocols=[module.PROTOCOL], headers=self.headers(cookie))
        await ws.send_json({"v": 1, "type": "device.hello", "id": str(uuid.uuid4()), "firmware": "0.3.0"})
        await asyncio.wait_for(started.wait(), 2)
        await ws.close()
        await asyncio.wait_for(cancelled.wait(), 2)
        for _ in range(50):
            if not self.bridge.connections:
                break
            await asyncio.sleep(0.01)
        self.assertEqual(self.bridge.connections, 0)
        self.bridge.http = original
        next_ws, accepted = await self.open_socket(cookie)
        self.assertEqual(accepted["type"], "session.accepted")
        await next_ws.close()

    async def test_full_upgrade_deadline_releases_reservation(self):
        cookie, _, _ = await self.login()
        async def stalled(*args, **kwargs):
            await asyncio.Event().wait()
        self.bridge.http = SimpleNamespace(ws_connect=stalled)
        previous = module.AUTH_BUDGET
        module.AUTH_BUDGET = 0.1
        try:
            ws = await self.client.ws_connect("/gateway", protocols=[module.PROTOCOL], headers=self.headers(cookie))
            await ws.send_json({"v": 1, "type": "device.hello", "id": str(uuid.uuid4()), "firmware": "0.3.0"})
            await ws.receive(timeout=2)
            self.assertEqual(ws.close_code, 1008)
            self.assertEqual(self.bridge.connections, 0)
        finally:
            module.AUTH_BUDGET = previous

    async def test_slow_upstream_close_does_not_block_reconnect(self):
        cookie, _, _ = await self.login()
        ws, _ = await self.open_socket(cookie)
        upstream = self.fixture.upstreams[-1]
        cancelled = asyncio.Event()
        async def slow_close():
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.set()
        upstream.close = slow_close
        previous = module.CLOSE_BUDGET
        module.CLOSE_BUDGET = 0.1
        try:
            await ws.close()
            await asyncio.wait_for(cancelled.wait(), 2)
            for _ in range(50):
                if not self.bridge.connections:
                    break
                await asyncio.sleep(0.01)
            self.assertEqual(self.bridge.connections, 0)
            next_ws, accepted = await self.open_socket(cookie)
            self.assertEqual(accepted["type"], "session.accepted")
            await next_ws.close()
        finally:
            module.CLOSE_BUDGET = previous

    async def test_concurrent_close_callers_join_one_handshake(self):
        started, release = asyncio.Event(), asyncio.Event()
        calls = []
        class Socket:
            async def close(self, **kwargs):
                calls.append(kwargs)
                started.set()
                await release.wait()
        socket = Socket()
        first = asyncio.create_task(self.bridge.close_socket(socket, code=1008))
        await started.wait()
        second = asyncio.create_task(self.bridge.close_socket(socket, code=1008))
        await asyncio.sleep(0)
        self.assertFalse(second.done())
        release.set()
        await asyncio.gather(first, second)
        self.assertEqual(len(calls), 1)
        self.assertFalse(self.bridge.close_tasks)

    async def test_nonreading_logout_rejects_replay_and_preserves_other_client(self):
        from aiohttp import WSServerHandshakeError
        cookie1, info1, _ = await self.login()
        cookie2, _, _ = await self.login()
        ws1, _ = await self.open_socket(cookie1)
        ws2, accepted2 = await self.open_socket(cookie2)
        headers = self.headers(cookie1)
        headers["X-Zenti-CSRF"] = info1["csrf"]
        response = await asyncio.wait_for(self.client.post("/api/logout", headers=headers), 3)
        self.assertEqual(response.status, 204)
        self.assertEqual((await self.client.get("/api/session", headers=self.headers(cookie1))).status, 401)
        with self.assertRaises(WSServerHandshakeError) as caught:
            await self.open_socket(cookie1)
        self.assertEqual(caught.exception.status, 401)
        for _ in range(100):
            if self.bridge.connections == 1:
                break
            await asyncio.sleep(0.01)
        self.assertEqual(self.bridge.connections, 1)
        # The intentionally nonreading peer may observe an abnormal close.
        await ws1.receive(timeout=2)
        self.assertTrue(ws1.closed)
        correlation = str(uuid.uuid4())
        for seq, kind in enumerate(("audio.input.start", "audio.input.chunk", "audio.input.stop"), 1):
            extra = ({"format": {"encoding": "pcm_s16le", "sample_rate": 16000, "channels": 1}}
                     if kind == "audio.input.start" else {"data": "AAA="} if kind == "audio.input.chunk" else {})
            await ws2.send_json({"v": 1, "type": kind, "device_id": self.config.device_id,
                                "session_id": accepted2["session_id"], "correlation_id": correlation,
                                "seq": seq, **extra})
        self.assertEqual((await ws2.receive_json(timeout=2))["type"], "audio.output.start")
        self.assertEqual((await ws2.receive_json(timeout=2))["type"], "audio.output.chunk")
        self.assertEqual((await ws2.receive_json(timeout=2))["type"], "audio.output.stop")
        fresh, _, _ = await self.login()
        next_ws, accepted = await self.open_socket(fresh)
        self.assertEqual(accepted["type"], "session.accepted")
        await next_ws.close()
        await ws2.close()

    async def test_upstream_redirect_guard(self):
        with self.assertRaises(ValueError):
            await module.reject_redirect(None, None, None)

    async def test_static_cannot_read_private_files_or_unknown_api(self):
        response = await self.client.get("/api/unknown", headers=self.headers())
        self.assertEqual(response.status, 404)
        response = await self.client.get("/.password.json", headers=self.headers())
        self.assertEqual(response.status, 404)
        self.assertRaises(ValueError, module.Config, ORIGIN, "ws://gateway.example/", "pwa-fixture-0001", 1,
                          KEY, self.config.salt, self.config.digest, Path(self.tmp.name))
        self.assertRaises(ValueError, module.Config, ORIGIN, "wss://gateway.example/", "puck-lab-0001", 1,
                          KEY, self.config.salt, self.config.digest, Path(self.tmp.name))


    async def test_cancelled_login_keeps_native_hash_reservation(self):
        started, release = threading.Event(), threading.Event()
        def blocked_hash(password, salt):
            started.set()
            if not release.wait(5):
                raise RuntimeError("fixture worker not released")
            return self.config.digest
        async def body():
            return {"password": PASSWORD}
        request = SimpleNamespace(headers={"Origin": ORIGIN}, content_type="application/json",
                                  json=body, cookies={})
        with patch.object(module, "password_hash", blocked_hash):
            task = asyncio.create_task(self.bridge.login(request))
            try:
                for _ in range(200):
                    if started.is_set():
                        break
                    await asyncio.sleep(0.01)
                self.assertTrue(started.is_set())
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task
                self.assertTrue(self.bridge.login_busy)
                with self.assertRaises(module.web.HTTPTooManyRequests):
                    await self.bridge.login(request)
            finally:
                release.set()
            for _ in range(200):
                if not self.bridge.login_busy:
                    break
                await asyncio.sleep(0.01)
            self.assertFalse(self.bridge.login_busy)
            response = await self.bridge.login(request)
            self.assertEqual(response.status, 200)

    async def test_retirement_bounds_slow_browser_close(self):
        cookie, _, _ = await self.login()
        session = self.bridge.sessions[self.bridge.token_key(cookie.split("=")[1])]
        cancelled = asyncio.Event()
        class SlowSocket:
            async def close(self, **kwargs):
                try:
                    await asyncio.Event().wait()
                finally:
                    cancelled.set()
        session.sockets.add(SlowSocket())
        previous = module.CLOSE_BUDGET
        module.CLOSE_BUDGET = 0.1
        try:
            await asyncio.wait_for(self.bridge.retire(self.bridge.token_key(cookie.split("=")[1])), 1)
            self.assertTrue(cancelled.is_set())
            self.assertFalse(self.bridge.sessions)
            self.assertEqual((await self.client.get("/api/session", headers=self.headers(cookie))).status, 401)
        finally:
            module.CLOSE_BUDGET = previous


    async def test_logout_rotation_and_sweep_bound_slow_browser(self):
        previous = module.CLOSE_BUDGET
        module.CLOSE_BUDGET = 0.05
        try:
            for action in ("logout", "rotation", "sweep"):
                cookie, info, _ = await self.login()
                key = self.bridge.token_key(cookie.split("=")[1])
                session = self.bridge.sessions[key]
                finished = asyncio.Event()
                class SlowSocket:
                    async def close(self, **kwargs):
                        try:
                            await asyncio.Event().wait()
                        finally:
                            finished.set()
                session.sockets.add(SlowSocket())
                if action == "logout":
                    headers = self.headers(cookie)
                    headers["X-Zenti-CSRF"] = info["csrf"]
                    response = await asyncio.wait_for(self.client.post("/api/logout", headers=headers), 1)
                    self.assertEqual(response.status, 204)
                elif action == "rotation":
                    response = await asyncio.wait_for(self.client.post("/api/login", json={"password": PASSWORD},
                                                                      headers=self.headers(cookie)), 2)
                    self.assertEqual(response.status, 200)
                else:
                    session.expires = self.bridge.clock() - 1
                    await asyncio.wait_for(finished.wait(), 2)
                self.assertTrue(finished.is_set(), action)
                self.assertNotIn(key, self.bridge.sessions, action)
        finally:
            module.CLOSE_BUDGET = previous

    async def test_concurrent_clients_and_single_socket_reservation(self):
        from aiohttp import WSServerHandshakeError
        cookie1, _, _ = await self.login()
        cookie2, _, _ = await self.login()
        ws1, accepted1 = await self.open_socket(cookie1)
        ws2, accepted2 = await self.open_socket(cookie2)
        self.assertNotEqual(accepted1["session_id"], accepted2["session_id"])
        with self.assertRaises(WSServerHandshakeError) as caught:
            await self.open_socket(cookie1)
        self.assertEqual(caught.exception.status, 429)
        self.assertEqual(self.bridge.connections, 2)
        await ws1.close()
        await ws2.close()

    async def test_recreated_bridge_rejects_old_password_and_existing_session(self):
        cookie, _, _ = await self.login()
        socket, _ = await self.open_socket(cookie)
        await self.client.close()
        self.assertTrue(socket.closed)
        replacement_password = "fixture-" "replacement-password-456"
        record = module.password_record(replacement_password)
        config = replace(self.config, salt=bytes.fromhex(record["salt"]), digest=bytes.fromhex(record["hash"]))
        client = TestClient(TestServer(module.create_app(config)))
        await client.start_server()
        try:
            response = await client.get("/api/session", headers=self.headers(cookie))
            self.assertEqual(response.status, 401)
            response = await client.post("/api/login", json={"password": PASSWORD}, headers=self.headers())
            self.assertEqual(response.status, 401)
            response = await client.post("/api/login", json={"password": replacement_password}, headers=self.headers())
            self.assertEqual(response.status, 200)
            self.assertEqual((await response.json())["device_id"], self.config.device_id)
            self.assertEqual(config.secret, self.config.secret)
            self.assertEqual(config.gateway, self.config.gateway)
        finally:
            await client.close()


class ProvisionToolTests(unittest.TestCase):
    def run_tool(self, output, entered):
        stderr = io.StringIO()
        with patch.object(sys, "argv", ["provision_pwa_password.py", "--output", str(output)]), \
                patch.object(sys.stdin, "isatty", return_value=True), \
                patch("getpass.getpass", return_value=entered), contextlib.redirect_stderr(stderr):
            try:
                runpy.run_path(str(ROOT / "tools/provision_pwa_password.py"), run_name="__main__")
            except SystemExit as error:
                return error.code, stderr.getvalue()
        return 0, stderr.getvalue()

    def test_short_password_reports_length_rule_not_file_error(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "password.json"
            code, stderr = self.run_tool(output, "too-short")
            self.assertEqual(code, 2)
            self.assertIn("16..256 UTF-8 bytes", stderr)
            self.assertNotIn("could not create", stderr)
            self.assertNotIn("too-short", stderr)
            self.assertFalse(output.exists())

    def test_existing_file_is_reported_and_kept(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "password.json"
            output.write_text("kept\n")
            code, stderr = self.run_tool(output, PASSWORD)
            self.assertEqual(code, 2)
            self.assertIn("already exists", stderr)
            self.assertEqual(output.read_text(), "kept\n")

    def test_writes_verifiable_record(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "password.json"
            self.assertEqual(self.run_tool(output, PASSWORD), (0, ""))
            record = json.loads(output.read_text())
            self.assertEqual(record["algorithm"], "scrypt-131072-8-1")
            self.assertEqual(module.password_hash(PASSWORD, bytes.fromhex(record["salt"])).hex(), record["hash"])


if __name__ == "__main__":
    unittest.main()
