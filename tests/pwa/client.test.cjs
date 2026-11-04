/* Browser logic under deterministic clocks and transports; no real gateway. */
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
const client = path.join(root, 'pwa/client');
class Events {
  constructor() { this.listeners = new Map(); }
  addEventListener(name, fn) {
    const list = this.listeners.get(name) || [];
    list.push(fn); this.listeners.set(name, list);
  }
  dispatchEvent(ev) { (this.listeners.get(ev.type) || []).forEach(fn => fn(ev)); }
}
class Event { constructor(type, opts = {}) { this.type = type; this.detail = opts.detail; } }
const wall = Date.parse('2026-10-07T21:00:00Z');
class Clock extends Date { static now() { return wall; } }
function runtime(file) {
  const timers = new Map(); let timerId = 0;
  class Socket {
    static CONNECTING = 0; static OPEN = 1;
    constructor() { this.readyState = 0; this.sent = []; }
    send(text) { this.sent.push(JSON.parse(text)); }
    close() { this.readyState = 3; if (this.onclose) this.onclose({ code: 1000 }); }
  }
  const ctx = vm.createContext({
    window: {}, EventTarget: Events, CustomEvent: Event, Date: Clock, Intl,
    URL, TextEncoder, btoa, atob, ArrayBuffer, Uint8Array, URLSearchParams, location: { search: '' }, performance: { now: () => 12000 },
    localStorage: { getItem: () => null, setItem() {} },
    WebSocket: Socket, crypto: { randomUUID: () => '11111111-2222-4333-8444-555555555555' },
    setTimeout(fn, delay) { timers.set(++timerId, { fn, delay }); return timerId; },
    clearTimeout(id) { timers.delete(id); }, console,
  });
  vm.runInContext(fs.readFileSync(path.join(client, file), 'utf8'), ctx);
  return { ctx, timers, Socket };
}
test('calendar uses wall time while repaint scheduling stays monotonic', () => {
  const { ctx } = runtime('shell.js');
  const shell = new ctx.window.ZentiShell();
  shell.el = { time: {}, date: {}, puck: { setAttribute() {} } };
  assert.equal(shell.tick(12000, true), true);
  assert.equal(shell.el.time.textContent, shell.formatTime(new Clock(wall)));
  assert.equal(shell.el.date.textContent, shell.formatDate(new Clock(wall)));
  assert.equal(shell.tick(12001), false);
  assert.equal(shell.tick(72000), true);
});
test('explicit disconnect cancels retries; network loss still reconnects', () => {
  const { ctx, timers } = runtime('gateway.js');
  const gateway = new ctx.window.GatewayClient('wss://example', { device_id: 'pwa-lab-0001', generation: 1, secret: 'base64:' + btoa('x'.repeat(32)) });
  gateway.connect();
  gateway.ws.close(); assert.equal(timers.size, 1);
  gateway.disconnect(); assert.equal(timers.size, 0);
  gateway.connect(); gateway.disconnect(); assert.equal(timers.size, 0);
});
test('approval responses exactly follow the shared schema and require auth', () => {
  const { ctx, Socket } = runtime('gateway.js');
  const gateway = new ctx.window.GatewayClient('wss://example', { device_id: 'pwa-lab-0001', generation: 1, secret: 'base64:' + btoa('x'.repeat(32)) });
  gateway.ws = new Socket(); gateway.ws.readyState = Socket.OPEN;
  const id = '11111111-2222-4333-8444-555555555555';
  assert.equal(gateway.respondApproval(true, id), false);
  assert.equal(gateway.ws.sent.length, 0);
  gateway._authed = true;
  for (const approved of [true, false]) {
    assert.equal(gateway.respondApproval(approved, id), true);
    const msg = gateway.ws.sent.at(-1);
    const schema = JSON.parse(fs.readFileSync(path.join(root, 'protocol/v1/schemas/approval.response.schema.json')));
    for (const key of schema.required) assert.ok(key in msg, key);
    for (const key of Object.keys(msg)) assert.ok(key in schema.properties, key);
    assert.equal(msg.decision, approved ? 'approved' : 'denied');
    assert.equal(msg.device_id, 'pwa-lab-0001');
    assert.equal(msg.decided_at, wall / 1000);
  }
});
test('pre-auth approval and binary frames never reach consumers', () => {
  const { ctx, Socket } = runtime('gateway.js');
  const gateway = new ctx.window.GatewayClient('wss://example', { device_id: 'pwa-lab-0001', generation: 1, secret: 'base64:' + btoa('x'.repeat(32)) });
  let approvals = 0, audio = 0;
  gateway.addEventListener('approval', () => approvals++);
  gateway.addEventListener('audio', () => audio++);
  gateway._onControl({ type: 'approval.request', v: 1 });
  assert.equal(approvals, 0); assert.equal(audio, 0);
  gateway.ws = new Socket(); gateway.ws.readyState = Socket.OPEN;
  gateway._authed = true;
  gateway._onControl({ type: 'approval.request', v: 1 });
  assert.equal(approvals, 1); assert.equal(audio, 0);
  gateway._onMessage({ data: new ArrayBuffer(2) }); assert.equal(audio, 0);
});
test('volume remains remembered across mute and unmute', () => {
  const { ctx } = runtime('audio.js');
  const engine = new ctx.window.AudioEngine(new Events());
  engine.setLevel(70); engine.setMuted(true); assert.equal(engine.effectiveVolume, 0);
  engine.setLevel(55); engine.setMuted(false); assert.equal(engine.effectiveVolume, 55);
});
test('every JavaScript and inline script parses; offline assets exist', () => {
  for (const file of ['app.js', 'gateway.js', 'shell.js', 'audio.js', 'sw.js', 'voice.js', 'capture-worklet.js']) {
    new vm.Script(fs.readFileSync(path.join(client, file), 'utf8'), { filename: file });
  }
  const html = fs.readFileSync(path.join(client, 'index.html'), 'utf8');
  for (const match of html.matchAll(/<script>([\s\S]*?)<\/script>/g)) new vm.Script(match[1]);
  const worker = fs.readFileSync(path.join(client, 'sw.js'), 'utf8');
  for (const match of worker.matchAll(/'\.\/([^']+)'/g)) assert.ok(fs.existsSync(path.join(client, match[1])));
  assert.match(html, /<button\b(?=[^>]*\bid="talk")(?=[^>]*\bdisabled)[^>]*>/);
  assert.ok(!html.includes("setTimeout(() => setState('speaking')"));
});
test('offline response survives and cache refresh finishes within event lifetime', async () => {
  const handlers = {}, updates = [];
  const cached = { name: 'cached' };
  const cache = { match: async () => cached, put: async (req, fresh) => updates.push(fresh.name) };
  let online = true;
  const ctx = vm.createContext({
    self: { location: { origin: 'https://example' }, addEventListener: (k, fn) => { handlers[k] = fn; } },
    caches: { open: async () => cache }, URL, Request, Response, console,
    fetch: async () => { if (!online) throw Error('offline'); return { status: 200, type: 'basic', clone: () => ({ name: 'fresh' }) }; },
  });
  vm.runInContext(fs.readFileSync(path.join(client, 'sw.js'), 'utf8'), ctx);
  async function request() {
    let response; const work = [];
    handlers.fetch({ request: new Request('https://example/shell.js'),
      waitUntil: promise => work.push(promise), respondWith: promise => { response = promise; } });
    assert.equal(work.length, 1); // lifetime registered before any await
    assert.equal(await response, cached);
    await Promise.all(work);
  }
  await request(); assert.deepEqual(updates, ['fresh']);
  online = false; await request(); assert.deepEqual(updates, ['fresh']);
});

test('cache quota failure still serves a successful uncached network response', async () => {
  const handlers = {}, work = [];
  const fresh = { status: 200, type: 'basic', clone: () => ({}) };
  const cache = { match: async () => undefined, put: async () => { throw Error('quota exceeded'); } };
  const ctx = vm.createContext({
    self: { location: { origin: 'https://example' }, addEventListener: (k, fn) => { handlers[k] = fn; } },
    caches: { open: async () => cache }, URL, Request, Response,
    console: { warn() {} }, fetch: async () => fresh,
  });
  vm.runInContext(fs.readFileSync(path.join(client, 'sw.js'), 'utf8'), ctx);
  let response;
  handlers.fetch({ request: new Request('https://example/shell.js'),
    waitUntil: promise => work.push(promise), respondWith: promise => { response = promise; } });
  assert.equal(await response, fresh);
  await Promise.all(work);
});
