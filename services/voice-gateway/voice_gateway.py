#!/usr/bin/env python3
"""Authenticated, fail-closed WSS ingress for Zentari Voice Pucks."""
from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import signal
import ssl
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from websockets.exceptions import ConnectionClosed
from websockets.legacy.server import WebSocketServerProtocol, serve

from local_adapters import LocalStt, LocalTts
from hermes_adapter import UnixHermes
from agent_adapter import UnixAgent
from conversation import Conversations
from voice_turn import TurnError, UnavailableHermes, VoiceTurnSession

PROTOCOL = "zentari.voice.v1"
DEVICE_RE = re.compile(r"^[a-z0-9-]{8,64}$")
FIRMWARE_RE = re.compile(r"^\d+\.\d+\.\d+$")
MAX_FRAME_BYTES = 4096
AUTH_TIMEOUT_SECONDS = 10
SESSION_TTL_SECONDS = 900


def _env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    raw = os.getenv(name, str(default))
    try:
        value = int(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be an integer") from exc
    if not minimum <= value <= maximum:
        raise RuntimeError(f"{name} must be between {minimum} and {maximum}")
    return value


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _decode_secret(value: str) -> bytes:
    if not value.startswith("base64:"):
        raise ValueError("secret must use base64: encoding")
    encoded = value.removeprefix("base64:")
    try:
        secret = base64.b64decode(encoded, validate=True)
    except ValueError as exc:
        raise ValueError("invalid base64 secret") from exc
    if len(secret) < 32:
        raise ValueError("secret must contain at least 32 bytes")
    return secret


@dataclass(frozen=True)
class DeviceCredential:
    device_id: str
    generation: int
    secret: bytes
    revoked: bool


class Registry:
    def __init__(self, path: Path):
        self.path = path
        self._mtime_ns = -1
        self._devices: dict[tuple[str, int], DeviceCredential] = {}

    def reload(self, *, force: bool = False) -> None:
        stat = self.path.stat()
        if not force and stat.st_mtime_ns == self._mtime_ns:
            return
        raw = json.loads(self.path.read_text(encoding="utf-8"))
        if raw.get("version") != 1 or not isinstance(raw.get("devices"), list):
            raise ValueError("registry requires version=1 and devices[]")
        devices: dict[tuple[str, int], DeviceCredential] = {}
        for item in raw["devices"]:
            device_id = item.get("device_id")
            generation = item.get("generation")
            if not isinstance(device_id, str) or not DEVICE_RE.fullmatch(device_id):
                raise ValueError("invalid registry device_id")
            if not isinstance(generation, int) or generation < 1:
                raise ValueError("invalid registry generation")
            key = (device_id, generation)
            if key in devices:
                raise ValueError("duplicate device generation")
            devices[key] = DeviceCredential(
                device_id=device_id,
                generation=generation,
                secret=_decode_secret(item.get("secret", "")),
                revoked=item.get("status") != "active",
            )
        self._devices = devices
        self._mtime_ns = stat.st_mtime_ns

    def get(self, device_id: str, generation: int) -> DeviceCredential | None:
        self.reload()
        return self._devices.get((device_id, generation))

    @property
    def ready(self) -> bool:
        try:
            self.reload()
        except (OSError, ValueError, json.JSONDecodeError):
            return False
        return any(not item.revoked for item in self._devices.values())


def proof_message(challenge_id: str, nonce: str, device_id: str, generation: int, hello_id: str) -> bytes:
    return f"zv1\n{challenge_id}\n{nonce}\n{device_id}\n{generation}\n{hello_id}".encode("utf-8")


def expected_proof(credential: DeviceCredential, challenge_id: str, nonce: str, hello_id: str) -> str:
    body = proof_message(challenge_id, nonce, credential.device_id, credential.generation, hello_id)
    return _b64url(hmac.new(credential.secret, body, hashlib.sha256).digest())


class JsonLogger:
    def __init__(self) -> None:
        self.logger = logging.getLogger("voice_gateway")

    def emit(self, event: str, **fields: Any) -> None:
        allowed = {"device_id", "generation", "session_id", "correlation_id", "category", "peer", "latency_ms",
                   "close_origin", "close_code_received", "close_code_sent", "session_age_ms", "cleanup_ms"}
        record = {"ts_ms": int(time.time() * 1000), "event": event}
        record.update({key: value for key, value in fields.items() if key in allowed and value is not None})
        self.logger.info(json.dumps(record, separators=(",", ":"), sort_keys=True))


def _close_code(frame: Any) -> int | None:
    """Only numeric RFC close codes, never the peer-controlled reason string."""
    code = getattr(frame, "code", None)
    return code if type(code) is int and 1000 <= code <= 4999 else None


class VoiceGateway:
    def __init__(self, registry: Registry, *, auth_timeout: int = AUTH_TIMEOUT_SECONDS, session_ttl: int = SESSION_TTL_SECONDS):
        self.registry = registry
        self.auth_timeout = auth_timeout
        self.session_ttl = session_ttl
        self.log = JsonLogger()
        self.shutting_down = False
        self.conversations = Conversations()
        self.voice_worker = asyncio.Lock()
        self.stt = LocalStt()
        self.hermes = (UnixAgent(os.environ['VOICE_AGENT_SOCKET']) if os.getenv('VOICE_AGENT_SOCKET') else
                       UnixHermes() if os.getenv("VOICE_HERMES_SOCKET") else UnavailableHermes())
        self.tts = LocalTts()

    async def _receive_json(self, ws: WebSocketServerProtocol) -> dict[str, Any]:
        raw = await asyncio.wait_for(ws.recv(), timeout=self.auth_timeout)
        if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_FRAME_BYTES:
            raise ValueError("invalid frame")
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError("object required")
        return value

    @staticmethod
    async def _send(ws: WebSocketServerProtocol, event_type: str, **fields: Any) -> None:
        await ws.send(json.dumps({"v": 1, "type": event_type, **fields}, separators=(",", ":")))

    async def reject(self, ws: WebSocketServerProtocol, reason: str, *, device_id: str | None = None,
                     generation: int | None = None) -> None:
        self.log.emit("session_rejected", device_id=device_id, generation=generation, category=reason)
        try:
            await self._send(ws, "session.rejected", reason=reason)
        finally:
            await ws.close(code=1008, reason="authorization rejected")

    async def handle(self, ws: WebSocketServerProtocol) -> None:
        started = time.monotonic()
        device_id: str | None = None
        session_id: str | None = None
        turn_session: VoiceTurnSession | None = None
        session_started: float | None = None
        close_origin = "handler_exit"
        close_received: int | None = None
        close_sent: int | None = None
        try:
            if ws.subprotocol != PROTOCOL:
                await self.reject(ws, "subprotocol")
                return
            hello = await self._receive_json(ws)
            hello_id = hello.get("id")
            device_id = hello.get("device_id")
            if (hello.get("v") != 1 or hello.get("type") != "device.hello" or
                    not isinstance(hello_id, str) or not _is_uuid(hello_id) or
                    not isinstance(device_id, str) or not DEVICE_RE.fullmatch(device_id) or
                    not isinstance(hello.get("firmware"), str) or not FIRMWARE_RE.fullmatch(hello["firmware"])):
                await self.reject(ws, "hello_invalid", device_id=device_id if isinstance(device_id, str) else None)
                return
            challenge_id = str(uuid.uuid4())
            nonce = _b64url(secrets.token_bytes(32))
            issued_ms = int(time.time() * 1000)
            await self._send(ws, "auth.challenge", challenge_id=challenge_id, nonce=nonce,
                             hello_id=hello_id, issued_ms=issued_ms,
                             expires_ms=issued_ms + self.auth_timeout * 1000)
            proof = await self._receive_json(ws)
            generation = proof.get("generation")
            credential = self.registry.get(device_id, generation) if isinstance(generation, int) else None
            valid = (
                proof.get("v") == 1 and proof.get("type") == "auth.prove" and
                proof.get("challenge_id") == challenge_id and proof.get("hello_id") == hello_id and
                proof.get("device_id") == device_id and credential is not None and not credential.revoked and
                isinstance(proof.get("proof"), str) and
                hmac.compare_digest(proof["proof"], expected_proof(credential, challenge_id, nonce, hello_id))
            )
            if not valid:
                await self.reject(ws, "auth_invalid", device_id=device_id,
                                  generation=generation if isinstance(generation, int) else None)
                return
            session_id = str(uuid.uuid4())
            session_started = time.monotonic()
            correlation_id = hello_id
            await self._send(ws, "session.accepted", device_id=device_id, session_id=session_id,
                             correlation_id=correlation_id, expires_ms=int(time.time() * 1000) + self.session_ttl * 1000)
            self.log.emit("session_accepted", device_id=device_id, generation=generation, session_id=session_id,
                          correlation_id=correlation_id, latency_ms=int((time.monotonic() - started) * 1000))
            async def send_turn(frame: dict[str, Any]) -> None:
                await ws.send(json.dumps(frame, separators=(",", ":")))

            turn_session = VoiceTurnSession(device_id, session_id, self.voice_worker, send_turn,
                                            self.stt, self.hermes, self.tts, hello_id, self.conversations)
            async with asyncio.timeout(self.session_ttl):
                async for raw in ws:
                    if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_FRAME_BYTES:
                        close_origin = "frame_rejected"
                        await ws.close(code=1009, reason="frame too large")
                        return
                    # A registry edit takes effect for every event of an established session.
                    try:
                        current = self.registry.get(device_id, generation)
                    except (OSError, ValueError, json.JSONDecodeError):
                        current = None
                    if current is None or current.revoked:
                        close_origin = "credential_revoked"
                        await self.reject(ws, "credential_revoked", device_id=device_id, generation=generation)
                        return
                    event = json.loads(raw)
                    if not isinstance(event, dict):
                        await turn_session.error("event_invalid")
                        await turn_session.cancel()
                        continue
                    if event.get("type") == "session.close":
                        close_origin = "client_close"
                        await ws.close(code=1000, reason="client close")
                        return
                    await turn_session.receive(event)
            close_origin = "peer_close" if getattr(ws, "close_rcvd", None) is not None else "transport_lost"
        except TimeoutError:
            close_origin = "session_expired" if session_id else "authentication_timeout"
            await self.reject(ws, "timeout", device_id=device_id)
        except (ValueError, json.JSONDecodeError, KeyError):
            close_origin = "protocol_error"
            await self.reject(ws, "protocol", device_id=device_id)
        except ConnectionClosed as error:
            close_received = _close_code(getattr(error, "rcvd", None))
            close_sent = _close_code(getattr(error, "sent", None))
            if close_origin == "handler_exit":
                close_origin = "peer_close" if close_received is not None else "transport_lost"
        except asyncio.CancelledError:
            close_origin = "server_shutdown" if self.shutting_down else "handler_cancelled"
            raise
        finally:
            ended = time.monotonic()
            fields = {"device_id": device_id, "session_id": session_id, "close_origin": close_origin,
                      "close_code_received": close_received if close_received is not None else _close_code(getattr(ws, "close_rcvd", None)),
                      "close_code_sent": close_sent if close_sent is not None else _close_code(getattr(ws, "close_sent", None)),
                      "session_age_ms": max(0, int((ended - session_started) * 1000)) if session_started is not None else None}
            if session_id:
                # Handler observation precedes adapter cleanup; it isn't a wire-down timestamp.
                self.log.emit("session_ended", **fields)
            try:
                if turn_session is not None:
                    await turn_session.close()
            finally:
                if session_id:
                    self.log.emit("session_closed", **fields,
                                  cleanup_ms=max(0, int((time.monotonic() - ended) * 1000)))


def _is_uuid(value: str) -> bool:
    try:
        return str(uuid.UUID(value)) == value.lower()
    except ValueError:
        return False


async def health_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, gateway: VoiceGateway) -> None:
    try:
        request = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=2)
        path = request.split(b" ", 2)[1] if request.startswith(b"GET ") else b""
        live = not gateway.shutting_down
        ready = live and gateway.registry.ready
        voice_ready = (ready and gateway.stt.ready and gateway.tts.ready and
                       getattr(gateway.hermes, "ready", False))
        ok = (live if path == b"/healthz" else
              ready if path == b"/readyz" else
              voice_ready if path == b"/voice-readyz" else False)
        status = (b"200 OK" if ok else
                  b"503 Service Unavailable" if path in (b"/healthz", b"/readyz", b"/voice-readyz") else
                  b"404 Not Found")
        body = json.dumps({"status": "ok" if ok else "unavailable"}, separators=(",", ":")).encode()
        writer.write(b"HTTP/1.1 " + status + b"\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
        await writer.drain()
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, IndexError):
        pass
    finally:
        writer.close()
        await writer.wait_closed()


