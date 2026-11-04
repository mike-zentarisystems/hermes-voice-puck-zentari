const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto, createHmac } = require('node:crypto');
const dir = path.resolve(__dirname, '../../pwa/client');
const credential = { device_id: 'pwa-test-0001', generation: 1, secret: 'base64:' + btoa('x'.repeat(32)) };
const uuid = () => webcrypto.randomUUID();
function runtime(files = ['gateway.js']) {
  let timerId = 0; const timers = new Map();
  class Socket {
    static OPEN = 1; static CONNECTING = 0;
    constructor() { this.readyState = 0; this.protocol = 'zentari.voice.v1'; this.sent = []; this.bufferedAmount = 0; }
    send(raw) { this.sent.push(JSON.parse(raw)); }
    open() { this.readyState = 1; this.onopen?.(); }
    close(code = 1000) { this.readyState = 3; this.onclose?.({ code }); }
  }
  const ctx = vm.createContext({ window: {}, EventTarget, CustomEvent, WebSocket: Socket,
    crypto: webcrypto, TextEncoder, URL, btoa, atob, ArrayBuffer, Uint8Array, DataView,
    setTimeout(fn, ms) { timers.set(++timerId, { fn, ms }); return timerId; },
    clearTimeout(id) { timers.delete(id); }, localStorage: { getItem: () => null, setItem() {} },
    navigator: { mediaDevices: {} }, console });
  for (const file of files) vm.runInContext(fs.readFileSync(path.join(dir, file), 'utf8'), ctx);
  const gateway = new ctx.window.GatewayClient('wss://localhost:8443/', credential);
  return { ctx, gateway, timers, Socket };
}
function ready(r) {
  const { gateway, Socket } = r;
  gateway.ws = new Socket(); gateway.ws.readyState = 1; gateway._stopped = false;
  gateway._authed = true; gateway.sessionId = uuid(); return gateway;
}
function output(g, type, fields = {}, correlation = g.turn?.id) {
  g._onMessage({ data: JSON.stringify({ v: 1, type, device_id: credential.device_id,
    session_id: g.sessionId, correlation_id: correlation, seq: g.outputSeq + 1, ...fields }) });
}
function thinking(g) { g.startCapture(); g.sendAudio(new Uint8Array([0, 0]).buffer); g.stopCapture(); }
const pcm = { encoding: 'pcm_s16le', sample_rate: 16000, channels: 1 };

