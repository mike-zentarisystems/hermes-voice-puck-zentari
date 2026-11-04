#!/usr/bin/env python3
"""Personal PWA session bridge. No provider calls; gateway credential stays here."""
from __future__ import annotations

import argparse
import asyncio
import base64
from collections import deque
from dataclasses import dataclass, field
import hashlib
import hmac
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import stat
import time
from urllib.parse import urlsplit
import uuid

from aiohttp import ClientSession, ClientTimeout, TraceConfig, WSMsgType, web

# Keep direct-script, container and importlib test entrypoints equivalent.
_password_spec = importlib.util.spec_from_file_location("pwa_passwords", Path(__file__).with_name("passwords.py"))
_passwords = importlib.util.module_from_spec(_password_spec)
_password_spec.loader.exec_module(_passwords)
password_hash = _passwords.password_hash
password_record = _passwords.password_record

PROTOCOL = "zentari.voice.v1"
COOKIE = "__Host-zenti-session"
TTL = 8 * 3600  # Fixed sign-in lifetime; activity does not extend it.
MAX_SESSIONS = 8
MAX_FRAME = 4096
AUTH_BUDGET = 8
CLOSE_BUDGET = 1
ALLOWED = {"conversation.open", "conversation.reset", "text.input",
           "audio.input.start", "audio.input.chunk", "audio.input.stop", "cancel"}
DEVICE = re.compile(r"^[a-z0-9-]{8,64}$")
TOKEN = re.compile(r"^[A-Za-z0-9_-]{43}$")


def private_json(path: Path) -> dict:
    # Production is Linux. Refuse symlinks and group/world access before reading.
    with path.open("rb") as stream:
        mode = os.fstat(stream.fileno()).st_mode
        if path.is_symlink() or not stat.S_ISREG(mode) or mode & 0o077:
            raise ValueError("private file permissions required")
        raw = stream.read(4097)
    if len(raw) > 4096:
        raise ValueError("private file too large")
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("private object required")
    return value


@dataclass(frozen=True)
class Config:
    origin: str
    gateway: str
    device_id: str
    generation: int
    secret: bytes = field(repr=False)
    salt: bytes = field(repr=False)
    digest: bytes = field(repr=False)
    client_root: Path
    ca_file: str | None = None
    expires_at: int | None = None

    def __post_init__(self):
        origin = urlsplit(self.origin)
        endpoint = urlsplit(self.gateway)
        if (origin.scheme != "https" or not origin.hostname or origin.path or
                origin.query or origin.fragment or origin.username or origin.password):
            raise ValueError("exact HTTPS origin required")
        if (endpoint.scheme != "wss" or not endpoint.hostname or endpoint.username or
                endpoint.password or endpoint.fragment or endpoint.query):
            raise ValueError("fixed WSS gateway required")
        if not DEVICE.fullmatch(self.device_id) or self.device_id.startswith("puck-"):
            raise ValueError("dedicated browser identity required")
        if type(self.generation) is not int or self.generation < 1:
            raise ValueError("invalid generation")
        if len(self.secret) < 32 or len(self.salt) != 32 or len(self.digest) != 32:
            raise ValueError("invalid key/hash")
        if self.expires_at is not None and (type(self.expires_at) is not int or self.expires_at <= time.time()):
            raise ValueError("enrollment expired")

    @classmethod
    def load(cls, origin, enrollment, password, client_root, ca_file=None):
        credential, record = private_json(enrollment), private_json(password)
        if (credential.get("v") != 1 or credential.get("type") != "device.credential" or
                not {"session:connect", "audio:stream", "events:receive"}.issubset(credential.get("scope", [])) or
                "approval:respond" in credential.get("scope", []) or
                record.get("v") != 1 or record.get("algorithm") != "scrypt-131072-8-1"):
            raise ValueError("invalid private configuration")
        key = credential["credential"]
        if not isinstance(key, str) or not key.startswith("base64:"):
            raise ValueError("invalid credential encoding")
        return cls(origin, credential["gateway_origin"], credential["device_id"],
                   credential["generation"], base64.b64decode(key[7:], validate=True),
                   bytes.fromhex(record["salt"]), bytes.fromhex(record["hash"]),
                   client_root, ca_file, credential.get("expires_at"))


