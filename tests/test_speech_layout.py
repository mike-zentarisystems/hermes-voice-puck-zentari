"""Actual subprocesses must find their sibling runner in any installed layout."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import gc

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'services/voice-gateway'))

WORKER = '''
import struct,sys
if sys.argv[1]=='stt':
    sys.stdin.buffer.read(); sys.stdout.buffer.write(b'layout fixture')
else:
    sink=sys.stdout.buffer; source=sys.stdin.buffer
    sink.write(bytes(4)); sink.flush()
    while header:=source.read(4):
        source.read(struct.unpack('!I',header)[0])
        sink.write(struct.pack('!I',4)+bytes(4)); sink.flush()
'''


class SpeechLayoutTests(unittest.IsolatedAsyncioTestCase):
    async def test_stt_readiness_requires_model_vocabulary(self):
        from local_adapters import LocalStt
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('model.bin', 'config.json', 'tokenizer.json'): (root/name).touch()
            self.assertFalse(LocalStt(temp).ready)
            (root/'vocabulary.txt').touch()
            self.assertTrue(LocalStt(temp).ready)

    async def test_runner_is_relative_to_adapter_not_container_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); nested = root / 'installed/gateway'; nested.mkdir(parents=True)
            adapter = nested / 'local_adapters.py'
            adapter.write_bytes((ROOT / 'services/voice-gateway/local_adapters.py').read_bytes())
            (nested / 'voice_local.py').write_text(WORKER)
            spec = importlib.util.spec_from_file_location('layout_adapters', adapter)
            module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
            self.assertEqual(await module._run('stt', bytes(8), str(root), 4096), b'layout fixture')
            model = root / 'voice.onnx'; model.touch(); model.with_suffix('.onnx.json').write_text('{}')
            tts = module.LocalTts(str(model))
            try:
                await tts.start()
                self.assertEqual(await tts.synthesize('layout fixture'), bytes(4))
            finally:
                await tts.close()

    async def test_repeated_non_native_voice_resampling_has_no_thread_growth(self):
        import numpy as np
        import voice_local
        if not Path('/proc/self/status').exists(): self.skipTest('Linux native thread evidence required')
        def threads():
            return int(next(line.split()[1] for line in Path('/proc/self/status').read_text().splitlines()
                            if line.startswith('Threads:')))
        samples = np.full(2205, 1000, dtype='<i2')
        # Delay cyclic collection to expose per-utterance native thread pools.
        was_enabled = gc.isenabled(); gc.disable()
        try:
            before = threads()
            for _ in range(12):
                self.assertEqual(len(voice_local.pcm_to_16k(samples, 22050)), 3200)
            self.assertLessEqual(threads(), before + 2)
        finally:
            if was_enabled: gc.enable()
            gc.collect()