async def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    registry = Registry(Path(os.environ["VOICE_GATEWAY_DEVICE_REGISTRY"]))
    registry.reload(force=True)
    gateway = VoiceGateway(registry, auth_timeout=_env_int("VOICE_GATEWAY_AUTH_TIMEOUT_SECONDS", 10, 3, 60),
                           session_ttl=_env_int("VOICE_GATEWAY_SESSION_TTL_SECONDS", 900, 30, 86400))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(os.environ["VOICE_GATEWAY_TLS_CERT"], os.environ["VOICE_GATEWAY_TLS_KEY"])
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, stop.set)
    health = await asyncio.start_server(lambda r, w: health_handler(r, w, gateway), "0.0.0.0", 9184)
    try:
        try:
            await gateway.tts.start()
        except TurnError as exc:
            gateway.log.emit("tts_warmup_failed", category=exc.code)
        async with health, serve(gateway.handle, "0.0.0.0", 8443, ssl=context, subprotocols=[PROTOCOL],
                                 max_size=MAX_FRAME_BYTES, max_queue=8, ping_interval=20, ping_timeout=10,
                                 close_timeout=5):
            gateway.log.emit("gateway_ready")
            await stop.wait()
            gateway.shutting_down = True
    finally:
        await gateway.tts.close()


if __name__ == "__main__":
    asyncio.run(main())
