"""Bounded post-auth voice turns for the Zentari Voice Gateway."""
from __future__ import annotations

import asyncio
import base64
import binascii
import json
import logging
import re
import time
import uuid
from dataclasses import dataclass
from typing import Awaitable, Callable, Protocol

MAX_FRAME = 4096
CHUNK = 1024
INPUT_LIMIT = 480000
OUTPUT_LIMIT = 1920000  # 60 s of 16 kHz PCM16; puck firmware and PWA enforce the same bound.
TEXT_LIMIT = 4096
TURN_SECONDS = 150  # Room for a slow agent answer plus 60 s of speech.
STAGE_SECONDS = 30
PCM = {"encoding": "pcm_s16le", "sample_rate": 16000, "channels": 1}
# Speech starts after the first sentence is synthesized, not the whole reply.
SENTENCE = re.compile(r"(?<=[.!?\u2026])\s+|\s*\n+\s*")
FIRST_MIN, GROUP_MAX = 24, 250
# Chunks sent unpaced at the start of a reply: the puck's ~90 ms DMA ring would otherwise
# run nearly empty and underrun when synthesis of the next sentence delays a chunk.
# 4 x 32 ms stays within the protocol's eight queued output messages.
LEAD_CHUNKS = 4
B64_RE = re.compile(r"^[A-Za-z0-9+/]*={0,2}$")



def _segments(text: str) -> list[str]:
    """Split a reply into TTS segments: a short first sentence, then groups of up to GROUP_MAX chars."""
    parts = [part.strip() for part in SENTENCE.split(text) if part and part.strip()]
    segments: list[str] = []
    for part in parts:
        if segments and (len(segments[0]) < FIRST_MIN if len(segments) == 1 else
                         len(segments[-1]) + 1 + len(part) <= GROUP_MAX):
            segments[-1] += " " + part
        else:
            segments.append(part)
    return segments or [text]

class TurnError(Exception):
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


class SttAdapter(Protocol):
    async def transcribe(self, pcm: bytes) -> str: ...


class HermesAdapter(Protocol):
    async def answer(self, transcript: str) -> str: ...


class TtsAdapter(Protocol):
    async def synthesize(self, answer: str) -> bytes: ...


class UnavailableHermes:
    """No supported, enforced read-only Hermes interface is configured."""
    ready = False

    async def answer(self, transcript: str) -> str:
        raise TurnError("hermes_readonly_unavailable")


@dataclass
class Turn:
    correlation_id: str
    started: float
    pcm: bytearray
    chunks: int = 0
    state: str = "listening"


