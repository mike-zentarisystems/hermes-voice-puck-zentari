/* Zenti PWA — interactive shell.
 *
 * Mirrors the device shell in components/zv_ui/src/zv_ui_shell.c so the two can
 * be compared line for line. The rules that are copied rather than reinvented:
 *
 *   - mute is a flag beside the level, so unmuting restores the operator's
 *     chosen volume instead of returning to silence;
 *   - the clock is the only chrome that repaints on a timer, on a bounded
 *     period, and input-driven repaints do not reset that cadence;
 *   - a restart goes through an explicit ARE YOU SURE screen and nothing else.
 *
 * What a browser genuinely cannot know is reported as unknown rather than
 * invented: an absent Battery Status API shows a dash, not a fake percentage.
 */

const REPAINT_PERIOD_MS = 60000; // mirrors CONFIG_ZV_UI_REPAINT_PERIOD_MS
const DEFAULT_TZ = 'America/New_York'; // IANA form of the firmware's EST5EDT,M3.2.0,M11.1.0
const LS_TZ = 'zenti-timezone';

const MENU_ITEMS = [
  { id: 'network', label: 'NETWORK' },
  { id: 'volume', label: 'VOLUME' },
  { id: 'mute', label: 'MUTE' },
  { id: 'restart', label: 'RESTART' },
];

const SCREENS = { HOME: 'home', MENU: 'menu', VOLUME: 'volume', CONFIRM: 'confirm' };

function readLS(key, fallback) {
  try { const v = localStorage.getItem(key); return v === null ? fallback : v; }
  catch { return fallback; }
}
function writeLS(key, value) {
  try { localStorage.setItem(key, value); } catch { /* private mode: session only */ }
}

class ZentiShell extends EventTarget {
  /**
   * @param {object} opts
   * @param {AudioEngine|null} opts.audio  owns volume/mute when audio is live
   * @param {() => object} opts.linkInfo    gateway URL + state, shown on NETWORK
   */
  constructor({ audio = null, linkInfo = () => ({}) } = {}) {
    super();
    this.audio = audio;
    this.linkInfo = linkInfo;
    this.screen = SCREENS.HOME;
    this.focus = 0;
    this.notice = '';
    this.noticeUntil = 0;
    this.battery = null;
    this.link = { state: 'offline', bars: 0 };
    this.timeZone = new URLSearchParams(location.search).get('tz') || readLS(LS_TZ, DEFAULT_TZ);
    this._lastClock = -Infinity; // first tick always paints, as on the device
    this._dragging = false;
    this.el = {};
  }

  /* ------------------------------------------------------------- volume */

  /* With no audio engine (demo mode, no credentials) the shell still owns the
   * setting, so the indicator never lies about a state that cannot be applied. */
  get level() { return this.audio ? this.audio.level : (this._level ?? 40); }
  get muted() { return this.audio ? this.audio.muted : !!this._muted; }
  get effectiveVolume() { return this.muted ? 0 : this.level; }

  setLevel(v) {
    const next = Math.max(0, Math.min(100, Math.round(v) || 0));
    if (this.audio) this.audio.setLevel(next); else this._level = next;
    this._emit('volume', this.volumeState);
    return next;
  }

  toggleMute() {
    const next = this.audio ? this.audio.toggleMute() : (this._muted = !this._muted);
    this._emit('volume', this.volumeState);
    return next;
  }

  get volumeState() { return { level: this.level, muted: this.muted, effective: this.effectiveVolume }; }

  /* --------------------------------------------------------------- clock */

  /* An unknown zone must fail visibly. Falling back silently to the machine's
   * own zone is the exact bug a configurable timezone exists to prevent. */
  formatTime(now) {
    try {
      return new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit', timeZone: this.timeZone }).format(now);
    } catch { return '--:--'; }
  }

  formatDate(now) {
    try {
      return new Intl.DateTimeFormat(undefined, { weekday: 'short', month: 'short', day: 'numeric', timeZone: this.timeZone }).format(now);
    } catch { return '—'; }
  }

  setTimeZone(tz) {
    const candidate = (tz || '').trim() || DEFAULT_TZ;
    try { new Intl.DateTimeFormat(undefined, { timeZone: candidate }); }
    catch { this.flash('UNKNOWN TIMEZONE'); return false; }
    this.timeZone = candidate;
    writeLS(LS_TZ, candidate);
    this._lastClock = -Infinity; // repaint immediately, do not wait out the period
    this.tick(performance.now(), true);
    return true;
  }

  /* Called from the animation loop. Repaints the clock at most once per period;
   * every other visual change is driven by an event, not by this timer. */
  tick(now = performance.now(), force = false) {
    if (!force && now - this._lastClock < REPAINT_PERIOD_MS) return false;
    this._lastClock = now;
    // The monotonic clock schedules repaint; wall time supplies the calendar.
    const d = new Date(Date.now());
    this.el.time.textContent = this.formatTime(d);
    this.el.date.textContent = this.formatDate(d);
    this.el.puck.setAttribute('data-tz', this.timeZone);
    return true;
  }

