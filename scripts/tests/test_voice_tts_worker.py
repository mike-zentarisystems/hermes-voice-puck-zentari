"""Persistent TTS framing/lifecycle checks using real subprocesses, no assets."""
from __future__ import annotations

import asyncio
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "services/voice-gateway"))
import local_adapters
import voice_local
from voice_turn import TurnError


FAKE_WORKER = '''
import os, struct, sys, time
from pathlib import Path
source, sink = sys.stdin.buffer, sys.stdout.buffer
mode = Path(sys.argv[-1] + ".mode")
startup = mode.read_text() if mode.exists() else ""
if startup == "crash": sys.exit(1)
if startup == "hang": time.sleep(60)
sink.write(struct.pack("!I", 99 if startup == "bad" else 0)); sink.flush()
while True:
    header = source.read(4)
    if not header: break
    text = source.read(struct.unpack("!I", header)[0]).decode()
    Path(sys.argv[-1] + ".started").write_text(text)
    if text == "crash": sys.exit(1)
    if text == "truncated":
        sink.write(struct.pack("!I", 20) + b"xx"); sink.flush(); sys.exit(1)
    if text == "hang":
        sink.write(b"\\x00\\x00"); sink.flush(); time.sleep(60)
    if text in ("oversize", "odd", "empty", "flood"):
        sink.write(struct.pack("!I", {"oversize": 960002, "odd": 3, "empty": 0, "flood": 960002}[text]))
        if text == "flood": sink.write(b"x" * 262144)
        sink.flush(); time.sleep(60)
    pcm = text.encode("utf-8").hex().encode("ascii")
    sink.write(struct.pack("!I", len(pcm)) + pcm); sink.flush()
'''


class TtsWorkerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.model = self.root / "voice.onnx"
        self.model.write_bytes(b"fixture")
        Path(str(self.model) + ".json").write_text("{}")
        self.script = self.root / "worker.py"
        self.script.write_text(FAKE_WORKER, encoding="utf-8")
        self.processes = []
        self.environments = []
        original_spawn = asyncio.create_subprocess_exec

        async def spawn(*args, **kwargs):
            self.assertEqual(args[1:3], (str(Path(local_adapters.__file__).with_name("voice_local.py")), "tts-serve"))
            self.assertEqual(kwargs["stderr"], asyncio.subprocess.DEVNULL)
            self.environments.append(kwargs["env"])
            process = await original_spawn(args[0], str(self.script), *args[2:], **kwargs)
            self.processes.append(process)
            return process

        self.spawn_patch = patch.object(local_adapters.asyncio, "create_subprocess_exec", side_effect=spawn)
        self.spawn_patch.start()
        self.tts = local_adapters.LocalTts(str(self.model))

    async def asyncTearDown(self):
        await self.tts.close()
        self.spawn_patch.stop()
        for process in self.processes:
            self.assertIsNotNone(process.returncode, "worker was not reaped")
        self.temp.cleanup()

    async def wait_started(self, text):
        marker = Path(str(self.model) + ".started")
        async with asyncio.timeout(5):
            while not marker.exists() or marker.read_text() != text:
                await asyncio.sleep(0.01)

    async def assert_recovers(self, bad, code="tts_failed"):
        with self.assertRaises(TurnError) as error:
            await self.tts.synthesize(bad)
        self.assertEqual(error.exception.code, code)
        self.assertIsNone(self.tts._process)
        self.assertIsNotNone(self.processes[-1].returncode)
        self.assertEqual(await self.tts.synthesize("next"), b"6e657874")

    async def test_warmup_and_successive_utf8_requests_reuse_one_process(self):
        await self.tts.start()
        for text in ("hello", "caf\u00e9", "third"):
            self.assertEqual(await self.tts.synthesize(text), text.encode().hex().encode())
        self.assertEqual(len(self.processes), 1)

    async def test_concurrent_calls_do_not_interleave_frames(self):
        result = await asyncio.gather(*(self.tts.synthesize(text) for text in ("one", "two", "three")))
        self.assertEqual(result, [text.encode().hex().encode() for text in ("one", "two", "three")])
        self.assertEqual(len(self.processes), 1)

    async def test_crash_and_truncated_body_restart_for_next_request(self):
        for text in ("crash", "truncated"):
            await self.assert_recovers(text)

    async def test_oversized_odd_and_empty_responses_fail_before_body_read(self):
        for text in ("oversize", "odd", "empty", "flood"):
            async with asyncio.timeout(5):
                await self.assert_recovers(text)

    async def test_request_timeout_reaps_and_recovers(self):
        await self.tts.start()
        with patch.object(local_adapters, "STAGE_SECONDS", 0.2):
            with self.assertRaises(TurnError) as error:
                await self.tts.synthesize("hang")
        self.assertEqual(error.exception.code, "tts_timeout")
        self.assertIsNotNone(self.processes[-1].returncode)
        self.assertEqual(await self.tts.synthesize("next"), b"6e657874")

    async def test_cancellation_discards_partial_frame_then_recovers(self):
        task = asyncio.create_task(self.tts.synthesize("hang"))
        await self.wait_started("hang")
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertIsNotNone(self.processes[-1].returncode)
        self.assertEqual(await self.tts.synthesize("next"), b"6e657874")
        self.assertEqual(len(self.processes), 2)

    async def test_cancelled_waiter_does_not_kill_current_owner(self):
        owner = asyncio.create_task(self.tts.synthesize("hang"))
        await self.wait_started("hang")
        waiter = asyncio.create_task(self.tts.synthesize("waiting"))
        await asyncio.sleep(0)
        waiter.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await waiter
        self.assertIsNone(self.processes[-1].returncode)
        owner.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await owner

    async def test_startup_crash_bad_handshake_and_timeout_recover(self):
        mode = Path(str(self.model) + ".mode")
        for value in ("crash", "bad", "hang"):
            mode.write_text(value)
            with patch.object(local_adapters, "STAGE_SECONDS", 1):
                with self.assertRaises(TurnError) as error:
                    await self.tts.start()
            self.assertEqual(error.exception.code, "tts_timeout" if value == "hang" else "tts_failed")
            self.assertIsNone(self.tts._process)
            mode.unlink()
            await self.tts.start()
            await self.tts._stop()

    async def test_invalid_text_never_spawns(self):
        for text in ("", "x" * 4097, "\u00e9" * 2049):
            with self.assertRaises(TurnError) as error:
                await self.tts.synthesize(text)
            self.assertEqual(error.exception.code, "text_invalid")
        self.assertEqual(self.processes, [])

    async def test_maximum_utf8_request_is_accepted(self):
        text = "\u00e9" * 2048
        self.assertEqual(await self.tts.synthesize(text), text.encode().hex().encode())

    async def test_idle_worker_exit_restarts_before_next_request(self):
        await self.tts.start()
        process = self.processes[-1]
        process.kill()
        await process.wait()
        self.assertEqual(await self.tts.synthesize("next"), b"6e657874")
        self.assertEqual(len(self.processes), 2)

    async def test_cancellation_during_process_creation_reaps_child(self):
        original_spawn = local_adapters.asyncio.create_subprocess_exec
        spawned = asyncio.Event()
        release = asyncio.Event()

        async def delayed_spawn(*args, **kwargs):
            process = await original_spawn(*args, **kwargs)
            spawned.set()
            await release.wait()
            return process

        with patch.object(local_adapters.asyncio, "create_subprocess_exec", side_effect=delayed_spawn):
            task = asyncio.create_task(self.tts.start())
            await asyncio.wait_for(spawned.wait(), 5)
            task.cancel()
            release.set()
            with self.assertRaises(asyncio.CancelledError):
                await task
        self.assertIsNotNone(self.processes[-1].returncode)
        self.assertEqual(await self.tts.synthesize("next"), b"6e657874")

    async def test_missing_assets_fail_without_spawn(self):
        self.model.unlink()
        with self.assertRaises(TurnError) as error:
            await self.tts.synthesize("hello")
        self.assertEqual(error.exception.code, "tts_unavailable")
        self.assertEqual(self.processes, [])

    async def test_environment_excludes_credentials(self):
        with patch.dict(os.environ, {"OPENAI_API_KEY": "private-sentinel", "VOICE_SECRET": "private-sentinel"}):
            await self.tts.start()
        self.assertNotIn("private-sentinel", str(self.environments))
        self.assertEqual(self.environments[0]["OMP_NUM_THREADS"], "1")

    async def test_close_is_idempotent_and_prevents_restart(self):
        await self.tts.start()
        await self.tts.close()
        await self.tts.close()
        with self.assertRaises(TurnError) as error:
            await self.tts.synthesize("hello")
        self.assertEqual(error.exception.code, "tts_unavailable")
        self.assertEqual(len(self.processes), 1)