@dataclass
class Session:
    expires: float
    csrf: str = field(repr=False)
    sockets: set = field(default_factory=set, repr=False)


class Bridge:
    def __init__(self, config: Config, clock=time.monotonic):
        self.config, self.clock = config, clock
        self.sessions = {}
        self.attempts = deque()
        self.login_busy = False
        self.hash_task = None
        self.connections = 0
        self.close_tasks = {}
        self.http = None

    @staticmethod
    def token_key(token):
        return hashlib.sha256(token.encode("ascii")).digest()

    def get_session(self, request):
        token = request.cookies.get(COOKIE, "")
        if not TOKEN.fullmatch(token):
            return None
        key = self.token_key(token)
        session = self.sessions.get(key)
        if session and session.expires > self.clock():
            return session
        return None

    async def retire(self, key):
        session = self.sessions.pop(key, None)
        if session:
            await asyncio.gather(*(self.close_socket(ws, code=1008, message=b"session ended")
                                   for ws in tuple(session.sockets)), return_exceptions=True)

    async def sweep(self):
        while True:
            await asyncio.sleep(1)
            for key, session in list(self.sessions.items()):
                if session.expires <= self.clock():
                    await self.retire(key)

    def check_origin(self, request):
        if request.headers.get("Origin") != self.config.origin:
            raise web.HTTPForbidden()

    def session_info(self, session):
        return {"device_id": self.config.device_id, "csrf": session.csrf,
                "gateway_path": "/gateway", "expires_in": max(0, int(session.expires - self.clock()))}

    async def login(self, request):
        self.check_origin(request)
        if request.content_type != "application/json":
            raise web.HTTPUnsupportedMediaType()
        now = self.clock()
        while self.attempts and self.attempts[0] <= now - 60:
            self.attempts.popleft()
        # Global limit is intentional for a personal app behind one reverse proxy.
        # Never trust X-Forwarded-For as a way around it.
        if self.login_busy or len(self.attempts) >= 5:
            raise web.HTTPTooManyRequests(headers={"Retry-After": "60"})
        self.attempts.append(now)
        self.login_busy = True
        hash_task = None
        try:
            async with asyncio.timeout(5):
                value = await request.json()
            password = value.get("password") if isinstance(value, dict) else None
            if not isinstance(password, str) or not 1 <= len(password.encode("utf-8")) <= 256:
                raise web.HTTPUnauthorized()
            # Cancelling the HTTP handler cannot cancel the native scrypt worker.
            # Keep its reservation until the worker actually finishes.
            hash_task = asyncio.create_task(asyncio.to_thread(password_hash, password, self.config.salt))
            self.hash_task = hash_task
            digest = await asyncio.shield(hash_task)
            if not hmac.compare_digest(digest, self.config.digest):
                raise web.HTTPUnauthorized()
            for key, session in list(self.sessions.items()):
                if session.expires <= self.clock():
                    await self.retire(key)
            old = request.cookies.get(COOKIE, "")
            if TOKEN.fullmatch(old):
                await self.retire(self.token_key(old))
            if len(self.sessions) >= MAX_SESSIONS:
                raise web.HTTPTooManyRequests()
            token = secrets.token_urlsafe(32)
            session = Session(self.clock() + TTL, secrets.token_urlsafe(32))
            self.sessions[self.token_key(token)] = session
            response = web.json_response(self.session_info(session))
            response.set_cookie(COOKIE, token, max_age=TTL, secure=True, httponly=True,
                                samesite="Strict", path="/")
            return response
        except (ValueError, TimeoutError):
            raise web.HTTPBadRequest() from None
        finally:
            if hash_task is not None and not hash_task.done():
                hash_task.add_done_callback(self.hash_finished)
            else:
                self.hash_task = None
                self.login_busy = False

    def hash_finished(self, task):
        # Retrieve failures from a worker whose requesting handler was cancelled.
        if not task.cancelled():
            task.exception()
        self.hash_task = None
        self.login_busy = False

    async def current(self, request):
        session = self.get_session(request)
        if not session:
            raise web.HTTPUnauthorized()
        return web.json_response(self.session_info(session))

    async def logout(self, request):
        self.check_origin(request)
        session = self.get_session(request)
        if not session:
            raise web.HTTPUnauthorized()
        csrf = request.headers.get("X-Zenti-CSRF", "")
        if not TOKEN.fullmatch(csrf) or not hmac.compare_digest(csrf, session.csrf):
            raise web.HTTPForbidden()
        await self.retire(self.token_key(request.cookies[COOKIE]))
        response = web.Response(status=204)
        response.del_cookie(COOKIE, path="/", secure=True, httponly=True, samesite="Strict")
        return response

    async def receive(self, ws):
        frame = await ws.receive(timeout=10)
        if frame.type != WSMsgType.TEXT or len(frame.data.encode("utf-8")) > MAX_FRAME:
            raise ValueError("frame invalid")
        value = json.loads(frame.data)
        if not isinstance(value, dict):
            raise ValueError("object required")
        return value

    async def authenticate(self, upstream, hello_id):
        credential = self.config
        await upstream.send_json({"v": 1, "type": "device.hello", "id": hello_id,
                                  "device_id": credential.device_id,
                                  "board_id": "pwa-browser-v1", "firmware": "0.3.0"})
        challenge = await self.receive(upstream)
        challenge_id, nonce = challenge.get("challenge_id"), challenge.get("nonce")
        expiry = challenge.get("expires_ms")
        if (challenge.get("v") != 1 or challenge.get("type") != "auth.challenge" or
                challenge.get("hello_id") != hello_id or not isinstance(challenge_id, str) or
                str(uuid.UUID(challenge_id)) != challenge_id or not isinstance(nonce, str) or
                not re.fullmatch(r"[A-Za-z0-9_-]{43,64}", nonce) or type(expiry) is not int or
                expiry <= time.time() * 1000):
            raise ValueError("challenge invalid")
        body = f"zv1\n{challenge_id}\n{nonce}\n{credential.device_id}\n{credential.generation}\n{hello_id}".encode()
        proof = base64.urlsafe_b64encode(hmac.new(credential.secret, body, hashlib.sha256).digest()).rstrip(b"=").decode()
        await upstream.send_json({"v": 1, "type": "auth.prove", "challenge_id": challenge_id,
                                  "hello_id": hello_id, "device_id": credential.device_id,
                                  "generation": credential.generation, "proof": proof})
        accepted = await self.receive(upstream)
        if (accepted.get("v") != 1 or accepted.get("type") != "session.accepted" or
                accepted.get("device_id") != credential.device_id or accepted.get("correlation_id") != hello_id or
                not isinstance(accepted.get("session_id"), str) or
                str(uuid.UUID(accepted["session_id"])) != accepted["session_id"] or
                type(accepted.get("expires_ms")) is not int or accepted["expires_ms"] <= time.time() * 1000):
            raise ValueError("gateway auth rejected")
        return accepted

    async def pipe(self, source, target, session, accepted, deadline, *, browser):
        async for frame in source:
            if min(session.expires, deadline) <= self.clock():
                return
            if frame.type != WSMsgType.TEXT or len(frame.data.encode("utf-8")) > MAX_FRAME:
                return
            value = json.loads(frame.data)
            if not isinstance(value, dict):
                return
            if not browser and value.get("type") not in {
                    "conversation.ready", "transcript.final", "assistant.thinking", "assistant.text",
                    "audio.output.start", "audio.output.chunk", "audio.output.stop", "error"}:
                return
            if browser and (value.get("type") not in ALLOWED or value.get("v") != 1 or
                            value.get("device_id") != self.config.device_id or
                            value.get("session_id") != accepted["session_id"]):
                return
            # Sequential bounded writes: never queue an entire voice turn.
            async with asyncio.timeout(5):
                await target.send_str(frame.data)

    async def close_socket(self, socket, **kwargs):
        # Retirement and the socket handler must join the same handshake.
        # Returning the HTTP upgrade handler early can close its transport.
        task = self.close_tasks.get(socket)
        if task is None:
            task = asyncio.create_task(self.finish_close(socket, kwargs))
            self.close_tasks[socket] = task
            task.add_done_callback(lambda done: self.close_tasks.pop(socket, None))
        await asyncio.shield(task)

    async def finish_close(self, socket, kwargs):
        try:
            async with asyncio.timeout(CLOSE_BUDGET):
                await socket.close(**kwargs)
        except Exception:
            pass

    async def open_upstream(self, hello_id):
        upstream = None
        try:
            # Includes TLS, HTTP upgrade and both application auth messages.
            async with asyncio.timeout(AUTH_BUDGET):
                tls = ssl.create_default_context(cafile=self.config.ca_file)
                upstream = await self.http.ws_connect(
                    self.config.gateway, protocols=[PROTOCOL], ssl=tls,
                    max_msg_size=MAX_FRAME, heartbeat=20, compress=0)
                if upstream.protocol != PROTOCOL:
                    raise ValueError("subprotocol invalid")
                accepted = await self.authenticate(upstream, hello_id)
                return upstream, accepted
        except BaseException:
            if upstream:
                await self.close_socket(upstream)
            raise

    async def socket(self, request):
        self.check_origin(request)
        session = self.get_session(request)
        if not session:
            raise web.HTTPUnauthorized()
        if self.config.expires_at is not None and self.config.expires_at <= time.time():
            raise web.HTTPServiceUnavailable()
        if session.sockets or self.connections >= MAX_SESSIONS:
            raise web.HTTPTooManyRequests()
        protocols = [item.strip() for item in request.headers.get("Sec-WebSocket-Protocol", "").split(",")]
        if protocols != [PROTOCOL]:
            raise web.HTTPBadRequest()
        ws = web.WebSocketResponse(protocols=[PROTOCOL], max_msg_size=MAX_FRAME, heartbeat=20, compress=False)
        self.connections += 1
        session.sockets.add(ws)
        upstream, auth_task, watch_task = None, None, None
        try:
            await ws.prepare(request)
            hello = await self.receive(ws)
            hello_id = hello.get("id")
            if (set(hello) != {"v", "type", "id", "firmware"} or hello.get("v") != 1 or
                    hello.get("type") != "device.hello" or hello.get("firmware") != "0.3.0" or
                    not isinstance(hello_id, str) or str(uuid.UUID(hello_id)) != hello_id):
                raise ValueError("hello invalid")
            # A close or any extra pre-auth frame cancels the pending upgrade.
            auth_task = asyncio.create_task(self.open_upstream(hello_id))
            watch_task = asyncio.create_task(ws.receive())
            done, _ = await asyncio.wait([auth_task, watch_task], return_when=asyncio.FIRST_COMPLETED)
            if auth_task in done and not auth_task.cancelled() and auth_task.exception() is None:
                upstream, accepted = auth_task.result()
            if watch_task in done:
                raise ValueError("browser disconnected or sent pre-auth data")
            if upstream is None:
                await auth_task  # Propagate generic timeout/network failure.
            watch_task.cancel()
            await asyncio.gather(watch_task, return_exceptions=True)
            if self.get_session(request) is not session or ws.closed:
                return ws
            now = time.time()
            remaining = min(session.expires - self.clock(), accepted["expires_ms"] / 1000 - now)
            if self.config.expires_at is not None:
                remaining = min(remaining, self.config.expires_at - now)
            if remaining <= 0:
                return ws
            deadline = self.clock() + remaining
            accepted["expires_ms"] = int((now + remaining) * 1000)
            await ws.send_json(accepted)
            tasks = [asyncio.create_task(self.pipe(ws, upstream, session, accepted, deadline, browser=True)),
                     asyncio.create_task(self.pipe(upstream, ws, session, accepted, deadline, browser=False))]
            try:
                # Server deadline closes even an idle/custom client ignoring expiry.
                async with asyncio.timeout(remaining):
                    await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        except (ValueError, TimeoutError, OSError):
            pass
        except Exception:
            pass  # No payload/address/authentication detail in logs or responses.
        finally:
            pending = [task for task in (auth_task, watch_task) if task]
            for task in pending:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            # A concurrent disconnect/auth completion still owns an opened socket.
            if upstream is None and auth_task and not auth_task.cancelled() and auth_task.exception() is None:
                upstream, _ = auth_task.result()
            if upstream:
                await self.close_socket(upstream)
            session.sockets.discard(ws)
            self.connections -= 1
            await self.close_socket(ws, code=1008, message=b"connection ended")
        return ws

    async def static(self, request):
        relative = request.match_info["path"] or "index.html"
        # Only the client root is served, never bridge configuration/secret mounts.
        path = (self.config.client_root / relative).resolve()
        root = self.config.client_root.resolve()
        if not path.is_relative_to(root) or not path.is_file() or any(part.startswith(".") for part in Path(relative).parts):
            raise web.HTTPNotFound()
        return web.FileResponse(path)


