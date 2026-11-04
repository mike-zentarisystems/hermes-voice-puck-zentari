/* One decoded authored pose at a time. Playback drives a separate voice meter;
 * these expression images are not aligned for full-face crossfading. */
(() => {
  class AvatarView {
    constructor(image, unavailable = () => {}, meter = null) {
      this.image = image;
      this.meter = meter;
      this.unavailable = unavailable;
      this.frames = new Map();
      this.failed = new Set();
      this.desired = '';
      this.id = 'zenti';
      this.envelope = 0;
      this.lastTime = null;
    }
    select(id) {
      this.id = ['zenti', 'orbit'].includes(id) ? id : 'zenti';
      this.frames.clear(); this.failed.clear(); this.desired = '';
      this.envelope = 0; this.lastTime = null; this.level(0);
      this.show('idle');
      // Decode the speaking pose before the first reply, without displaying it.
      void this.load(`${this.id}-speaking`).catch(() => {});
    }
    load(key) {
      if (!this.frames.has(key)) {
        const frame = new Image(); frame.src = `avatars/${key}.png`;
        this.frames.set(key, frame.decode().then(() => frame.src));
      }
      return this.frames.get(key);
    }
    show(expression) {
      if (this.failed.has(`${this.id}-${expression}`)) expression = 'idle';
      const key = `${this.id}-${expression}`;
      if (this.desired === key) return;
      this.desired = key;
      this.load(key).then(src => {
        if (this.desired !== key) return;
        this.image.src = src; this.image.hidden = false;
      }).catch(() => {
        if (this.desired !== key) return;
        if (expression !== 'idle') { this.failed.add(key); this.show('idle'); }
        else this.unavailable();
      });
    }
    update(state, rms, now, motion) {
      const states = ['idle', 'listening', 'thinking', 'working', 'speaking', 'success', 'error', 'approval'];
      this.show(state === 'offline' ? 'error' : states.includes(state) ? state : 'idle');
      const target = state === 'speaking' && motion && Number.isFinite(rms)
        ? Math.min(1, Math.max(0, (rms - 0.01) / 0.06)) : 0;
      const elapsed = this.lastTime === null ? 16 : Math.min(100, Math.max(0, now - this.lastTime));
      this.lastTime = now;
      if (state !== 'speaking' || !motion) this.envelope = 0;
      else this.envelope += (target - this.envelope) * (1 - Math.exp(-elapsed / (target > this.envelope ? 35 : 100)));
      this.level(this.envelope);
    }
    level(value) {
      if (this.meter) this.meter.style.setProperty('--voice-level', value.toFixed(3));
    }
  }
  window.AvatarView = AvatarView;
})();