class WorkerProtocolTests(unittest.TestCase):
    def run_worker(self, payload):
        source = types.SimpleNamespace(buffer=io.BytesIO(payload))
        sink = types.SimpleNamespace(buffer=io.BytesIO())
        load = patch.object(voice_local, "_load_voice", return_value=object())
        synth = patch.object(voice_local, "_synthesize", side_effect=lambda voice, text: text.encode().hex().encode())
        with load as loaded, synth as synthesized, patch.object(sys, "stdin", source), patch.object(sys, "stdout", sink):
            voice_local.tts_serve("fixture")
        return sink.buffer.getvalue(), loaded, synthesized

    def test_worker_loads_once_and_frames_multiple_responses(self):
        frame = lambda text: struct.pack("!I", len(text.encode())) + text.encode()
        output, loaded, synthesized = self.run_worker(frame("one") + frame("caf\u00e9"))
        loaded.assert_called_once_with("fixture")
        self.assertEqual(synthesized.call_count, 2)
        self.assertEqual(output, b"\x00" * 4 + struct.pack("!I", 6) + b"6f6e65" + struct.pack("!I", 10) + b"636166c3a9")

    def test_worker_rejects_unbounded_invalid_and_truncated_requests(self):
        for payload in (struct.pack("!I", 0), struct.pack("!I", 4097), b"\x00\x00",
                        struct.pack("!I", 3) + b"x", struct.pack("!I", 1) + b"\xff"):
            with self.subTest(payload=payload):
                with self.assertRaises((ValueError, EOFError)):
                    self.run_worker(payload)

    def test_wave_buffer_is_bounded_before_large_output_is_retained(self):
        output = voice_local._BoundedWav()
        with self.assertRaises(ValueError):
            output.write(b"x" * (30 * 48000 * 2 + 45))
        self.assertEqual(output.getvalue(), b"")


class GatewayTtsLifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def run_gateway(self, warmup_error=None, server_error=None):
        import voice_gateway
        gateway = MagicMock()
        gateway.tts.start = AsyncMock(side_effect=warmup_error)
        gateway.tts.close = AsyncMock()
        server = MagicMock()
        server.__aenter__ = AsyncMock(side_effect=server_error, return_value=server)
        server.__aexit__ = AsyncMock(return_value=False)
        stop = asyncio.Event()
        stop.set()
        loop = asyncio.get_running_loop()
        with patch.dict(os.environ, {"VOICE_GATEWAY_DEVICE_REGISTRY": "fixture-registry",
                                    "VOICE_GATEWAY_TLS_CERT": "fixture-cert",
                                    "VOICE_GATEWAY_TLS_KEY": "fixture-key"}), \
                patch.object(voice_gateway, "Registry"), \
                patch.object(voice_gateway.logging, "basicConfig"), \
                patch.object(voice_gateway, "VoiceGateway", return_value=gateway), \
                patch.object(voice_gateway.ssl, "SSLContext"), \
                patch.object(voice_gateway.asyncio, "Event", return_value=stop), \
                patch.object(loop, "add_signal_handler"), \
                patch.object(voice_gateway.asyncio, "start_server", new=AsyncMock(return_value=server)), \
                patch.object(voice_gateway, "serve", new=MagicMock(return_value=server)):
            if server_error:
                with self.assertRaises(OSError):
                    await voice_gateway.main()
            else:
                await voice_gateway.main()
        gateway.tts.start.assert_awaited_once()
        gateway.tts.close.assert_awaited_once()
        return gateway

    async def test_gateway_preloads_and_reaps_worker_on_shutdown(self):
        gateway = await self.run_gateway()
        self.assertTrue(gateway.shutting_down)

    async def test_warmup_failure_is_redacted_and_gateway_remains_available(self):
        gateway = await self.run_gateway(warmup_error=TurnError("tts_failed"))
        gateway.log.emit.assert_any_call("tts_warmup_failed", category="tts_failed")
        gateway.log.emit.assert_any_call("gateway_ready")

    async def test_listener_failure_still_reaps_worker(self):
        await self.run_gateway(server_error=OSError("fixture listener failure"))


if __name__ == "__main__":
    unittest.main()