async def reject_redirect(session, context, params):
    raise ValueError("gateway redirects forbidden")


def create_app(config, *, serve_static=True):
    bridge = Bridge(config)

    @web.middleware
    async def boundary(request, handler):
        if request.host != urlsplit(config.origin).netloc or request.query_string:
            raise web.HTTPForbidden()
        try:
            response = await handler(request)
        except web.HTTPException as error:
            response = web.Response(status=error.status, text="Request unavailable",
                                    headers={"Retry-After": error.headers["Retry-After"]} if "Retry-After" in error.headers else {})
        response.headers.update({"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff",
                                 "Referrer-Policy": "no-referrer", "Permissions-Policy": "camera=(), geolocation=(), microphone=(self)",
                                 "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' blob:; media-src 'self' blob:; connect-src 'self'; worker-src 'self'; manifest-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'"})
        return response

    async def headers(request, response):
        # WS headers must be set before prepare(), not after handler return.
        response.headers.update({"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff",
                                 "Referrer-Policy": "no-referrer", "Permissions-Policy": "camera=(), geolocation=(), microphone=(self)",
                                 "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' blob:; media-src 'self' blob:; connect-src 'self'; worker-src 'self'; manifest-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'"})

    async def lifecycle(app):
        trace = TraceConfig()
        trace.on_request_redirect.append(reject_redirect)
        async with ClientSession(timeout=ClientTimeout(total=None, sock_connect=8),
                                 trust_env=False, trace_configs=[trace]) as http:
            bridge.http = http
            sweeper = asyncio.create_task(bridge.sweep())
            yield
            sweeper.cancel()
            await asyncio.gather(sweeper, return_exceptions=True)
            # Native hashing outlives a cancelled HTTP waiter; join it on shutdown.
            if bridge.hash_task is not None:
                await asyncio.shield(bridge.hash_task)
            for key in list(bridge.sessions):
                await bridge.retire(key)

    app = web.Application(middlewares=[boundary], client_max_size=1024)
    app["bridge"] = bridge
    app.cleanup_ctx.append(lifecycle)
    app.on_response_prepare.append(headers)
    app.router.add_post("/api/login", bridge.login)
    app.router.add_get("/api/session", bridge.current)
    app.router.add_post("/api/logout", bridge.logout)
    app.router.add_get("/gateway", bridge.socket)
    # Unknown API routes fail closed instead of returning the SPA shell.
    async def unavailable(request):
        raise web.HTTPNotFound()
    app.router.add_route("*", "/api/{tail:.*}", unavailable)
    if serve_static:
        app.router.add_get("/{path:.*}", bridge.static)
    return app


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--origin", required=True)
    parser.add_argument("--enrollment-file", type=Path, required=True)
    parser.add_argument("--password-file", type=Path, required=True)
    parser.add_argument("--ca-file")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--container", action="store_true", help="bind container interface; publish host loopback only")
    args = parser.parse_args()
    try:
        config = Config.load(args.origin, args.enrollment_file, args.password_file,
                             Path(__file__).resolve().parent / "client", args.ca_file)
    except (ValueError, KeyError, TypeError, OSError):
        parser.error("invalid private bridge configuration")
    # Only an existing local HTTPS reverse proxy may expose this process.
    web.run_app(create_app(config), host="0.0.0.0" if args.container else "127.0.0.1", port=args.port, access_log=None, print=None)