  /* ------------------------------------------------------------- battery */

  async initBattery() {
    /* Battery Status is not universal. Where it is missing we show a dash and
     * say so, rather than parking a made-up percentage on the panel. */
    if (!navigator.getBattery) { this.renderBattery(); return; }
    try {
      const b = await navigator.getBattery();
      const sync = () => { this.battery = { level: b.level, charging: b.charging }; this.renderBattery(); };
      b.addEventListener('levelchange', sync);
      b.addEventListener('chargingchange', sync);
      sync();
    } catch {
      this.battery = null;
      this.renderBattery();
    }
  }

  renderBattery() {
    const el = this.el.batt;
    if (!el) return;
    if (!this.battery) {
      el.textContent = '—';
      el.title = 'Battery level unavailable: this browser does not expose the Battery Status API.';
      return;
    }
    const pct = Math.round(this.battery.level * 100);
    el.textContent = this.battery.charging ? `${pct}% ⚡` : `${pct}%`;
    el.title = this.battery.charging ? `Battery ${pct}%, charging` : `Battery ${pct}%`;
  }

  /* ---------------------------------------------------------------- menu */

  flash(text, ms = 2500) {
    this.notice = text;
    this.noticeUntil = performance.now() + ms;
    this.render();
  }

  go(screen) {
    this.screen = screen;
    this.render();
    this._emit('screen', screen);
  }

  activate(index) {
    const item = MENU_ITEMS[index];
    if (!item) return;
    this.focus = index;
    if (item.id === 'network') this.showNetwork();
    else if (item.id === 'volume') this.go(SCREENS.VOLUME);
    else if (item.id === 'mute') this.flash(this.toggleMute() ? 'MUTED' : 'UNMUTED');
    else if (item.id === 'restart') this.go(SCREENS.CONFIRM);
  }

  showNetwork() {
    const info = this.linkInfo() || {};
    const url = info.url || 'not configured';
    this.flash(`${(info.state || 'offline').toUpperCase()} · ${url}`);
  }

  /* Restart is destructive enough to deserve its own screen, mirroring the
   * puck. Nothing else in this app can trigger it. */
  confirmRestart() {
    this.go(SCREENS.CONFIRM);
  }

  performRestart() {
    /* Teardown and reload are owned by the host so the shell stays free of
     * gateway and audio teardown detail. */
    this._emit('restart', { at: Date.now(), via: 'confirm-screen' });
  }

  /* --------------------------------------------------------------- mount */

  mount() {
    const $ = (id) => document.getElementById(id);
    this.el = {
      puck: $('puck'), time: $('time'), date: $('date'), batt: $('batt-pct'),
      home: $('home-btn'), plate: $('vol-plate'),
      menuScreen: $('menu-screen'), menuRows: $('menu-rows'),
      volScreen: $('vol-screen'), volBar: $('vol-bar'), volFill: $('vol-fill'), volPct: $('vol-pct'),
      volHint: $('vol-hint'),
      confirmScreen: $('confirm-screen'), notice: $('shell-notice'),
    };

    this.el.menuRows.innerHTML = MENU_ITEMS.map((it, i) =>
      `<li role="option" data-index="${i}" aria-selected="${i === this.focus}">${it.label}<span class="row-tail"></span></li>`
    ).join('');

    this.el.home.addEventListener('click', () => this.go(SCREENS.MENU));
    this.el.plate.addEventListener('click', () => this.flash(this.toggleMute() ? 'MUTED' : 'UNMUTED'));
    this.el.menuRows.addEventListener('click', (e) => {
      const li = e.target.closest('li[data-index]');
      if (li) this.activate(Number(li.dataset.index));
    });
    $('confirm-yes').addEventListener('click', () => this.performRestart());
    $('confirm-no').addEventListener('click', () => this.go(SCREENS.MENU));
    this.el.confirmScreen.addEventListener('click', (e) => {
      if (e.target === this.el.confirmScreen) this.go(SCREENS.MENU);
    });

    this._bindSlider();
    this._bindGestures();
    this._bindKeys();

    if (this.audio) this.audio.addEventListener('volume', () => this.render());
    this.render();
    this.initBattery();
    this.tick(performance.now(), true);
  }

