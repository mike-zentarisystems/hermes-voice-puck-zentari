"""M3a transport limits and fail-closed turn behavior without model assets."""
from __future__ import annotations

import asyncio
import base64
import sys
import unittest
import uuid
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "services" / "voice-gateway"))
import voice_turn
from voice_turn import PCM, VoiceTurnSession, UnavailableHermes, _segments


class FakeStt:
    async def transcribe(self, pcm):
        return "What's the status of the server?"


class FakeTts:
    async def synthesize(self, text):
        return b"\x00\x00" * 256


class FakeHermes:
    async def answer(self, text):
        return "The server is available."


class VoiceTurnTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.sent = []
        self.worker = asyncio.Lock()
        self.session = VoiceTurnSession("puck-lab-0001", str(uuid.uuid4()), self.worker,
                                        self._send, FakeStt(), FakeHermes(), FakeTts())
        self.correlation = str(uuid.uuid4())
        self.seq = 0

    async def asyncTearDown(self):
        await self.session.close()

    async def _send(self, frame):
        self.sent.append(frame)

    async def event(self, kind, **fields):
        self.seq += 1
        frame = {"v": 1, "type": kind, "device_id": self.session.device_id,
                 "session_id": self.session.session_id, "correlation_id": self.correlation,
                 "seq": self.seq, **fields}
        await self.session.receive(frame)

    async def test_round_trip_is_bounded_and_ordered(self):
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data=base64.b64encode(b"\x00\x00" * 512).decode())
        await self.event("audio.input.stop")
        await self.session.task
        self.assertEqual([item["type"] for item in self.sent],
                         ["transcript.final", "assistant.thinking", "assistant.text",
                          "audio.output.start", "audio.output.chunk", "audio.output.stop"])
        self.assertEqual([item["seq"] for item in self.sent], list(range(1, 7)))
        self.assertFalse(self.worker.locked())

    async def test_stage_logging_has_identifiers_not_transcript_or_answer(self):
        async def operation():
            return "private-transcript-sentinel"
        with self.assertLogs("voice_turn", level="INFO") as captured:
            result = await self.session._stage(operation(), "stt")
        self.assertEqual(result, "private-transcript-sentinel")
        self.assertIn(self.session.device_id, str(captured.output))
        self.assertNotIn("sentinel", str(captured.output))
        self.assertIn('"stage":"stt"', str(captured.output))

    async def test_no_speech_skips_agent_and_audio_then_next_turn_recovers(self):
        calls = []
        class EmptyStt:
            async def transcribe(self, pcm): return " \t\n"
        class UnexpectedHermes:
            async def answer(self, text): calls.append("hermes"); return "unexpected"
        class UnexpectedTts:
            async def synthesize(self, text): calls.append("tts"); return b"\0\0"
        self.session.stt, self.session.hermes, self.session.tts = EmptyStt(), UnexpectedHermes(), UnexpectedTts()
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data=base64.b64encode(b"\0\0"*512).decode())
        await self.event("audio.input.stop")
        await self.session.task
        self.assertEqual([(event["type"],event.get("code")) for event in self.sent], [("error","no_speech")])
        self.assertEqual(calls, [])
        self.assertFalse(self.worker.locked())
        self.assertIsNone(self.session.turn)
        self.assertFalse(self.session.closed)
        self.session.stt, self.session.hermes, self.session.tts = FakeStt(), FakeHermes(), FakeTts()
        self.sent.clear()
        self.correlation = str(uuid.uuid4())
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data=base64.b64encode(b"\0\0"*512).decode())
        await self.event("audio.input.stop")
        await self.session.task
        self.assertEqual(self.sent[-1]["type"], "audio.output.stop")
        self.assertFalse(self.worker.locked())

    async def test_each_adapter_failure_is_redacted_and_next_turn_recovers(self):
        class Failure:
            async def transcribe(self, value): raise RuntimeError("secret-sentinel")
            async def answer(self, value): raise RuntimeError("secret-sentinel")
            async def synthesize(self, value): raise RuntimeError("secret-sentinel")
        defaults = {"stt": FakeStt(), "hermes": FakeHermes(), "tts": FakeTts()}
        for stage in defaults:
            with self.subTest(stage=stage):
                self.session.stt, self.session.hermes, self.session.tts = defaults.values()
                setattr(self.session, stage, Failure())
                self.sent.clear()
                self.correlation = str(uuid.uuid4())
                await self.event("audio.input.start", format=PCM)
                await self.event("audio.input.chunk", data=base64.b64encode(b"\x00\x00").decode())
                await self.event("audio.input.stop")
                await self.session.task
                self.assertEqual(self.sent[-1]["code"], stage + "_failed")
                self.assertFalse(any(x["type"].startswith("audio.output") for x in self.sent))
                self.assertNotIn("sentinel", str(self.sent))
                self.assertFalse(self.worker.locked())
        self.session.stt, self.session.hermes, self.session.tts = defaults.values()
        self.sent.clear()
        self.correlation = str(uuid.uuid4())
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data=base64.b64encode(b"\x00\x00").decode())
        await self.event("audio.input.stop")
        await self.session.task
        self.assertEqual(self.sent[-1]["type"], "audio.output.stop")

    async def test_stage_timeout_cancels_pending_work_and_releases_slot(self):
        stopped = asyncio.Event()
        class SlowHermes:
            async def answer(self, value):
                try:
                    await asyncio.sleep(60)
                finally:
                    stopped.set()
        self.session.hermes = SlowHermes()
        with patch("voice_turn.STAGE_SECONDS", .01):
            await self.event("audio.input.start", format=PCM)
            await self.event("audio.input.chunk", data=base64.b64encode(b"\x00\x00").decode())
            await self.event("audio.input.stop")
            await self.session.task
        self.assertTrue(stopped.is_set())
        self.assertFalse(self.worker.locked())
        self.assertEqual(self.sent[-1]["code"], "hermes_timeout")

    async def test_nested_agent_deadlines_are_not_clipped_to_legacy_stage(self):
        import os
        from agent_adapter import UnixAgent
        with patch.dict(os.environ, {'VOICE_AGENT_TIMEOUT_SECONDS': '59', 'VOICE_AGENT_DEVICE_IDS': ''}):
            adapter = UnixAgent('fixture-socket')
        self.assertEqual(adapter.timeout, 59)
        self.assertLess(adapter.timeout, adapter.stage_timeout)
        self.session.hermes = adapter
        async def completed():
            return 'Ready.'
        with patch('voice_turn.asyncio.wait_for', wraps=asyncio.wait_for) as wait:
            self.assertEqual(await self.session._stage(completed(), 'hermes'), 'Ready.')
        self.assertEqual(wait.call_args.args[1], 60)

    async def test_wrong_identity_and_sequence_fail_closed(self):
        await self.session.receive({"v": 1, "type": "audio.input.start", "device_id": "other-device",
                                    "session_id": self.session.session_id, "correlation_id": self.correlation,
                                    "seq": 1, "format": PCM})
        self.assertEqual(self.sent[-1]["code"], "identity_invalid")
        self.assertIsNone(self.session.turn)
        await self.event("audio.input.start", format=PCM)
        self.seq -= 1
        await self.event("audio.input.stop")
        self.assertEqual(self.sent[-1]["code"], "sequence_invalid")

    async def test_malformed_chunk_and_empty_stop_fail_closed(self):
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data="%%%")
        self.assertEqual(self.sent[-1]["code"], "audio_invalid")
        self.assertIsNone(self.session.turn)
        self.correlation = str(uuid.uuid4())
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.stop")
        self.assertEqual(self.sent[-1]["code"], "input_empty")

    async def test_missing_readonly_hermes_cannot_emit_audio(self):
        self.session.hermes = UnavailableHermes()
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data=base64.b64encode(b"\x00\x00").decode())
        await self.event("audio.input.stop")
        await self.session.task
        self.assertEqual(self.sent[-1]["code"], "hermes_readonly_unavailable")
        self.assertFalse(any(item["type"].startswith("audio.output") for item in self.sent))

    async def test_correlation_cannot_be_reused_after_turn(self):
        await self.event("audio.input.start", format=PCM)
        await self.event("cancel")
        await self.event("audio.input.start", format=PCM)
        self.assertEqual(self.sent[-1]["code"], "turn_stale")
        self.assertIsNone(self.session.turn)

    async def test_other_session_is_busy_while_input_buffer_owned(self):
        await self.event("audio.input.start", format=PCM)
        other_frames = []
        async def send(frame): other_frames.append(frame)
        other = VoiceTurnSession("puck-lab-0002", str(uuid.uuid4()), self.worker,
                                 send, FakeStt(), FakeHermes(), FakeTts())
        await other.receive({"v": 1, "type": "audio.input.start", "device_id": other.device_id,
                             "session_id": other.session_id, "correlation_id": str(uuid.uuid4()),
                             "seq": 1, "format": PCM})
        self.assertEqual(other_frames[-1]["code"], "busy")
        self.assertIsNone(other.turn)
        await other.close()

    async def test_cancel_frees_worker_and_new_turn(self):
        ready = asyncio.Event()
        class SlowStt:
            async def transcribe(self, pcm):
                ready.set()
                await asyncio.sleep(60)
        self.session.stt = SlowStt()
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data=base64.b64encode(b"\x00\x00").decode())
        await self.event("audio.input.stop")
        await ready.wait()
        await self.event("cancel")
        self.assertIsNone(self.session.turn)
        self.assertFalse(self.worker.locked())
        self.correlation = str(uuid.uuid4())
        await self.event("audio.input.start", format=PCM)
        self.assertIsNotNone(self.session.turn)

    async def test_silent_recording_expires_and_next_turn_can_start(self):
        with patch("voice_turn.TURN_SECONDS", .02):
            await self.event("audio.input.start", format=PCM)
            deadline = self.session.deadline
            await asyncio.wait_for(deadline, 1)
        self.assertIsNone(self.session.turn)
        self.assertFalse(self.worker.locked())
        self.assertEqual(self.sent[-1]["code"], "turn_timeout")
        self.correlation = str(uuid.uuid4())
        await self.event("audio.input.start", format=PCM)
        self.assertIsNotNone(self.session.turn)

    async def test_whole_turn_deadline_cancels_processing_before_recovery(self):
        started, stopped = asyncio.Event(), asyncio.Event()
        class SlowStt:
            async def transcribe(self, pcm):
                started.set()
                try:
                    await asyncio.sleep(60)
                finally:
                    stopped.set()
        self.session.stt = SlowStt()
        with patch("voice_turn.TURN_SECONDS", .03):
            await self.event("audio.input.start", format=PCM)
            deadline = self.session.deadline
            await self.event("audio.input.chunk", data="AAA=")
            await self.event("audio.input.stop")
            await started.wait()
            # The processing timeout and independent watchdog share the budget;
            # either may win, but work must stop before the slot is released.
            await asyncio.wait_for(asyncio.gather(deadline, return_exceptions=True), 1)
            if self.session.task is not None:
                await self.session.task
        self.assertTrue(stopped.is_set())
        self.assertFalse(self.worker.locked())
        self.assertIsNone(self.session.turn)
        self.assertFalse(any(x["type"].startswith("audio.output") for x in self.sent))
        self.assertEqual(self.sent[-1]["code"], "turn_timeout")

    async def test_failed_error_delivery_still_cancels_input_and_releases_worker(self):
        await self.event("audio.input.start", format=PCM)
        async def broken(frame):
            raise RuntimeError("transport closed")
        self.session.send = broken
        with self.assertRaises(RuntimeError):
            await self.event("audio.input.chunk", data="%%%")
        self.assertIsNone(self.session.turn)
        self.assertIsNone(self.session.deadline)
        self.assertFalse(self.worker.locked())

    async def test_completed_turn_removes_deadline(self):
        await self.event("audio.input.start", format=PCM)
        deadline = self.session.deadline
        await self.event("audio.input.chunk", data="AAA=")
        await self.event("audio.input.stop")
        await self.session.task
        await asyncio.sleep(0)
        self.assertTrue(deadline.cancelled())
        self.assertIsNone(self.session.deadline)

    async def test_background_transport_failure_closes_session_and_releases_slot(self):
        async def broken(frame):
            raise RuntimeError("transport-secret-sentinel")
        self.session.send = broken
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data="AAA=")
        await self.event("audio.input.stop")
        await self.session.task
        self.assertTrue(self.session.closed)
        self.assertFalse(self.worker.locked())
        self.assertIsNone(self.session.deadline)
        self.assertFalse(self.sent)

    async def test_failed_processing_error_delivery_does_not_leave_task_exception(self):
        self.session.hermes = UnavailableHermes()
        async def broken_error(frame):
            if frame["type"] == "error":
                raise RuntimeError("transport closed")
        self.session.send = broken_error
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data="AAA=")
        await self.event("audio.input.stop")
        await self.session.task
        self.assertTrue(self.session.closed)
        self.assertFalse(self.worker.locked())

    async def test_concurrent_cancel_and_disconnect_keep_slot_until_cleanup(self):
        started, cleaning, finish = asyncio.Event(), asyncio.Event(), asyncio.Event()
        class SlowStt:
            async def transcribe(self, pcm):
                started.set()
                try:
                    await asyncio.sleep(60)
                finally:
                    cleaning.set()
                    await finish.wait()
        self.session.stt = SlowStt()
        await self.event("audio.input.start", format=PCM)
        await self.event("audio.input.chunk", data="AAA=")
        await self.event("audio.input.stop")
        await started.wait()
        cancellation = asyncio.create_task(self.session.cancel())
        await cleaning.wait()
        disconnect = asyncio.create_task(self.session.close())
        await asyncio.sleep(.01)
        self.assertTrue(self.worker.locked())
        self.assertFalse(disconnect.done())
        finish.set()
        await asyncio.wait_for(asyncio.gather(cancellation, disconnect), 1)
        self.assertFalse(self.worker.locked())


