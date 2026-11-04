"""Signal-based checks for the actual local TTS conversion, with no model calls."""
import importlib.util
from pathlib import Path
import unittest

try:
    import numpy as np
except ImportError:  # gateway image provides numpy; repo validation must collect cleanly without it
    np = None

RUNNER = Path('/app/voice_local.py')
TEST_PATH = Path(__file__).resolve()
if len(TEST_PATH.parents) >= 3:
    candidate = TEST_PATH.parents[2] / 'services/voice-gateway/voice_local.py'
    if candidate.is_file():
        RUNNER = candidate
spec = importlib.util.spec_from_file_location('voice_local_resampling', RUNNER)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


@unittest.skipUnless(importlib.util.find_spec("numpy"), "numpy unavailable; run inside the gateway image")
class VoiceResamplingTests(unittest.TestCase):
    def tone(self, frequency):
        return np.rint(12000 * np.sin(2 * np.pi * frequency * np.arange(22050) / 22050)).astype('<i2')

    def test_native_16k_preserves_all_pcm_values(self):
        samples = np.arange(-32768, 32768, dtype=np.int32).astype('<i2')
        self.assertEqual(runner.pcm_to_16k(samples, 16000), samples.tobytes())

    def test_preserves_duration_pitch_and_passband_level(self):
        output = np.frombuffer(runner.pcm_to_16k(self.tone(1000), 22050), dtype='<i2')
        self.assertLessEqual(abs(len(output) - 16000), 1)
        central = output[1000:-1000].astype(np.float64)
        spectrum = np.abs(np.fft.rfft(central))
        frequency = np.argmax(spectrum) * 16000 / len(central)
        self.assertLess(abs(frequency - 1000), 2)
        self.assertAlmostEqual(float(np.sqrt(np.mean(central ** 2))) / (12000 / np.sqrt(2)), 1, delta=0.015)

    def test_rejects_above_nyquist_energy_before_downsampling(self):
        output = np.frombuffer(runner.pcm_to_16k(self.tone(10000), 22050), dtype='<i2')
        rms = float(np.sqrt(np.mean(output[1000:-1000].astype(np.float64) ** 2)))
        self.assertLess(rms / (12000 / np.sqrt(2)), 0.0032)  # At least 49 dB attenuation.

    def test_flush_keeps_short_utterance_tail(self):
        samples = np.full(2205, 10000, dtype='<i2')
        output = np.frombuffer(runner.pcm_to_16k(samples, 22050), dtype='<i2')
        self.assertEqual(len(output), 1600)
        self.assertGreater(float(output[-32:].mean()), 9900)

    def test_invalid_or_oversize_audio_fails_closed(self):
        for samples, rate in ((np.zeros(0, dtype='<i2'), 22050),
                              (np.zeros(10, dtype='<i2'), 0),
                              (np.zeros(480001, dtype='<i2'), 16000)):
            with self.subTest(rate=rate, frames=len(samples)):
                with self.assertRaises(ValueError):
                    runner.pcm_to_16k(samples, rate)


if __name__ == '__main__':
    unittest.main()