  _bindSlider() {
    const bar = this.el.volBar;
    const fromEvent = (e) => {
      const r = bar.getBoundingClientRect();
      return r.width ? ((e.clientX - r.left) / r.width) * 100 : this.level;
    };
    bar.addEventListener('pointerdown', (e) => {
      this._dragging = true;
      bar.setPointerCapture(e.pointerId);
      this.setLevel(fromEvent(e));
    });
    bar.addEventListener('pointermove', (e) => { if (this._dragging) this.setLevel(fromEvent(e)); });
    const stop = () => { this._dragging = false; };
    bar.addEventListener('pointerup', stop);
    bar.addEventListener('pointercancel', stop);
    bar.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowRight' || e.key === 'ArrowUp') { this.setLevel(this.level + 10); e.preventDefault(); }
      if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') { this.setLevel(this.level - 10); e.preventDefault(); }
    });
  }

  /* Gestures are decided on release, never while the finger is still down, so a
   * long press cannot also count as a swipe. */
  _bindGestures() {
    const puck = this.el.puck;
    let start = null;
    puck.addEventListener('pointerdown', (e) => {
      if (e.target.closest('.screen, .plate')) return;
      start = { x: e.clientX, y: e.clientY, t: performance.now() };
    });
    puck.addEventListener('pointerup', (e) => {
      if (!start) return;
      const dx = e.clientX - start.x, dy = e.clientY - start.y, dt = performance.now() - start.t;
      start = null;
      const moved = Math.abs(dx) >= 12 || Math.abs(dy) >= 12;
      if (!moved) {
        if (this.screen === SCREENS.HOME) this.go(SCREENS.MENU);
        else if (this.screen === SCREENS.MENU) this.go(SCREENS.HOME);
        return;
      }
      if (this.screen !== SCREENS.MENU || dt > 600) return;
      if (Math.abs(dx) >= Math.abs(dy)) return; // horizontal means nothing in a vertical list
      this.focus = (this.focus + (dy > 0 ? 1 : -1) + MENU_ITEMS.length) % MENU_ITEMS.length;
      this.render();
    });
  }

  _bindKeys() {
    this.el.puck.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowDown' || e.key === 'ArrowRight') {
        if (this.screen === SCREENS.MENU) { this.focus = (this.focus + 1) % MENU_ITEMS.length; this.render(); e.preventDefault(); }
      } else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') {
        if (this.screen === SCREENS.MENU) { this.focus = (this.focus - 1 + MENU_ITEMS.length) % MENU_ITEMS.length; this.render(); e.preventDefault(); }
      } else if (e.key === 'Enter' || e.key === ' ') {
        if (this.screen === SCREENS.MENU) { this.activate(this.focus); e.preventDefault(); }
      } else if (e.key === 'Escape') {
        if (this.screen !== SCREENS.HOME) this.go(SCREENS.MENU);
      }
    });
  }

  /* -------------------------------------------------------------- render */

  render() {
    const { puck, menuScreen, volScreen, confirmScreen, home, notice } = this.el;
    puck.dataset.screen = this.screen;
    menuScreen.hidden = this.screen !== SCREENS.MENU;
    volScreen.hidden = this.screen !== SCREENS.VOLUME;
    confirmScreen.hidden = this.screen !== SCREENS.CONFIRM;
    home.hidden = this.screen !== SCREENS.HOME;

    if (performance.now() > this.noticeUntil) this.notice = '';
    notice.textContent = this.notice;
    notice.hidden = !this.notice;

    this._renderPlate();

    if (this.screen === SCREENS.MENU) {
      const rows = this.el.menuRows.children;
      for (let i = 0; i < rows.length; i++) {
        const sel = i === this.focus;
        rows[i].setAttribute('aria-selected', String(sel));
        const item = MENU_ITEMS[i];
        const tail = item.id === 'volume' ? `${this.level}%`
          : item.id === 'mute' ? (this.muted ? 'ON' : 'OFF')
          : item.id === 'network' ? (this.link.state || 'offline').toUpperCase()
          : '';
        rows[i].lastElementChild.textContent = tail;
      }
    }

    if (this.screen === SCREENS.VOLUME) {
      this.el.volPct.textContent = `${this.level}%`;
      this.el.volPct.classList.toggle('muted', this.muted);
      this.el.volFill.style.width = `${this.level}%`;
      this.el.volBar.classList.toggle('muted', this.muted);
      this.el.volBar.setAttribute('aria-valuenow', String(this.level));
      this.el.volBar.setAttribute('aria-valuetext', this.muted ? `muted, remembers ${this.level}%` : `${this.level} percent`);
      this.el.volHint.textContent = this.muted ? 'MUTED — LEVEL REMEMBERED' : 'DRAG THE BAR — ESC TO GO BACK';
    }

    this.renderBattery();
  }

  _renderPlate() {
    const plate = this.el.plate;
    plate.classList.toggle('muted', this.muted);
    plate.setAttribute('aria-pressed', String(this.muted));
    plate.setAttribute('aria-label', this.muted ? 'Muted, tap to unmute' : `Volume ${this.level}, tap to mute`);
    if (this.muted) { plate.innerHTML = '<span class="plate-label">MUTE</span>'; return; }
    const filled = Math.round(this.level / 10);
    const segs = Array.from({ length: 10 }, (_, i) =>
      `<span class="seg${i < filled ? '' : ' off'}" style="height:${4 + i * 2}px"></span>`).join('');
    plate.innerHTML = '<span class="body"></span><span class="cone"></span>' + segs;
  }

  setLink(state, bars = 0) {
    this.link = { state, bars };
    this.render();
  }

  _emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }
}

window.ZentiShell = ZentiShell;
window.ZENTI_MENU_ITEMS = MENU_ITEMS;