/* Continuous 16 kHz PCM16 playback: jitter prebuffer and windowed-sinc
 * resampling to the context rate across chunk boundaries. Bounded at ~2 s. */
const SIZE = 32768, MASK = SIZE - 1, HALF = 8, PHASES = 64;
const START = 3200, RESUME = 1600; // 200 ms before speech starts, 100 ms after an underrun.
class ZentiPlayback extends AudioWorkletProcessor {
  constructor() {
    super(); this.ring = new Float32Array(SIZE); this.step = 16000 / sampleRate;
    this.table = new Float32Array((PHASES + 1) * 2 * HALF);
    for (let p = 0; p <= PHASES; p++) {
      let sum = 0;
      for (let j = 0; j < 2 * HALF; j++) {
        const t = j - (HALF - 1) - p / PHASES, x = Math.PI * 0.9 * t;
        const w = 0.42 + 0.5 * Math.cos(Math.PI * t / HALF) + 0.08 * Math.cos(2 * Math.PI * t / HALF);
        const value = Math.abs(t) >= HALF ? 0 : (x === 0 ? 1 : Math.sin(x) / x) * w;
        this.table[p * 2 * HALF + j] = value; sum += value;
      }
      for (let j = 0; j < 2 * HALF; j++) this.table[p * 2 * HALF + j] /= sum;
    }
    this.reset(0);
    this.port.onmessage = ({ data }) => {
      if (data.type === 'stop') { this.reset(data.gen); return; }
      if (data.gen !== this.gen) return;
      if (data.type === 'end') { this.ended = true; return; }
      if (data.type !== 'pcm') return;
      const view = new DataView(data.buffer), count = view.byteLength >> 1;
      if (this.write - this.read + count > SIZE - 4 * HALF) { this.reset(this.gen); this.port.postMessage({ type: 'overflow', gen: this.gen }); return; }
      for (let i = 0; i < count; i++) this.ring[(this.write + i) & MASK] = view.getInt16(i * 2, true) / 32768;
      this.write += count;
    };
  }
  reset(gen) {
    this.gen = gen; this.ring.fill(0); this.write = 0; this.read = 0; this.frac = 0;
    this.playing = false; this.ended = false; this.need = START; this.done = false;
  }
  process(_inputs, outputs) {
    const out = outputs[0][0], ring = this.ring, table = this.table;
    for (let i = 0; i < out.length; i++) {
      const available = this.write - this.read;
      if (!this.playing) {
        if (available < this.need && !(this.ended && available > 0)) { out[i] = 0; continue; }
        this.playing = true;
      }
      if (this.ended && available <= 0) {
        out[i] = 0;
        if (!this.done) { this.done = true; this.playing = false; this.port.postMessage({ type: 'drained', gen: this.gen }); }
        continue;
      }
      if (!this.ended && available < HALF + 1) { this.playing = false; this.need = RESUME; out[i] = 0; continue; }
      const base = Math.round(this.frac * PHASES) * 2 * HALF, first = this.read - (HALF - 1);
      let y = 0;
      for (let j = 0; j < 2 * HALF; j++) {
        const index = first + j;
        if (index >= 0 && index < this.write) y += ring[index & MASK] * table[base + j];
      }
      out[i] = y;
      this.frac += this.step;
      while (this.frac >= 1) { this.frac -= 1; this.read++; }
    }
    return true;
  }
}
registerProcessor('zenti-playback', ZentiPlayback);
