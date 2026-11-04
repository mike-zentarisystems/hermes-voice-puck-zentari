const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function fixture(Image) {
  const ctx = vm.createContext({ window: {}, Image });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../../pwa/client/avatar.js'), 'utf8'), ctx);
  const image = { src: '', hidden: false }, properties = {};
  const meter = { style: { setProperty(key, value) { properties[key] = value; } } };
  return { image, properties, view: new ctx.window.AvatarView(image, () => {}, meter) };
}
const tick = () => new Promise(resolve => setImmediate(resolve));

for (const id of ['zenti', 'orbit']) {
  test(`${id}: one sharp speaking pose; only real playback drives meter; reduced motion and idle stop it`, async () => {
    const { image, properties, view } = fixture(class { decode() { return Promise.resolve(); } });
    const level = () => Number(properties['--voice-level']);
    view.select(id); await tick();
    assert.match(image.src, new RegExp(`${id}-idle.png$`));
    view.update('speaking', 0.2, 0, true); await tick();
    const speaking = image.src;
    assert.match(speaking, new RegExp(`${id}-speaking.png$`));
    assert.ok(level() > 0 && level() < 1, 'meter opens gradually');
    for (let i = 1; i <= 12; i++) {
      view.update('speaking', i % 2 ? 0.03 : 0.2, i * 16, true); await tick();
      assert.equal(image.src, speaking, 'loudness must never blend or swap faces');
    }
    const closing = [];
    for (let i = 0; i < 40; i++) { view.update('speaking', 0, 220 + i * 16, true); closing.push(level()); }
    assert.ok(closing.every((v, i) => !i || v <= closing[i - 1]), 'silence closes monotonically');
    assert.ok(level() < 0.01);
    view.update('speaking', 0.2, 900, false); await tick();
    assert.equal(level(), 0); assert.equal(image.src, speaking, 'reduced motion preserves meaningful state');
    view.update('idle', null, 920, true); await tick();
    assert.match(image.src, new RegExp(`${id}-idle.png$`)); assert.equal(level(), 0);
    for (const state of ['listening', 'thinking', 'working', 'success', 'error', 'approval']) {
      view.update(state, 0.2, 950, true); await tick();
      assert.match(image.src, new RegExp(`${id}-${state}.png$`));
      assert.equal(level(), 0, 'non-playback states cannot fabricate speaking activity');
    }
  });
}

test('slow image decoding cannot replace a newer avatar or expression', async () => {
  const decodes = new Map();
  const { image, properties, view } = fixture(class {
    decode() { return new Promise(resolve => decodes.set(this.src, resolve)); }
  });
  view.select('zenti'); view.update('listening', null, 0, true);
  view.select('orbit');
  decodes.get('avatars/orbit-idle.png')(); await tick();
  assert.match(image.src, /orbit-idle.png$/);
  decodes.get('avatars/zenti-listening.png')(); await tick();
  assert.match(image.src, /orbit-idle.png$/);
  assert.equal(properties['--voice-level'], '0.000');
});

test('missing expression keeps decoded artwork visible without retrying every playback frame', async () => {
  let failures = 0;
  const { image, view } = fixture(class {
    decode() { return this.src.includes('thinking') ? (failures++, Promise.reject(Error('missing fixture'))) : Promise.resolve(); }
  });
  view.select('zenti'); await tick();
  for (let i = 0; i < 4; i++) { view.update('thinking', null, i * 16, true); await tick(); }
  assert.match(image.src, /zenti-idle.png$/);
  assert.equal(image.hidden, false); assert.equal(failures, 1);
});
