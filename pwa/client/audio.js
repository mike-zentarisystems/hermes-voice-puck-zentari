/* One microphone owner, bounded capture and cancellable streamed playback. */
(() => {
  const clamp = value => Math.max(0, Math.min(100, Math.round(Number(value)) || 0));
  const read = (key, fallback) => { try { return localStorage.getItem(key) ?? fallback; } catch { return fallback; } };
  class AudioEngine extends EventTarget {
    constructor(gateway) {
      super(); this.gateway = gateway; this.ctx = null; this.gain = null; this._init = null;
      this.stream = null; this.source = null; this.sink = null; this.worklet = null;
      this.micMuted = false; this.inputDevice = ''; this.voiceReplies = true; this.vad = null;
      this.capturing = false; this.starting = false; this.generation = 0; this.captureTimer = null;
      this.flushTimer = null; this.flushDone = null;
      this.player = null; this.playGen = 0; this.pending = false; this.outputEnded = false; this.playing = false;
      this.level = clamp(read('zenti-volume-level', 40)); this.muted = read('zenti-volume-muted', '0') === '1';
      gateway.addEventListener('audio-start', () => { this.stopPlayback(); this.outputEnded = false; });
      gateway.addEventListener('audio', e => this._onAudioFrame(e.detail.data));
      gateway.addEventListener('audio-end', () => {
        this.outputEnded = true;
        if (this.pending) this.player.port.postMessage({ type: 'end', gen: this.playGen }); else this._drained();
      });
      gateway.addEventListener('reset', () => { this.abortCapture(); this.stopPlayback(); });
    }
    async init() {
      if (!this.ctx) {
        this.ctx = new (window.AudioContext || window.webkitAudioContext)();
        const ctx = this.ctx;
        this.gain = ctx.createGain(); this.analyser = ctx.createAnalyser(); this.analyser.fftSize = 256;
        this.gain.connect(ctx.destination); this._applyGain();
        this._init = Promise.all(['./capture-worklet.js', './playback-worklet.js'].map(url => ctx.audioWorklet.addModule(url))).then(() => {
          if (this.ctx !== ctx) return;
          const player = new AudioWorkletNode(ctx, 'zenti-playback', { numberOfInputs: 0, outputChannelCount: [1] });
          player.port.onmessage = ({ data }) => { if (this.player === player) this._onPlayer(data); };
          // The voice meter reads real playback before gain, independent of speaker volume.
          player.connect(this.gain); player.connect(this.analyser); this.player = player;
        });
      }
      // Called from the input gesture so autoplay policies permit resume.
      // A context that cannot start (no audio backend, blocked autoplay) may
      // leave resume() pending indefinitely; bound the wait so callers such as
      // text send are never held hostage by audio. The context still exists and
      // may start on a later gesture; a suspended context simply stays silent.
      const resume = Promise.race([
        this.ctx.resume(),
        new Promise(resolve => setTimeout(resolve, 1000)),
      ]);
      try { await Promise.all([this._init, resume]); }
      catch { await this.dispose(); throw Error('audio_unavailable'); }
    }
    async startCapture(handsFree = false) {
      if (this.micMuted || this.capturing || this.starting || !this.gateway.ready || this.gateway.turn) return false;
      const token = ++this.generation; this.starting = true;
      try {
        await this.init();
        if (token !== this.generation) return false;
        const stream = await navigator.mediaDevices.getUserMedia({ audio: {
          ...(this.inputDevice ? { deviceId: { exact: this.inputDevice } } : {}),
          channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true
        } });
        if (token !== this.generation || this.micMuted || !this.gateway.ready) { stream.getTracks().forEach(t => t.stop()); return false; }
        this.stream = stream;
        this.source = this.ctx.createMediaStreamSource(stream);
        this.worklet = new AudioWorkletNode(this.ctx, 'zenti-capture');
        this.sink = this.ctx.createGain(); this.sink.gain.value = 0;
        const node = this.worklet;
        node.port.onmessage = ({ data }) => {
          if (this.worklet !== node || token !== this.generation) return;
          if (data.type === 'pcm' && this.capturing) {
            this._captureFrame(data.buffer, node);
          } else if (data.type === 'flushed' && this.flushDone) this.flushDone();
          else if (data.type === 'limit') void this.stopCapture();
          else if (data.type === 'overflow') this.gateway.cancel('capture_backpressure');
        };
        this.vad = handsFree ? { pre: [], voiced: 0, silent: 0, started: false } : null;
        if (!handsFree && !this.gateway.startCapture()) { this.abortCapture(); return false; }
        this.capturing = true; this.starting = false;
        this.source.connect(node); node.connect(this.sink); this.sink.connect(this.ctx.destination);
        for (const track of stream.getTracks()) track.onended = () => this.gateway.cancel('microphone_ended');
        this.captureTimer = setTimeout(() => { void this.stopCapture(); }, 15000);
        this._emit('capture-start'); return true;
      } catch (err) {
        if (token === this.generation) { this.abortCapture(); this.gateway.cancel(err.name === 'NotAllowedError' ? 'microphone_permission_denied' : 'microphone_unavailable'); }
        return false;
      } finally { if (token === this.generation) this.starting = false; }
    }
    setMicMuted(value) {
      this.micMuted = !!value;
      if (this.micMuted) { this.abortCapture(); if (this.gateway.turn?.state === 'listening') this.gateway.cancel(); }
      this._emit('microphone', { muted: this.micMuted }); return this.micMuted;
    }
    setInputDevice(id) {
      this.abortCapture(); if (this.gateway.turn?.state === 'listening') this.gateway.cancel();
      this.inputDevice = id || '';
    }
    _captureFrame(buffer, node) {
      if (this.micMuted) { this.abortCapture(); return; }
      const vad = this.vad;
      if (vad) {
        const view = new DataView(buffer); let energy = 0;
        for (let i = 0; i < view.byteLength; i += 2) energy += (view.getInt16(i, true) / 32768) ** 2;
        const loud = Math.sqrt(energy / (view.byteLength / 2)) >= 0.015;
        if (!vad.started) {
          vad.pre.push(buffer); if (vad.pre.length > 6) vad.pre.shift();
          vad.voiced = loud ? vad.voiced + 1 : 0;
          node.port.postMessage({ type: 'credit' });
          if (vad.voiced < 3) return;
          if (!this.gateway.startCapture()) { this.abortCapture(); return; }
          vad.started = true;
          for (const frame of vad.pre) if (!this.gateway.sendAudio(frame)) { this.abortCapture(); return; }
          vad.pre = []; return;
        }
        vad.silent = loud ? 0 : vad.silent + view.byteLength / 32;
      }
      if (this.gateway.sendAudio(buffer)) node.port.postMessage({ type: 'credit' });
      else { this.abortCapture(); return; }
      if (vad?.silent >= 700) void this.stopCapture();
    }
    async stopCapture() {
      if (this.starting) { this.abortCapture(); return false; }
      if (!this.capturing || this.flushDone) return false;
      if (this.vad && !this.vad.started) { this.gateway.cancel('speech_not_detected'); return false; }
      clearTimeout(this.captureTimer);
      // Stop physical capture now, then deliver the worklet's bounded final frame.
      this.source?.disconnect(); this._stopTracks();
      const node = this.worklet, token = this.generation;
      const flushed = await new Promise(resolve => {
        this.flushDone = () => { clearTimeout(this.flushTimer); this.flushDone = null; resolve(true); };
        this.flushTimer = setTimeout(() => { this.flushDone = null; resolve(false); }, 1000);
        node.port.postMessage({ type: 'flush' });
      });
      if (token !== this.generation) return false;
      if (!flushed) { this.gateway.cancel('capture_flush_timeout'); return false; }
      this.abortCapture(); return this.gateway.stopCapture();
    }
    _stopTracks() {
      if (this.stream) {
        this.stream.getTracks().forEach(t => { t.onended = null; t.stop(); }); this.stream = null;
      }
    }
    abortCapture() {
      this.generation++; this.capturing = false; this.starting = false;
      clearTimeout(this.captureTimer); clearTimeout(this.flushTimer);
      if (this.flushDone) { const done = this.flushDone; this.flushDone = null; done(); }
      this._stopTracks(); this.source?.disconnect(); this.source = null;
      if (this.worklet) { this.worklet.port.onmessage = null; this.worklet.port.close(); this.worklet.disconnect(); this.worklet = null; }
      this.sink?.disconnect(); this.sink = null; this._emit('capture-stop');
    }
    get effectiveVolume() { return this.muted ? 0 : this.level; }
    _applyGain() { if (this.gain) this.gain.gain.value = this.effectiveVolume / 100; }
    setLevel(value) {
      this.level = clamp(value); try { localStorage.setItem('zenti-volume-level', String(this.level)); } catch {}
      this._applyGain(); this._emit('volume', this.state); return this.level;
    }
    setMuted(value) {
      this.muted = !!value; try { localStorage.setItem('zenti-volume-muted', this.muted ? '1' : '0'); } catch {}
      this._applyGain(); this._emit('volume', this.state); return this.muted;
    }
    toggleMute() { return this.setMuted(!this.muted); }
    get state() { return { level: this.level, muted: this.muted, effective: this.effectiveVolume }; }
    _onAudioFrame(buffer) {
      if (!this.voiceReplies) return;
      if (!buffer.byteLength || buffer.byteLength > 1024 || buffer.byteLength % 2) {
        this.gateway.cancel('playback_backpressure'); return;
      }
      // Audio that cannot render (no context yet, or a context that is not
      // running, e.g. suspended with no audio backend) is dropped, not
      // treated as backpressure: the turn completes as text instead of
      // being cancelled mid-reply. Playback drain is already satisfied
      // because nothing was queued.
      if (!this.ctx || this.ctx.state !== 'running' || !this.player) return;
      const copy = buffer.slice(0);
      this.player.port.postMessage({ type: 'pcm', gen: this.playGen, buffer: copy }, [copy]); this.pending = true;
      if (!this.playing) { this.playing = true; this._emit('playback-start'); }
    }
    _onPlayer(data) {
      if (data.gen !== this.playGen) return;
      if (data.type === 'overflow') this.gateway.cancel('playback_backpressure');
      else if (data.type === 'drained') { this.pending = false; this._drained(); }
    }
    _drained() {
      if (!this.outputEnded || this.pending) return;
      this.playing = false; this._emit('playback-end'); this.gateway.completePlayback();
    }
    stopPlayback() {
      this.playGen++; this.player?.port.postMessage({ type: 'stop', gen: this.playGen });
      this.pending = false; this.playing = false; this.outputEnded = false;
    }
    async dispose() {
      this.abortCapture(); this.stopPlayback();
      if (this.player) { this.player.port.onmessage = null; this.player.port.close(); this.player.disconnect(); this.player = null; }
      const ctx = this.ctx; this.ctx = null; this.gain = null; this._init = null;
      if (ctx && ctx.state !== 'closed') await ctx.close();
    }
    _emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }
  }
  window.AudioEngine = AudioEngine;
})();
