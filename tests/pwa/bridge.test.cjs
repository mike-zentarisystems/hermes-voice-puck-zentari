/* Deterministic bridge-client regressions; no microphone, TLS or Hermes acceptance. */
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.resolve(__dirname, '../../pwa/client/gateway.js'), 'utf8');

class Events {
  constructor() { this.listeners = new Map(); }
  addEventListener(type, fn) { const listeners = this.listeners.get(type) || []; listeners.push(fn); this.listeners.set(type, listeners); }
  dispatchEvent(event) { for (const fn of this.listeners.get(event.type) || []) fn(event); }
}
class Event { constructor(type, options) { this.type = type; this.detail = options.detail; } }
function runtime(url = 'wss://pwa.example/gateway', location = { protocol: 'https:', origin: 'https://pwa.example' }) {
  const timers = new Map(); let timer = 0, uuid = 0;
  class Socket {
    static OPEN = 1;
    constructor() { this.readyState = 0; this.protocol = 'zentari.voice.v1'; this.sent = []; }
    send(raw) { this.sent.push(JSON.parse(raw)); }
    open() { this.readyState = 1; this.onopen(); }
    close(code = 1000) { this.readyState = 3; this.onclose?.({ code }); }
  }
  const window = {};
  new Function('window', 'location', 'WebSocket', 'EventTarget', 'CustomEvent', 'crypto',
    'setTimeout', 'clearTimeout', source)(window, location, Socket, Events, Event,
    { randomUUID: () => '00000000-0000-4000-8000-' + String(++uuid).padStart(12, '0') },
    (fn, ms) => { timers.set(++timer, { fn, ms }); return timer; }, id => timers.delete(id));
  const client = new window.GatewayClient(url, { device_id: 'pwa-fixture-0001' }, true);
  function accepted() {
    client.connect(); client.ws.open();
    client._onControl({ v: 1, type: 'session.accepted', device_id: 'pwa-fixture-0001',
      session_id: '11111111-1111-4111-8111-111111111111', correlation_id: client.helloId, expires_ms: Date.now() + 900000 });
  }
  return { client, accepted, timers };
}
test('bridge hello contains no key, generation, nonce or proof; accepted session enables PCM', () => {
  const r = runtime(); r.accepted(); const c = r.client;
  assert.deepEqual(Object.keys(c.ws.sent[0]).sort(), ['firmware', 'id', 'type', 'v']);
  assert.equal(c.ready, true); assert.equal(c.startCapture(), true);
  assert.equal(c.sendAudio(new Uint8Array([0, 0]).buffer), true);
  assert.equal(c.stopCapture(), true);
  assert.deepEqual(c.ws.sent.slice(1).map(x => x.type), ['audio.input.start', 'audio.input.chunk', 'audio.input.stop']);
});
test('bridge rejects cross-origin, insecure and query-bearing endpoints', () => {
  for (const url of ['wss://evil.example/gateway', 'ws://pwa.example/gateway', 'wss://pwa.example/gateway?token=x']) {
    assert.equal(runtime(url).client.connect(), false);
  }
  assert.equal(runtime('wss://pwa.example/gateway', { protocol: 'http:', origin: 'http://pwa.example' }).client.connect(), false);
});
test('bridge refuses leaked upstream challenge and mismatched acceptance', () => {
  const c = runtime().client; c.connect(); c.ws.open();
  c._onControl({ v: 1, type: 'auth.challenge' }); assert.equal(c.ready, false);
  const r = runtime(); r.accepted(); r.client._onControl({ v: 1, type: 'session.accepted' });
  assert.equal(r.client.ready, false);
});
test('bridge never grants approval authority', () => {
  const r = runtime(); r.accepted();
  assert.equal(r.client.respondApproval(true, '11111111-1111-4111-8111-111111111111'), false);
});
test('cancel consumes late audio without playback and next turn remains usable', () => {
  const r = runtime(); r.accepted(); const c = r.client; let played = 0;
  c.addEventListener('audio', () => played++);
  c.startCapture(); c.sendAudio(new Uint8Array([0, 0]).buffer); c.stopCapture();
  const correlation = c.turn.id; c.cancel(); c.startCapture();
  for (const [type, fields] of [['audio.output.start', { format: { encoding: 'pcm_s16le', sample_rate: 16000, channels: 1 } }],
                                ['audio.output.chunk', { data: 'AAA=' }]]) {
    c._onMessage({ data: JSON.stringify({ v: 1, type, device_id: 'pwa-fixture-0001',
      session_id: c.sessionId, correlation_id: correlation, seq: c.outputSeq + 1, ...fields }) });
  }
  assert.equal(played, 0); assert.equal(c.turn.state, 'listening'); assert.equal(c.outputSeq, 2);
});
test('disconnect clears voice and retry; reconnect uses fresh hello', () => {
  const r = runtime(); r.accepted(); const c = r.client; const id = c.helloId;
  c.startCapture(); c.disconnect(); assert.equal(c.turn, null); assert.equal(r.timers.size, 0);
  c.connect(); c.ws.open(); assert.notEqual(c.helloId, id);
});
test('reconnect reopens conversation handle; session expiry clears capture', () => {
  const r = runtime(); r.accepted(); const c = r.client;
  c.conversationId = '22222222-2222-4222-8222-222222222222'; c.useContext = true;
  c.disconnect(); r.accepted();
  assert.equal(c.ws.sent.at(-1).type, 'conversation.open');
  assert.equal(c.ws.sent.at(-1).conversation_id, c.conversationId);
  c.startCapture();
  const expiry = [...r.timers.values()].find(x => x.ms > 10000 && x.ms !== 150000);
  expiry.fn(); assert.equal(c.ready, false); assert.equal(c.turn, null);
});
