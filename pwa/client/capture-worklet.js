/* Streaming FIR resampling to 16 kHz mono PCM16LE, 512 samples per frame.
 * Eight transfer credits bound the AudioWorklet -> main-thread queue. */
class ZentiCapture extends AudioWorkletProcessor {
  constructor() {
    super(); this.active = true; this.credits = 8; this.count = 0;
    this.frame = new ArrayBuffer(1024); this.view = new DataView(this.frame); this.used = 0;
    this.index = 0; this.next = 0; this.previous = 0; this.position = 0;
    this.history = new Float64Array(63); this.filter = new Float64Array(63);
    const cutoff = Math.min(1, 16000 / sampleRate) * 0.45;
    let sum = 0;
    for (let i = 0; i < 63; i++) {
      const x = i - 31;
      const sinc = x === 0 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * x) / (Math.PI * x);
      this.filter[i] = sinc * (0.54 - 0.46 * Math.cos(2 * Math.PI * i / 62)); sum += this.filter[i];
    }
    for (let i = 0; i < 63; i++) this.filter[i] /= sum;
    this.port.onmessage = ({ data }) => {
      if (data.type === 'credit') this.credits = Math.min(8, this.credits + 1);
      if (data.type === 'flush') {
        this.active = false; this.send(); this.port.postMessage({ type: 'flushed' });
      }
    };
  }
  send() {
    if (!this.used) return;
    if (!this.credits) { this.active = false; this.port.postMessage({ type: 'overflow' }); return; }
    const buffer = this.used === 512 ? this.frame : this.frame.slice(0, this.used * 2);
    this.credits--; this.port.postMessage({ type: 'pcm', buffer }, [buffer]);
    this.frame = new ArrayBuffer(1024); this.view = new DataView(this.frame); this.used = 0;
  }
  process(inputs) {
    if (!this.active) return true;
    const channels = inputs[0]; if (!channels?.length) return true;
    for (let i = 0; i < channels[0].length && this.active; i++) {
      let mono = 0;
      for (const channel of channels) mono += channel[i] || 0;
      mono /= channels.length;
      this.history[this.position] = mono;
      let filtered = 0;
      for (let k = 0; k < 63; k++) filtered += this.filter[k] * this.history[(this.position - k + 63) % 63];
      this.position = (this.position + 1) % 63;
      while (this.next <= this.index && this.active) {
        const fraction = this.next - (this.index - 1);
        const value = Math.max(-1, Math.min(1, this.previous + (filtered - this.previous) * fraction));
        this.view.setInt16(this.used++ * 2, Math.round(value * (value < 0 ? 32768 : 32767)), true);
        this.count++; this.next += sampleRate / 16000;
        if (this.used === 512) this.send();
        if (this.count === 240000) {
          this.send(); this.active = false; this.port.postMessage({ type: 'limit' });
        }
      }
      this.previous = filtered; this.index++;
    }
    return true;
  }
}
registerProcessor('zenti-capture', ZentiCapture);
