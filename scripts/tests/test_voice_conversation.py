"""Profile ownership, bounded context and text path; fixture Hermes only."""
import asyncio
import json
from pathlib import Path
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "services/voice-gateway"))
from conversation import Conversations
from voice_turn import VoiceTurnSession


class Hermes:
    def __init__(self): self.prompts = []
    async def answer(self, prompt):
        self.prompts.append(prompt)
        return "Fixture reply"


class ConversationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "profiles.json"
        self.path.write_text(json.dumps({"devices": {"puck-owner-0001": "owner", "browser-owner-01": "owner", "puck-other-0001": "other"},
            "profiles": {"owner": {"preferred_name": "Mike"}, "other": {"preferred_name": "Alice"}}}))
        self.store = Conversations(str(self.path)); self.hermes = Hermes()

    async def asyncTearDown(self): self.temp.cleanup()

    async def test_profile_context_reset_reconnect_and_isolation(self):
        item = self.store.open("puck-owner-0001")
        await self.store.answer("puck-owner-0001", item, "My project is Juniper", self.hermes)
        await self.store.answer("puck-owner-0001", item, "What project?", self.hermes)
        self.assertIn('"preferred_name": "Mike"', self.hermes.prompts[-1])
        self.assertIn("Juniper", self.hermes.prompts[-1]); self.assertIn("Fixture reply", self.hermes.prompts[-1])
        self.assertIs(self.store.open("puck-owner-0001", item.id), item)
        for device in ("puck-other-0001", "browser-owner-01"):
            with self.assertRaises(ValueError): self.store.open(device, item.id)
        fresh = self.store.open("puck-owner-0001", reset=True)
        self.assertNotEqual(item.id, fresh.id); self.assertEqual(fresh.messages, [])
        await self.store.answer("puck-owner-0001", fresh, "Name?", self.hermes)
        self.assertIn("Mike", self.hermes.prompts[-1]); self.assertNotIn("Juniper", self.hermes.prompts[-1])

    async def test_profile_changes_invalidate_ownership_and_missing_profile_is_explicit(self):
        item = self.store.open("puck-owner-0001")
        self.path.write_text('{"devices":{},"profiles":{}}')
        with self.assertRaises(ValueError): await self.store.answer("puck-owner-0001", item, "Recall?", self.hermes)
        unknown = self.store.open("puck-unknown-01")
        response = await self.store.answer("puck-unknown-01", unknown, "Name?", self.hermes)
        self.assertIn("profile is unavailable", response); self.assertNotIn("Mike", self.hermes.prompts[-1])

    async def test_context_and_total_conversations_are_bounded(self):
        item = self.store.open("puck-owner-0001")
        for i in range(24): await self.store.answer("puck-owner-0001", item, str(i) + 'x' * 400, self.hermes)
        self.assertLessEqual(len(item.messages), 16)
        self.assertTrue(all(len(p.encode()) <= 4096 for p in self.hermes.prompts))
        for i in range(80): self.store.open(f"fixture-{i:08d}")
        self.assertLessEqual(len(self.store.items), 64); self.assertLessEqual(len(self.store.defaults), 64)

    async def test_text_uses_real_turn_path_without_stt_and_rejects_foreign_context(self):
        class Stt:
            async def transcribe(self, _): raise AssertionError("text must not use microphone")
        class Tts:
            async def synthesize(self, _): return b'\x00\x00' * 16
        events = []
        async def send(event): events.append(event)
        session = VoiceTurnSession("puck-owner-0001", str(uuid.uuid4()), asyncio.Lock(), send,
                                   Stt(), self.hermes, Tts(), conversations=self.store)
        async def event(kind, seq, **extra):
            await session.receive(dict(v=1, type=kind, device_id=session.device_id,
                session_id=session.session_id, correlation_id=str(uuid.uuid4()), seq=seq, **extra))
        await event("conversation.open", 1)
        self.assertEqual(events[-1]["type"], "conversation.ready")
        await event("text.input", 2, text="What is my name?")
        await session.task
        self.assertIn("Mike", self.hermes.prompts[-1])
        self.assertEqual(events[-1]["type"], "audio.output.stop")
        foreign = self.store.open("puck-other-0001")
        await event("conversation.open", 3, conversation_id=foreign.id)
        self.assertEqual(events[-1]["code"], "conversation_unavailable")
        await session.close()