class VoiceTurnSession:
    def __init__(self, device_id: str, session_id: str, worker: asyncio.Lock,
                 send: Callable[[dict], Awaitable[None]], stt: SttAdapter,
                 hermes: HermesAdapter, tts: TtsAdapter, hello_id: str | None = None, conversations=None):
        self.conversations = conversations
        self.conversation = conversations.open(device_id) if conversations else None
        self.device_id = device_id
        self.session_id = session_id
        self.worker = worker
        self.send = send
        self.stt = stt
        self.hermes = hermes
        self.tts = tts
        self.input_seq = 0
        self.output_seq = 0
        self.turn: Turn | None = None
        self.used_correlations = {hello_id} if hello_id else set()
        self.task: asyncio.Task | None = None
        self.deadline: asyncio.Task | None = None
        self.closed = False
        self.owns_worker = False
        self.send_lock = asyncio.Lock()
        self.cancel_lock = asyncio.Lock()

    async def emit(self, event_type: str, correlation_id: str, **fields: object) -> None:
        if self.closed:
            return
        async with self.send_lock:
            self.output_seq += 1
            frame = {"v": 1, "type": event_type, "device_id": self.device_id,
                     "session_id": self.session_id, "correlation_id": correlation_id,
                     "seq": self.output_seq, **fields}
            if len(json.dumps(frame, separators=(",", ":")).encode()) > MAX_FRAME:
                raise TurnError("frame_too_large")
            await asyncio.wait_for(self.send(frame), 5)

    async def error(self, code: str, correlation_id: str | None = None) -> None:
        await self.emit("error", correlation_id or self.session_id, code=code)

    async def receive(self, frame: dict) -> None:
        if self.closed:
            return
        correlation_id = frame.get("correlation_id")
        try:
            self._validate_envelope(frame)
            event_type = frame["type"]
            if event_type in ("conversation.open", "conversation.reset"):
                if self.turn or not self.conversations:
                    raise TurnError("busy")
                extra = ("conversation_id",) if "conversation_id" in frame else ()
                if set(frame) != self._keys(*extra):
                    raise TurnError("event_invalid")
                if "conversation_id" in frame and (not isinstance(frame["conversation_id"], str) or not _uuid(frame["conversation_id"])):
                    raise TurnError("conversation_unavailable")
                try:
                    self.conversation = self.conversations.open(self.device_id,
                        frame.get("conversation_id"), event_type == "conversation.reset")
                except ValueError:
                    raise TurnError("conversation_unavailable")
                await self.emit("conversation.ready", correlation_id,
                    conversation_id=self.conversation.id,
                    principal=self.conversation.owner[1], capabilities=["text", "context", "reset"])
            elif event_type == "text.input":
                if self.turn is not None or self.worker.locked():
                    raise TurnError("busy")
                if set(frame) != self._keys("text"):
                    raise TurnError("event_invalid")
                _text(frame["text"])
                if len(frame["text"].encode()) > 3000 or not frame["text"].strip():
                    raise TurnError("text_invalid")
                if correlation_id in self.used_correlations or len(self.used_correlations) >= 256:
                    raise TurnError("turn_stale")
                await self.worker.acquire()
                self.owns_worker = True
                self.used_correlations.add(correlation_id)
                self.turn = Turn(correlation_id, time.monotonic(), bytearray(), state="processing")
                self.deadline = asyncio.create_task(self._expire(self.turn))
                self.task = asyncio.create_task(self._process(self.turn, frame["text"]))
            elif event_type == "audio.input.start":
                if self.turn is not None or self.worker.locked():
                    raise TurnError("busy")
                if frame.get("format") != PCM or set(frame) != self._keys("format"):
                    raise TurnError("format_invalid")
                if correlation_id in self.used_correlations or len(self.used_correlations) >= 256:
                    raise TurnError("turn_stale")
                await self.worker.acquire()
                self.owns_worker = True
                self.used_correlations.add(correlation_id)
                self.turn = Turn(correlation_id, time.monotonic(), bytearray())
                self.deadline = asyncio.create_task(self._expire(self.turn))
            elif event_type == "audio.input.chunk":
                turn = self._current(correlation_id, "listening")
                if set(frame) != self._keys("data"):
                    raise TurnError("event_invalid")
                data = frame["data"]
                if not isinstance(data, str) or len(data) > 1368 or not B64_RE.fullmatch(data):
                    raise TurnError("audio_invalid")
                try:
                    decoded = base64.b64decode(data, validate=True)
                except (binascii.Error, ValueError) as exc:
                    raise TurnError("audio_invalid") from exc
                if not decoded or len(decoded) > CHUNK or len(decoded) % 2:
                    raise TurnError("audio_invalid")
                if len(turn.pcm) + len(decoded) > INPUT_LIMIT or time.monotonic() - turn.started > TURN_SECONDS:
                    raise TurnError("input_limit")
                turn.pcm.extend(decoded)
                turn.chunks += 1
            elif event_type == "audio.input.stop":
                turn = self._current(correlation_id, "listening")
                if set(frame) != self._keys():
                    raise TurnError("event_invalid")
                if not turn.pcm:
                    raise TurnError("input_empty")
                turn.state = "processing"
                self.task = asyncio.create_task(self._process(turn))
            elif event_type == "cancel":
                self._current(correlation_id)
                if set(frame) != self._keys():
                    raise TurnError("event_invalid")
                await self.cancel()
            else:
                raise TurnError("event_invalid")
        except TurnError as exc:
            try:
                await self.error(exc.code, correlation_id if isinstance(correlation_id, str) else None)
            finally:
                if exc.code != "busy":
                    await self.cancel()

    async def _expire(self, turn: Turn) -> None:
        # Enforce the whole-turn budget even while a client sends no frames.
        await asyncio.sleep(TURN_SECONDS)
        if self.turn is turn:
            await self.cancel()
            try:
                await self.error("turn_timeout", turn.correlation_id)
            except Exception:
                # An unavailable transport cannot retain the worker or emit late audio.
                self.closed = True

    @staticmethod
    def _keys(*extra: str) -> set[str]:
        return {"v", "type", "device_id", "session_id", "correlation_id", "seq", *extra}

    def _validate_envelope(self, frame: dict) -> None:
        if (not isinstance(frame, dict) or frame.get("v") != 1 or
                frame.get("device_id") != self.device_id or
                frame.get("session_id") != self.session_id or
                not isinstance(frame.get("correlation_id"), str) or
                not _uuid(frame["correlation_id"])):
            raise TurnError("identity_invalid")
        seq = frame.get("seq")
        if type(seq) is not int or seq != self.input_seq + 1:
            raise TurnError("sequence_invalid")
        self.input_seq = seq

    def _current(self, correlation_id: str, state: str | None = None) -> Turn:
        turn = self.turn
        if turn is None or turn.correlation_id != correlation_id:
            raise TurnError("turn_stale")
        if state is not None and turn.state != state:
            raise TurnError("order_invalid")
        return turn

    async def _stage(self, operation: Awaitable, code: str):
        started = time.monotonic()
        correlation = self.turn.correlation_id if self.turn else None
        status = "FAIL"
        try:
            timeout = getattr(self.hermes,'stage_timeout',STAGE_SECONDS) if code == 'hermes' else STAGE_SECONDS
            result = await asyncio.wait_for(operation, min(90,max(STAGE_SECONDS,timeout)))
            status = "PASS"
            return result
        except asyncio.CancelledError:
            status = "CANCELLED"
            raise
        except TurnError:
            raise
        except asyncio.TimeoutError as exc:
            raise TurnError(code + "_timeout") from exc
        except Exception as exc:
            raise TurnError(code + "_failed") from exc
        finally:
            logging.getLogger("voice_turn").info(json.dumps({
                "event": "voice_stage", "device_id": self.device_id,
                "session_id": self.session_id,
                "correlation_id": correlation,
                "stage": code, "status": status,
                "elapsed_ms": int((time.monotonic() - started) * 1000)}, separators=(",", ":")))

    async def _speak(self, turn: Turn, segments: list[str]) -> None:
        """Stream each segment while the next one is synthesized."""
        def synth(segment):
            return asyncio.ensure_future(self._stage(self.tts.synthesize(segment), "tts"))
        pending = synth(segments[0])
        total = sent = 0
        try:
            for index in range(len(segments)):
                try:
                    audio = await pending
                    if not isinstance(audio, bytes) or not audio or len(audio) % 2 or total + len(audio) > OUTPUT_LIMIT:
                        raise TurnError("output_invalid")
                except TurnError:
                    if not index:
                        raise
                    # Speech already started and the full text is shown: end what was spoken cleanly.
                    break
                total += len(audio)
                pending = synth(segments[index + 1]) if index + 1 < len(segments) else None
                if self.turn is not turn:
                    return
                if not index:
                    await self.emit("audio.output.start", turn.correlation_id, format=PCM)
                for offset in range(0, len(audio), CHUNK):
                    chunk = audio[offset:offset + CHUNK]
                    await self.emit("audio.output.chunk", turn.correlation_id,
                                    data=base64.b64encode(chunk).decode("ascii"))
                    sent += 1
                    # Match 16 kHz mono playback after a small lead; the puck never buffers a full reply.
                    if sent > LEAD_CHUNKS:
                        await asyncio.sleep(len(chunk) / 32000)
            await self.emit("audio.output.stop", turn.correlation_id)
        finally:
            if pending is not None and not pending.done():
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)

    async def _process(self, turn: Turn, text: str | None = None) -> None:
        try:
            async with asyncio.timeout(max(0.001, TURN_SECONDS - (time.monotonic() - turn.started))):
                pcm = bytes(turn.pcm)
                turn.pcm.clear()
                transcript = text if text is not None else await self._stage(self.stt.transcribe(pcm), "stt")
                if text is None and isinstance(transcript, str) and not transcript.strip():
                    raise TurnError("no_speech")
                _text(transcript)
                if self.turn is not turn:
                    return
                await self.emit("transcript.final", turn.correlation_id, text=transcript)
                await self.emit("assistant.thinking", turn.correlation_id)
                operation = (self.conversations.answer(self.device_id, self.conversation, transcript, self.hermes,
                             dict(device_id=self.device_id, session_id=self.session_id, correlation_id=turn.correlation_id))
                             if self.conversations else self.hermes.answer(transcript))
                answer = await self._stage(operation, "hermes")
                _text(answer)
                if self.turn is not turn:
                    return
                await self.emit("assistant.text", turn.correlation_id, text=answer)
                await self._speak(turn, _segments(answer))
        except asyncio.CancelledError:
            raise
        except asyncio.TimeoutError:
            await self._processing_error("turn_timeout", turn.correlation_id)
        except TurnError as exc:
            await self._processing_error(exc.code, turn.correlation_id)
        except Exception:
            # A failed/slow transport must not leave an unobserved background
            # exception or continue emitting audio into this session.
            self.closed = True
        finally:
            turn.pcm.clear()
            if self.owns_worker:
                self.worker.release()
                self.owns_worker = False
            if self.turn is turn:
                self.turn = None
                if self.deadline is not None:
                    self.deadline.cancel()
                    self.deadline = None

    async def _processing_error(self, code: str, correlation_id: str) -> None:
        try:
            await self.error(code, correlation_id)
        except Exception:
            self.closed = True

    async def cancel(self) -> None:
        # Watchdog, client cancel and disconnect may race. Keep capacity owned
        # until the first cancellation has finished adapter cleanup.
        async with self.cancel_lock:
            await self._cancel()

    async def _cancel(self) -> None:
        turn, task, deadline = self.turn, self.task, self.deadline
        self.turn = None
        self.task = None
        self.deadline = None
        if deadline is not None and deadline is not asyncio.current_task():
            deadline.cancel()
            await asyncio.gather(deadline, return_exceptions=True)
        if turn is not None:
            turn.pcm.clear()
        if task is not None and not task.done():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        if self.owns_worker:
            self.worker.release()
            self.owns_worker = False

    async def close(self) -> None:
        self.closed = True
        await self.cancel()


def _uuid(value: str) -> bool:
    try:
        return str(uuid.UUID(value)) == value.lower()
    except (ValueError, AttributeError):
        return False


def _text(value: str) -> None:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > TEXT_LIMIT:
        raise TurnError("text_invalid")