class SentencePipelineTests(unittest.IsolatedAsyncioTestCase):
    ANSWER = "Yes. The GPU is at 41 degrees and idle. Memory use is normal. Nothing needs attention right now."

    async def turn(self, tts, answer=ANSWER):
        sent = []
        class Hermes:
            async def answer(self, text):
                return answer
        hermes = Hermes()
        async def send(frame):
            sent.append(frame)
            if frame["type"] == "audio.output.start":
                tts.started.set()
        session = VoiceTurnSession("puck-lab-0001", str(uuid.uuid4()), asyncio.Lock(), send, FakeStt(), hermes, tts)
        correlation, seq = str(uuid.uuid4()), 0
        for kind, fields in [("audio.input.start", {"format": PCM}),
                             ("audio.input.chunk", {"data": base64.b64encode(b"\x00\x00" * 512).decode()}),
                             ("audio.input.stop", {})]:
            seq += 1
            await session.receive({"v": 1, "type": kind, "device_id": session.device_id, "session_id": session.session_id,
                                   "correlation_id": correlation, "seq": seq, **fields})
        await asyncio.wait_for(session.task, 5)  # Short fixture audio keeps real pacing fast.
        await session.close()
        return sent

    def test_segments_start_short_and_group_the_rest(self):
        self.assertEqual(_segments("The server is available."), ["The server is available."])
        segments = _segments(self.ANSWER)
        self.assertEqual(segments[0], "Yes. The GPU is at 41 degrees and idle.")
        self.assertEqual(" ".join(segments), self.ANSWER)
        long = " ".join(f"Sentence number {i} is here." for i in range(40))
        self.assertTrue(all(len(part) <= 250 for part in _segments(long)[1:]))
        self.assertEqual(" ".join(_segments(long)), long)

    async def test_speech_starts_before_later_segments_are_synthesized(self):
        class Gated:
            def __init__(self):
                self.started, self.calls = asyncio.Event(), []
            async def synthesize(self, text):
                self.calls.append(text)
                if len(self.calls) > 1:
                    await self.started.wait()  # Deadlocks if the whole reply is synthesized first.
                return b"\x00\x00" * 600
        tts = Gated()
        sent = await self.turn(tts)
        self.assertEqual(tts.calls, _segments(self.ANSWER))
        types = [item["type"] for item in sent]
        self.assertEqual(types[:4], ["transcript.final", "assistant.thinking", "assistant.text", "audio.output.start"])
        self.assertEqual(types[-1], "audio.output.stop")
        self.assertEqual(types.count("audio.output.start"), 1)
        chunks = [base64.b64decode(item["data"]) for item in sent if item["type"] == "audio.output.chunk"]
        self.assertEqual(sum(map(len, chunks)), 1200 * len(tts.calls))
        self.assertTrue(all(len(chunk) <= 1024 and len(chunk) % 2 == 0 for chunk in chunks))
        self.assertEqual([item["seq"] for item in sent], list(range(1, len(sent) + 1)))

    async def test_later_segment_failure_ends_spoken_audio_cleanly(self):
        class FailsSecond:
            started, calls = asyncio.Event(), 0
            async def synthesize(self, text):
                type(self).calls += 1
                if self.calls > 1:
                    raise RuntimeError("tts sentinel")
                return b"\x00\x00" * 600
        sent = await self.turn(FailsSecond())
        types = [item["type"] for item in sent]
        self.assertNotIn("error", types)
        self.assertEqual(types[-1], "audio.output.stop")
        self.assertEqual(sum(len(base64.b64decode(item["data"])) for item in sent if item["type"] == "audio.output.chunk"), 1200)

    async def test_first_segment_failure_and_total_output_limit_fail_closed(self):
        class Fails:
            started = asyncio.Event()
            async def synthesize(self, text):
                raise RuntimeError("tts sentinel")
        types = [item["type"] for item in await self.turn(Fails())]
        self.assertNotIn("audio.output.start", types); self.assertEqual(types[-1], "error")
        class Large:
            started = asyncio.Event()
            async def synthesize(self, text):
                return b"\x00\x00" * 1200
        with patch.object(voice_turn, "OUTPUT_LIMIT", 4000):  # Limit spans segments: the second would exceed it.
            sent = await self.turn(Large())
        self.assertEqual(sent[-1]["type"], "audio.output.stop")
        self.assertEqual(sum(len(base64.b64decode(item["data"])) for item in sent if item["type"] == "audio.output.chunk"), 2400)


    async def test_reply_leads_by_four_chunks_then_paces_in_real_time(self):
        class Tts:
            started = asyncio.Event()
            async def synthesize(self, text):
                return b"\x00\x00" * 512 * 10  # ten 1024-byte chunks = 320 ms
        loop = asyncio.get_running_loop()
        start = loop.time()
        sent = await self.turn(Tts(), answer="The server is available.")
        self.assertEqual(sum(item["type"] == "audio.output.chunk" for item in sent), 10)
        elapsed = loop.time() - start
        # Unpaced lead of 4 chunks, then 6 paced chunks of 32 ms each.
        self.assertGreaterEqual(elapsed, 6 * 0.032 - 0.01)
        self.assertLess(elapsed, 10 * 0.032 - 0.04)
