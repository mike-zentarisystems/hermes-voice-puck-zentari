"""Local STT/TTS workers stay inside the gateway's 2-CPU quota (no model assets needed)."""
from __future__ import annotations

import importlib.util
import io
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

WORKER = Path(__file__).resolve().parents[2] / "services" / "voice-gateway" / "voice_local.py"


@unittest.skipUnless(importlib.util.find_spec("numpy"), "numpy unavailable; run inside the gateway image")
class SttThreadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Load the C extension before patch.dict snapshots sys.modules. Removing
        # a newly imported numpy module makes a later test try to reload it.
        import numpy  # noqa: F401

    def test_successful_empty_recognition_does_not_crash_worker(self):
        class Model:
            def __init__(self, *args, **kwargs): pass
            def transcribe(self, samples, **kwargs): return [], None
        fake = types.ModuleType("faster_whisper")
        fake.WhisperModel = Model
        spec = importlib.util.spec_from_file_location("voice_local_empty_test", WORKER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        stdin = types.SimpleNamespace(buffer=io.BytesIO(b"\0\0"*1600))
        stdout = types.SimpleNamespace(buffer=io.BytesIO())
        with patch.dict(sys.modules,{"faster_whisper":fake}), patch.object(sys,"stdin",stdin), patch.object(sys,"stdout",stdout):
            module.stt("/models/stt")
        self.assertEqual(stdout.buffer.getvalue(), b"")

    def test_stt_uses_two_cpu_threads(self):
        calls = {}

        class Model:
            def __init__(self, path, **kwargs):
                calls.update(kwargs)

            def transcribe(self, samples, **kwargs):
                return [types.SimpleNamespace(text=" hello ")], None

        fake = types.ModuleType("faster_whisper")
        fake.WhisperModel = Model
        spec = importlib.util.spec_from_file_location("voice_local_under_test", WORKER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        stdin = types.SimpleNamespace(buffer=io.BytesIO(b"\x00\x00" * 1600))
        stdout = types.SimpleNamespace(buffer=io.BytesIO())
        with patch.dict(sys.modules, {"faster_whisper": fake}), patch.object(sys, "stdin", stdin), \
                patch.object(sys, "stdout", stdout):
            module.stt("/models/stt")
        self.assertEqual(calls.get("cpu_threads"), 2)
        self.assertEqual(stdout.buffer.getvalue(), b"hello")


if __name__ == "__main__":
    unittest.main()
