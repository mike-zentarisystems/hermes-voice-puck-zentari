/* Hold-to-talk and cancel controls. Microphone permission races stay in AudioEngine. */
class VoiceControls {
  constructor(gateway, audio, button, { state = () => {}, notice = () => {}, text = () => {} } = {}) {
    this.gateway = gateway; this.audio = audio; this.button = button; this.held = false;
    this.handlers = [];
    const on = (target, type, fn) => { target.addEventListener(type, fn); this.handlers.push(() => target.removeEventListener(type, fn)); };
    const start = e => {
      if (e.type === 'keydown' && (![' ', 'Enter'].includes(e.key) || e.repeat)) return;
      if (e.type === 'pointerdown' && e.button !== 0) return;
      e.preventDefault();
      if (gateway.turn && gateway.turn.state !== 'listening') { gateway.cancel(); return; }
      if (this.held || !gateway.ready) return;
      this.held = true;
      if (e.type === 'pointerdown') button.setPointerCapture(e.pointerId);
      button.textContent = 'RELEASE TO SEND';
      void audio.startCapture().then(() => this.render());
    };
    const stop = e => {
      if (e.type === 'keyup' && ![' ', 'Enter'].includes(e.key)) return;
      if (!this.held) return;
      e.preventDefault(); this.held = false; void audio.stopCapture().then(() => this.render());
    };
    on(button, 'pointerdown', start); on(button, 'keydown', start);
    on(button, 'pointerup', stop); on(button, 'keyup', stop);
    on(button, 'pointercancel', () => gateway.cancel());
    on(button, 'lostpointercapture', () => { if (this.held) gateway.cancel(); });
    on(window, 'keydown', e => { if (e.key === 'Escape') gateway.cancel(); });
    on(window, 'blur', () => gateway.cancel());
    on(document, 'visibilitychange', () => { if (document.hidden) gateway.cancel(); });
    on(window, 'pagehide', () => gateway.disconnect());
    on(audio, 'capture-stop', () => { this.held = false; this.render(); });
    on(gateway, 'reset', () => { this.held = false; this.render(); });
    on(gateway, 'state', e => {
      if (e.detail.detail) notice(e.detail.detail.replace(/_/g, ' ').toUpperCase());
      this.render();
    });
    on(gateway, 'voice', e => {
      state(e.detail.state === 'offline' ? 'error' : e.detail.state);
      if (e.detail.error && e.detail.error !== 'cancelled') notice(e.detail.error.replace(/_/g, ' ').toUpperCase());
      this.render();
    });
    on(gateway, 'text', e => text(e.detail));
    this.render();
  }
  render() {
    const supported = window.isSecureContext && navigator.mediaDevices?.getUserMedia && window.AudioWorkletNode;
    this.button.disabled = !supported || !this.gateway.ready;
    const turn = this.gateway.turn;
    this.button.textContent = !supported ? 'VOICE REQUIRES HTTPS' : !this.gateway.ready ? 'VOICE NOT CONNECTED'
      : this.held ? 'RELEASE TO SEND' : turn ? 'CANCEL RESPONSE' : 'HOLD TO TALK';
    this.button.classList.toggle('talking', this.held);
    this.button.setAttribute('aria-label', this.button.textContent);
    this.button.title = turn ? 'Press to cancel. Escape also cancels.' : 'Hold to speak, release to send. Maximum 15 seconds.';
  }
  dispose() { this.handlers.forEach(remove => remove()); this.gateway.disconnect(); void this.audio.dispose(); }
}
window.VoiceControls = VoiceControls;
