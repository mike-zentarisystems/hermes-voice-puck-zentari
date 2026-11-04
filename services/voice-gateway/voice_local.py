"""Isolated local model runner. Protocol: bounded stdin -> stdout; diagnostics discarded by caller."""
from __future__ import annotations

import io
import struct
import sys
import wave
from pathlib import Path

TTS_OUTPUT_LIMIT = 960000  # Per segment: 30 seconds of PCM16 mono/16k.
TTS_TEXT_LIMIT = 4096


def stt(model_path: str) -> None:
    from faster_whisper import WhisperModel
    pcm = sys.stdin.buffer.read(480001)
    if not pcm or len(pcm) > 480000 or len(pcm) % 2:
        raise ValueError("invalid input")
    # Two threads = the gateway's 2-CPU quota. The default (4) was throttled and slower under
    # the same cap (1.0-1.14 s vs 0.70 s, identical transcript; 2026-10-15). Nothing else runs
    # during transcription, so unlike TTS it may use the whole quota.
    model = WhisperModel(model_path, device="cpu", compute_type="int8", local_files_only=True, cpu_threads=2)
    # Input is already validated PCM16 mono/16k. Avoid an unnecessary PyAV
    # WAV round trip (newer PyAV removed metadata_errors from av.open).
    import numpy as np
    samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
    segments, _ = model.transcribe(samples, beam_size=1, vad_filter=False)
    text = " ".join(segment.text.strip() for segment in segments).strip().encode("utf-8")
    # Successful recognition with no words is a normal no-speech outcome.
    # Keep model/worker exceptions distinct so callers never hide STT faults.
    if len(text) > 4096:
        raise ValueError("invalid transcript")
    sys.stdout.buffer.write(text)


def pcm_to_16k(samples, rate: int) -> bytes:
    """Convert mono PCM16 with FFmpeg's filtered resampler; flush its tail."""
    if not len(samples) or not 8000 <= rate <= 48000:
        raise ValueError("invalid TTS audio")
    if len(samples) * 16000 > 480000 * rate:
        raise ValueError("TTS output exceeds limit")
    # Preserve the established native-16k voice byte for byte.
    if rate == 16000:
        return samples.astype("<i2", copy=False).tobytes()
    import av
    frame = av.AudioFrame.from_ndarray(samples.reshape(1, -1), format="s16", layout="mono")
    frame.sample_rate = rate
    from fractions import Fraction
    # AudioResampler's implicit filter graph starts one pool per conversion.
    # Own the graph explicitly so cyclic GC cannot accumulate native threads.
    graph = av.filter.Graph()
    graph.threads = 1
    source = graph.add_abuffer(sample_rate=rate, format="s16", layout="mono", time_base=Fraction(1, rate))
    convert = graph.add("aresample", "16000")
    fmt = graph.add("aformat", "sample_fmts=s16:channel_layouts=mono")
    sink = graph.add("abuffersink")
    graph.link_nodes(source, convert, fmt, sink)
    graph.configure()
    graph.push(frame)
    graph.push(None)
    output = bytearray()
    while True:
        try:
            item = graph.pull()
        except av.error.EOFError:
            break
        output.extend(item.to_ndarray().astype("<i2", copy=False).tobytes())
    return bytes(output)


def _load_voice(model_path: str):
    import json
    import onnxruntime
    from piper import PiperVoice
    from piper.config import PiperConfig
    # One thread: onnxruntime's default (one per host core) thrashes the gateway's 2-CPU quota,
    # synthesizing 2.5x slower and throttling the whole container, which stalls real-time
    # audio pacing while the next sentence is synthesized (2026-10-15 measurement).
    options = onnxruntime.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    with open(model_path + ".json", encoding="utf-8") as config:
        return PiperVoice(config=PiperConfig.from_dict(json.load(config)),
                           session=onnxruntime.InferenceSession(model_path, sess_options=options,
                                                                providers=["CPUExecutionProvider"]))

class _BoundedWav(io.BytesIO):
    def write(self, data):
        # At most 30 seconds at the highest accepted native rate, plus WAV header.
        if self.tell() + len(data) > 30 * 48000 * 2 + 44:
            raise ValueError("TTS output exceeds limit")
        return super().write(data)


def _synthesize(voice, text: str) -> bytes:
    import numpy as np
    output = _BoundedWav()
    with wave.open(output, "wb") as wav:
        voice.synthesize_wav(text, wav)
    output.seek(0)
    with wave.open(output, "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError("unsupported TTS output")
        samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
        rate = wav.getframerate()
    pcm = pcm_to_16k(samples, rate)
    if not pcm or len(pcm) > TTS_OUTPUT_LIMIT or len(pcm) % 2:
        raise ValueError("TTS output exceeds limit")
    return pcm


def tts(model_path: str) -> None:
    text = sys.stdin.buffer.read(TTS_TEXT_LIMIT + 1).decode("utf-8")
    if not text or len(text.encode("utf-8")) > TTS_TEXT_LIMIT:
        raise ValueError("invalid text")
    sys.stdout.buffer.write(_synthesize(_load_voice(model_path), text))


def _read_exact(stream, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            raise EOFError("truncated TTS frame")
        data.extend(chunk)
    return bytes(data)


def tts_serve(model_path: str) -> None:
    """One model per process; uint32 network-order lengths, UTF-8 in / PCM out.

    A zero-length startup frame announces a loaded model. All response frames
    after that must contain nonempty, even-length PCM. Any error exits the worker;
    the parent discards both pipes rather than trying to resynchronize them.
    """
    voice = _load_voice(model_path)
    source, sink = sys.stdin.buffer, sys.stdout.buffer
    sink.write(struct.pack("!I", 0))
    sink.flush()
    while True:
        first = source.read(1)
        if not first:
            return
        size = struct.unpack("!I", first + _read_exact(source, 3))[0]
        if not 0 < size <= TTS_TEXT_LIMIT:
            raise ValueError("invalid text length")
        text = _read_exact(source, size).decode("utf-8", errors="strict")
        pcm = _synthesize(voice, text)
        sink.write(struct.pack("!I", len(pcm)))
        sink.write(pcm)
        sink.flush()


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("stt", "tts", "tts-serve") or not (
        Path(sys.argv[2]).is_dir() if sys.argv[1] == "stt" else Path(sys.argv[2]).is_file()
    ):
        raise SystemExit(2)
    try:
        {"stt": stt, "tts": tts, "tts-serve": tts_serve}[sys.argv[1]](sys.argv[2])
    except Exception:
        raise SystemExit(1)