test('real WebCrypto proof matches gateway HMAC bytes and base64url; acceptance requires proof', async () => {
  const r = runtime(); const g = r.gateway;
  assert.equal(g.startCapture(), false); assert.equal(g.connect(), true); g.ws.open();
  const challenge = { v: 1, type: 'auth.challenge', challenge_id: uuid(), nonce: 'a'.repeat(43),
    hello_id: g.helloId, expires_ms: Date.now() + 10000 };
  await g._onChallenge(challenge);
  const proof = g.ws.sent.at(-1);
  const canonical = `zv1\n${challenge.challenge_id}\n${challenge.nonce}\n${credential.device_id}\n1\n${g.helloId}`;
  assert.equal(proof.proof, createHmac('sha256', Buffer.alloc(32, 'x')).update(canonical).digest('base64url'));
  g._onControl({ v: 1, type: 'session.accepted', device_id: credential.device_id,
    correlation_id: g.helloId, session_id: uuid(), expires_ms: Date.now() + 900000 });
  assert.equal(g.ready, true); assert.equal(g.startCapture(), true);
  assert.equal(g.ws.sent.at(-1).type, 'audio.input.start');
});
test('origin mismatch, expired credentials, malformed secret and unsolicited acceptance fail closed', () => {
  const r = runtime(); const g = r.gateway;
  for (const extra of [{ gateway_origin: 'wss://other/' }, { expires_at: 1 }, { secret: 'x'.repeat(32) }, { generation: 0 }]) {
    assert.throws(() => r.ctx.window.validateVoiceCredential(g.url, { ...credential, ...extra }));
  }
  g.connect(); g.ws.open(); g._onControl({ v: 1, type: 'session.accepted', device_id: credential.device_id,
    correlation_id: g.helloId, session_id: uuid(), expires_ms: Date.now() + 10000 });
  assert.equal(g.ready, false); assert.equal(g.startCapture(), false); assert.equal(r.timers.size, 0);
});
test('async proof completion after disconnect cannot send into another socket', async () => {
  const r = runtime(); const g = r.gateway; g.connect(); g.ws.open();
  const old = g.ws;
  const operation = g._onChallenge({ v: 1, hello_id: g.helloId, challenge_id: uuid(), nonce: 'a'.repeat(43), expires_ms: Date.now() + 10000 });
  g.disconnect(); g.connect(); g.ws.open(); await operation;
  assert.equal(old.sent.length, 1); assert.equal(g.ws.sent.length, 1); g.disconnect();
});
test('one turn, PCM JSON framing, global sequence and normal playback drain', () => {
  const g = ready(runtime()); let frames = 0;
  g.addEventListener('audio', () => frames++);
  thinking(g); assert.equal(g.startCapture(), false);
  assert.deepEqual(g.ws.sent.map(x => x.type), ['audio.input.start', 'audio.input.chunk', 'audio.input.stop']);
  assert.deepEqual(g.ws.sent.map(x => x.seq), [1, 2, 3]);
  assert.equal(g.ws.sent[1].data, 'AAA=');
  output(g, 'transcript.final', { text: 'fixture' }); output(g, 'assistant.thinking'); output(g, 'assistant.text', { text: 'fixture answer' });
  output(g, 'audio.output.start', { format: pcm }); output(g, 'audio.output.chunk', { data: 'AAA=' }); output(g, 'audio.output.stop');
  assert.equal(frames, 1); assert.equal(g.turn.state, 'draining'); assert.equal(g.startCapture(), false);
  g.completePlayback(); assert.equal(g.turn, null); assert.equal(g.startCapture(), true); assert.equal(g.ws.sent.at(-1).seq, 4);
});
test('cancel drops late speech while consuming sequence; next correlation recovers', () => {
  const g = ready(runtime()); let frames = 0; g.addEventListener('audio', () => frames++);
  thinking(g); const old = g.turn.id; g.cancel(); thinking(g);
  output(g, 'audio.output.start', { format: pcm }, old); output(g, 'audio.output.chunk', { data: 'AAA=' }, old);
  assert.equal(g.turn.state, 'thinking'); assert.equal(frames, 0);
  output(g, 'audio.output.start', { format: pcm }); output(g, 'audio.output.chunk', { data: 'AAA=' });
  assert.equal(frames, 1);
});
test('malformed identity, skipped sequence, binary and oversized frames invalidate session', () => {
  for (const mutate of [m => { m.session_id = uuid(); }, m => { m.seq = 3; }, m => { m.extra = 1; }]) {
    const g = ready(runtime()); thinking(g);
    const m = { v: 1, type: 'assistant.thinking', device_id: credential.device_id,
      session_id: g.sessionId, correlation_id: g.turn.id, seq: 1 }; mutate(m);
    g._onMessage({ data: JSON.stringify(m) }); assert.equal(g.ready, false); assert.equal(g.turn, null);
  }
  for (const data of [new ArrayBuffer(2), 'x'.repeat(4097), 'null', '[]']) {
    const g = ready(runtime()); thinking(g); g._onMessage({ data }); assert.equal(g.ready, false);
  }
});
test('invalid audio, output order, empty output and byte limits cancel without playing', () => {
  for (const data of ['AA==', '!!!!', btoa('x'.repeat(1026))]) {
    const g = ready(runtime()); thinking(g); output(g, 'audio.output.start', { format: pcm });
    output(g, 'audio.output.chunk', { data }); assert.equal(g.turn, null);
  }
  const g = ready(runtime()); thinking(g); output(g, 'audio.output.chunk', { data: 'AAA=' }); assert.equal(g.turn, null);
  thinking(g); output(g, 'audio.output.start', { format: pcm }); output(g, 'audio.output.stop'); assert.equal(g.turn, null);
  thinking(g); output(g, 'audio.output.start', { format: pcm }); g.turn.outputBytes = 1920000;
  output(g, 'audio.output.chunk', { data: 'AAA=' }); assert.equal(g.turn, null);
  g.startCapture(); g.turn.inputBytes = 480000; assert.equal(g.sendAudio(new ArrayBuffer(2)), false); assert.equal(g.turn, null);
});
test('transport backpressure and turn deadline stop work; disconnect never replays audio', () => {
  const r = runtime(); const g = ready(r); thinking(g);
  const timeout = [...r.timers.values()].find(x => x.ms === 150000); timeout.fn(); assert.equal(g.turn, null);
  g.startCapture(); g.ws.bufferedAmount = 20000; g.sendAudio(new ArrayBuffer(2)); assert.equal(g.ready, false);
  const r2 = runtime(); const g2 = ready(r2); thinking(g2); g2.ws.onclose = ev => g2._onClose(ev); g2.ws.close();
  assert.equal(g2.turn, null); assert.equal(g2.ready, false); assert.equal(g2.sendAudio(new ArrayBuffer(2)), false);
  assert.equal(r2.timers.size, 1); g2.disconnect(); assert.equal(r2.timers.size, 0);
});
function processor(rate, credit = true) {
  const messages = []; let instance;
  class Base { constructor() { this.port = { postMessage: msg => { messages.push(msg); if (msg.type === 'pcm' && credit) instance.port.onmessage({ data: { type: 'credit' } }); } }; } }
  const ctx = vm.createContext({ AudioWorkletProcessor: Base, sampleRate: rate, registerProcessor(name, klass) { instance = new klass(); } });
  vm.runInContext(fs.readFileSync(path.join(dir, 'capture-worklet.js'), 'utf8'), ctx);
  return { instance, messages };
}
test('worklet resamples 44.1/48 kHz to exactly 16 kHz with little-endian bounded frames', () => {
  for (const rate of [44100, 48000]) {
    const { instance, messages } = processor(rate);
    for (let offset = 0; offset < rate; offset += 128) instance.process([[new Float32Array(Math.min(128, rate - offset)).fill(0.5)]]);
    instance.port.onmessage({ data: { type: 'flush' } });
    const frames = messages.filter(m => m.type === 'pcm');
    assert.equal(frames.reduce((n, m) => n + m.buffer.byteLength / 2, 0), 16000);
    assert.ok(frames.every(m => m.buffer.byteLength <= 1024 && m.buffer.byteLength % 2 === 0));
    assert.ok(Math.abs(new DataView(frames.at(-1).buffer).getInt16(0, true) - 16384) <= 1);
    assert.equal(messages.at(-1).type, 'flushed');
  }
});
test('FIR suppresses frequencies above output Nyquist instead of aliasing into speech', () => {
  function rms(frequency) {
    const { instance, messages } = processor(48000);
    for (let offset = 0; offset < 48000; offset += 128) {
      const samples = new Float32Array(Math.min(128, 48000 - offset));
      for (let i = 0; i < samples.length; i++) samples[i] = Math.sin(2 * Math.PI * frequency * (offset + i) / 48000) * 0.5;
      instance.process([[samples]]);
    }
    const values = messages.filter(m => m.type === 'pcm').flatMap(m => Array.from(new Int16Array(m.buffer))).slice(1000);
    return Math.sqrt(values.reduce((sum, x) => sum + x * x, 0) / values.length);
  }
  assert.ok(rms(10000) < rms(1000) / 30);
});
test('worklet stops at 15 seconds and bounds main-thread backlog to eight frames', () => {
  const r = processor(48000);
  for (let i = 0; i < 6000; i++) r.instance.process([[new Float32Array(128)]]);
  assert.equal(r.messages.filter(m => m.type === 'pcm').reduce((n, m) => n + m.buffer.byteLength, 0), 480000);
  assert.equal(r.messages.filter(m => m.type === 'limit').length, 1);
  const blocked = processor(48000, false);
  for (let i = 0; i < 500; i++) blocked.instance.process([[new Float32Array(128)]]);
  assert.equal(blocked.messages.filter(m => m.type === 'pcm').length, 8);
  assert.equal(blocked.messages.filter(m => m.type === 'overflow').length, 1);
});
function player(rate) {
  const messages = []; let instance;
  class Base { constructor() { this.port = { postMessage: msg => messages.push(msg) }; } }
  const ctx = vm.createContext({ AudioWorkletProcessor: Base, sampleRate: rate, registerProcessor(name, klass) { instance = new klass(); } });
  vm.runInContext(fs.readFileSync(path.join(dir, 'playback-worklet.js'), 'utf8'), ctx);
  const send = (samples, gen = 0) => {
    const view = new DataView(new ArrayBuffer(samples.length * 2));
    samples.forEach((x, i) => view.setInt16(i * 2, Math.round(x * 32767), true));
    instance.port.onmessage({ data: { type: 'pcm', gen, buffer: view.buffer } });
  };
  const render = blocks => { const out = []; for (let b = 0; b < blocks; b++) { const o = new Float32Array(128); instance.process([], [[o]]); out.push(...o); } return out; };
  return { instance, messages, send, render };
}
test('playback worklet resamples 512-sample chunks without boundary artifacts', () => {
  for (const rate of [44100, 48000]) {
    const p = player(rate), tone = n => 0.5 * Math.sin(2 * Math.PI * 1000 * n / 16000);
    for (let start = 0; start < 8000; start += 512) p.send(Array.from({ length: 512 }, (_, i) => tone(start + i)));
    const out = p.render(Math.ceil(rate * 0.4 / 128)).slice(Math.round(rate * 0.05), Math.round(rate * 0.35));
    let c = 0, q = 0;
    out.forEach((y, i) => { c += y * Math.cos(2 * Math.PI * 1000 * i / rate); q += y * Math.sin(2 * Math.PI * 1000 * i / rate); });
    c *= 2 / out.length; q *= 2 / out.length;
    const residual = Math.sqrt(out.reduce((sum, y, i) => sum + (y - c * Math.cos(2 * Math.PI * 1000 * i / rate) - q * Math.sin(2 * Math.PI * 1000 * i / rate)) ** 2, 0) / out.length);
    assert.ok(Math.abs(Math.hypot(c, q) - 0.5) < 0.01, `amplitude at ${rate}`);
    assert.ok(residual < 0.005, `residual ${residual} at ${rate}`);
  }
});
test('playback worklet waits 200 ms before speech, drains once and bounds its queue', () => {
  const p = player(48000);
  p.send(new Array(3000).fill(0.25)); assert.ok(p.render(40).every(y => y === 0));
  p.send(new Array(512).fill(0.25)); assert.ok(p.render(10).some(y => y !== 0));
  p.instance.port.onmessage({ data: { type: 'end', gen: 0 } }); p.render(400);
  assert.equal(JSON.stringify(p.messages), JSON.stringify([{ type: 'drained', gen: 0 }]));
  const q = player(48000); for (let i = 0; i < 70; i++) q.send(new Array(512).fill(0.1));
  assert.equal(JSON.stringify(q.messages), JSON.stringify([{ type: 'overflow', gen: 0 }]));
  q.instance.port.onmessage({ data: { type: 'stop', gen: 1 } }); q.send(new Array(4000).fill(0.1), 0);
  assert.ok(q.render(40).every(y => y === 0), 'stale generation audio ignored after stop');
});
function audioRuntime() {
  const r = runtime(['gateway.js', 'audio.js']); ready(r);
  const track = { stops: 0, stop() { this.stops++; } }, players = [];
  const node = () => ({ connect() {}, disconnect() {} });
  r.ctx.window.AudioContext = class {
    constructor() { this.state = 'running'; this.currentTime = 0; this.destination = {}; this.audioWorklet = { addModule: async () => {} }; }
    createAnalyser() { return { ...node(), getFloatTimeDomainData(a) { a.fill(0); } }; }
    createGain() { return { ...node(), gain: { value: 0 } }; }
    resume() { return Promise.resolve(); } close() { this.state = 'closed'; return Promise.resolve(); }
    createMediaStreamSource() { return node(); }
  };
  r.ctx.AudioWorkletNode = class {
    constructor(ctx, name) {
      Object.assign(this, node()); this.posted = []; if (name === 'zenti-playback') players.push(this);
      this.port = { close() {}, postMessage: data => {
        this.posted.push(data); if (data.type === 'flush') this.port.onmessage({ data: { type: 'flushed' } });
      } };
    }
  };
  r.ctx.navigator.mediaDevices.getUserMedia = async () => ({ getTracks: () => [track] });
  const engine = new r.ctx.window.AudioEngine(r.gateway);
  const player = () => players[players.length - 1];
  return { ...r, engine, track, player };
}
test('release during microphone permission prompt stops late stream and never starts a turn', async () => {
  const r = audioRuntime(); let permit;
  r.ctx.navigator.mediaDevices.getUserMedia = () => new Promise(resolve => { permit = resolve; });
  const pending = r.engine.startCapture(); await new Promise(resolve => setImmediate(resolve));
  await r.engine.stopCapture(); permit({ getTracks: () => [r.track] }); await pending;
  assert.equal(r.track.stops, 1); assert.equal(r.gateway.turn, null); assert.equal(r.engine.capturing, false);
});
test('capture flush sends final bytes before stop and releases all microphone resources', async () => {
  const r = audioRuntime(); assert.equal(await r.engine.startCapture(), true);
  r.engine.worklet.port.onmessage({ data: { type: 'pcm', buffer: new ArrayBuffer(1024) } });
  await r.engine.stopCapture(); assert.equal(r.track.stops, 1); assert.equal(r.engine.worklet, null);
  assert.deepEqual(r.gateway.ws.sent.map(m => m.type), ['audio.input.start', 'audio.input.chunk', 'audio.input.stop']);
});
test('playback streams frames to one player; player overflow cancels and stops speech', async () => {
  const r = audioRuntime(); await r.engine.init(); thinking(r.gateway);
  output(r.gateway, 'audio.output.start', { format: pcm });
  for (let i = 0; i < 20; i++) output(r.gateway, 'audio.output.chunk', { data: 'AAA=' });
  const pcmFrames = r.player().posted.filter(m => m.type === 'pcm');
  assert.equal(pcmFrames.length, 20); assert.ok(pcmFrames.every(m => m.buffer.byteLength === 2 && m.gen === r.engine.playGen));
  assert.equal(r.engine.playing, true);
  r.player().port.onmessage({ data: { type: 'overflow', gen: r.engine.playGen } });
  assert.equal(r.gateway.turn, null); assert.equal(r.engine.playing, false);
  assert.equal(r.player().posted[r.player().posted.length - 1].type, 'stop');
});
test('normal output stop retains turn until the current player generation drains', async () => {
  const r = audioRuntime(); await r.engine.init(); thinking(r.gateway);
  output(r.gateway, 'audio.output.start', { format: pcm }); output(r.gateway, 'audio.output.chunk', { data: 'AAA=' }); output(r.gateway, 'audio.output.stop');
  assert.equal(r.gateway.turn.state, 'draining'); assert.equal(r.player().posted[r.player().posted.length - 1].type, 'end');
  r.player().port.onmessage({ data: { type: 'drained', gen: r.engine.playGen - 1 } }); assert.equal(r.gateway.turn.state, 'draining');
  r.player().port.onmessage({ data: { type: 'drained', gen: r.engine.playGen } }); assert.equal(r.gateway.turn, null);
});

