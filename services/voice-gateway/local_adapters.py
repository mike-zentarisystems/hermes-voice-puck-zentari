"""Local STT/TTS subprocess adapters; model assets are mounted read-only."""
from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
import os
import struct
import sys
from pathlib import Path

from voice_local import TTS_OUTPUT_LIMIT
from voice_turn import INPUT_LIMIT, STAGE_SECONDS, TEXT_LIMIT, TurnError


def _worker_environment() -> dict[str, str]:
    # Apply the existing TTS limits to STT too. Inheriting host-wide BLAS
    # defaults can exhaust the gateway's task limit beside a warm TTS worker.
    # Model runners receive no gateway/provider credentials.
    env = {key: os.environ[key] for key in ("PATH", "LANG", "LC_ALL", "SYSTEMROOT")
           if key in os.environ}
    env.update(PYTHONUNBUFFERED="1", PYTHONDONTWRITEBYTECODE="1",
               OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1")
    return env


async def _run(kind: str, payload: bytes, model: str, limit: int) -> bytes:
    if not model or not ((kind == "stt" and Path(model).is_dir()) or
                         (kind == "tts" and Path(model).is_file())):
        raise TurnError(kind + "_unavailable")
    proc = await asyncio.create_subprocess_exec(
        sys.executable, str(Path(__file__).with_name("voice_local.py")), kind, model,
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL, env=_worker_environment(),
    )
    try:
        proc.stdin.write(payload)
        await proc.stdin.drain()
        proc.stdin.close()
        output = bytearray()
        while True:
            chunk = await proc.stdout.read(4096)
            if not chunk:
                break
            if len(output) + len(chunk) > limit:
                raise TurnError(kind + "_failed")
            output.extend(chunk)
        await proc.wait()
        if proc.returncode != 0 or (not output and kind != "stt"):
            raise TurnError(kind + "_failed")
        return bytes(output)
    finally:
        if proc.returncode is None:
            proc.kill()
        await proc.wait()



class LocalStt:
    def __init__(self, model: str | None = None):
        self.model = model if model is not None else os.getenv("VOICE_STT_MODEL", "")

    @property
    def ready(self):
        return bool(self.model) and all((Path(self.model) / name).is_file()
                                       for name in ("model.bin", "config.json", "tokenizer.json")) and any(
            (Path(self.model) / name).is_file() for name in ("vocabulary.txt", "vocabulary.json"))

    async def transcribe(self, pcm: bytes) -> str:
        if not pcm or len(pcm) > INPUT_LIMIT:
            raise TurnError("input_invalid")
        output = await _run("stt", pcm, self.model, TEXT_LIMIT)
        return output.decode("utf-8", errors="strict")


class LocalTts:
    def __init__(self, model: str | None = None):
        self.model = model if model is not None else os.getenv("VOICE_TTS_MODEL", "")
        self._process = None
        self._lock = asyncio.Lock()
        self._closed = False

    @property
    def ready(self):
        return bool(self.model) and Path(self.model).is_file() and Path(self.model + ".json").is_file()

    async def synthesize(self, answer: str) -> bytes:
        payload = answer.encode("utf-8")
        if not payload or len(payload) > TEXT_LIMIT:
            raise TurnError("text_invalid")
        async with self._operation():
            await self._start()
            self._process.stdin.write(struct.pack("!I", len(payload)) + payload)
            await self._process.stdin.drain()
            size = struct.unpack("!I", await self._process.stdout.readexactly(4))[0]
            # Validate before allocating/reading the response body.
            if not 0 < size <= TTS_OUTPUT_LIMIT or size % 2:
                raise TurnError("tts_failed")
            return await self._process.stdout.readexactly(size)

    @asynccontextmanager
    async def _operation(self):
        async with self._lock:
            try:
                async with asyncio.timeout(STAGE_SECONDS):
                    yield
            except BaseException as exc:
                await self._stop()
                if isinstance(exc, asyncio.CancelledError):
                    raise
                if isinstance(exc, TimeoutError):
                    raise TurnError("tts_timeout") from exc
                if isinstance(exc, TurnError):
                    raise
                if isinstance(exc, Exception):
                    raise TurnError("tts_failed") from exc
                raise

    async def _start(self) -> None:
        if self._closed or not self.ready:
            raise TurnError("tts_unavailable")
        if self._process is not None and self._process.returncode is None:
            return
        await self._stop()
        spawning = asyncio.create_task(asyncio.create_subprocess_exec(
            sys.executable, str(Path(__file__).with_name("voice_local.py")), "tts-serve", self.model,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL, env=_worker_environment(),
        ))
        try:
            self._process = await asyncio.shield(spawning)
        except asyncio.CancelledError:
            # Reap even if cancellation arrives while the OS is spawning it.
            self._process = await spawning
            raise
        if await self._process.stdout.readexactly(4) != b"\x00" * 4:
            raise TurnError("tts_failed")

    async def start(self) -> None:
        """Warm up at gateway startup; failures remain retryable on the next turn."""
        async with self._operation():
            await self._start()

    async def _stop(self) -> None:
        process, self._process = self._process, None
        if process is not None:
            if process.stdin is not None:
                process.stdin.close()
            if process.returncode is None:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass
            # A cancelled read can leave stdout flow-control paused. Drain after
            # killing so Process.wait() cannot hang waiting for that pipe's EOF.
            if process.stdout is not None:
                while await process.stdout.read(65536):
                    pass
            await process.wait()

    async def close(self) -> None:
        async with self._lock:
            self._closed = True
            await self._stop()