test('capture limit renews bounded session history rather than permanently disabling voice', () => {
  const r = runtime(); const g = ready(r); g.ws.onclose = ev => g._onClose(ev);
  for (let i = 0; i < 255; i++) g.retired.add(uuid());
  assert.equal(g.startCapture(), false); assert.equal(g.ready, false); assert.equal(g.turn, null);
  assert.equal(r.timers.size, 1); g.disconnect();
});
test('credential expiry bounds even a longer accepted session', () => {
  const r = runtime(); const g = r.gateway;
  g.credential.expires_at = Math.floor(Date.now() / 1000) + 60;
  g.ws = new r.Socket(); g.ws.readyState = 1; g._proofSent = true; g.helloId = uuid();
  g._onControl({ v: 1, type: 'session.accepted', device_id: credential.device_id,
    session_id: uuid(), correlation_id: g.helloId, expires_ms: Date.now() + 900000 });
  assert.ok([...r.timers.values()].find(x => x.ms <= 60000));
});

test('mic mute invalidates permission races and never sends a partial turn', async () => {
  const r = audioRuntime(); let permit;
  r.ctx.navigator.mediaDevices.getUserMedia = () => new Promise(resolve => { permit = resolve; });
  const pending = r.engine.startCapture(); await new Promise(resolve => setImmediate(resolve));
  r.engine.setMicMuted(true); permit({ getTracks: () => [r.track] }); await pending;
  assert.equal(r.track.stops, 1); assert.equal(r.gateway.turn, null);
  assert.equal(await r.engine.startCapture(), false);
  r.engine.setMicMuted(false); r.ctx.navigator.mediaDevices.getUserMedia = async () => ({ getTracks: () => [r.track] });
  await r.engine.startCapture(); r.engine.worklet.port.onmessage({ data: { type: 'pcm', buffer: new ArrayBuffer(1024) } });
  r.engine.setMicMuted(true);
  assert.equal(r.gateway.turn, null); assert.ok(!r.gateway.ws.sent.some(m => m.type === 'audio.input.stop'));
});
test('hands-free silence submits nothing; voiced pre-roll starts once and silence ends utterance', async () => {
  const r = audioRuntime(); await r.engine.startCapture(true); const node = r.engine.worklet;
  for (let i = 0; i < 8; i++) r.engine._captureFrame(new ArrayBuffer(1024), node);
  assert.equal(r.gateway.turn, null);
  const loud = new Int16Array(512); loud.fill(9000);
  for (let i = 0; i < 3; i++) r.engine._captureFrame(loud.buffer, node);
  assert.equal(r.gateway.turn.state, 'listening');
  assert.equal(r.gateway.ws.sent.filter(m => m.type === 'audio.input.start').length, 1);
  for (let i = 0; i < 23; i++) r.engine._captureFrame(new ArrayBuffer(1024), node);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(r.gateway.turn.state, 'thinking'); assert.equal(r.engine.stream, null);
});
test('microphone selection aborts current capture; silent replies still drain', async () => {
  const r = audioRuntime(); await r.engine.startCapture(); r.engine.setInputDevice('device-2');
  assert.equal(r.engine.stream, null); assert.equal(r.gateway.turn, null); assert.equal(r.engine.inputDevice, 'device-2');
  r.engine.voiceReplies = false; thinking(r.gateway);
  output(r.gateway, 'audio.output.start', { format: pcm }); output(r.gateway, 'audio.output.chunk', { data: 'AAA=' }); output(r.gateway, 'audio.output.stop');
  assert.equal(r.gateway.turn, null); assert.equal(r.player()?.posted.filter(m => m.type === 'pcm').length ?? 0, 0);
});
test('conversation restoration requires an authenticated matching control and text uses a real text event', () => {
  const r = runtime(); const g = ready(r); let detail;
  g.addEventListener('conversation', e => { detail = e.detail; });
  assert.equal(g.sendText('hello'), false); g.openConversation();
  const id = uuid();
  output(g, 'conversation.ready', { conversation_id: id, principal: 'owner', capabilities: ['text', 'context', 'reset'] }, g.pendingContext);
  assert.equal(detail.same, false); assert.equal(g.contextReady, true);
  g.openConversation(); output(g, 'conversation.ready', { conversation_id: id, principal: 'owner', capabilities: ['text'] }, g.pendingContext);
  assert.equal(detail.same, true); assert.equal(g.sendText('hello'), true);
  assert.equal(g.ws.sent.at(-1).type, 'text.input'); g.cancel();
  g.openConversation(true); output(g, 'conversation.ready', { conversation_id: uuid(), principal: 'other', capabilities: ['text'] }, g.pendingContext);
  assert.equal(detail.same, false); g.disconnect(); assert.equal(g.contextReady, false);
});
