/* Actual candidate gateway, synthetic microphone and fixture speech adapters. */
const { test } = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const { chromium, firefox } = require('playwright');
const { startFixture } = require('./browser-fixture.cjs');
const engine = process.env.ZV_TEST_BROWSER || 'chromium';
assert.ok(['chromium', 'firefox'].includes(engine), 'ZV_TEST_BROWSER must be chromium or firefox');
test(`${engine}: responsive PWA controls, text, profile context, reconnect and offline avatar`, { timeout: 90000 }, async t => {
  const config = await startFixture(t);
  const browser = engine === 'chromium'
    ? await chromium.launch({ headless: true, args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream', '--ignore-certificate-errors', '--autoplay-policy=no-user-gesture-required'] })
    : await firefox.launch({ headless: true, firefoxUserPrefs: { 'media.navigator.streams.fake': true, 'media.navigator.permission.disabled': true, 'media.autoplay.default': 0 } });
  t.after(() => browser.close());
  const context = await browser.newContext({ ignoreHTTPSErrors: true, permissions: engine === 'chromium' ? ['microphone'] : [], viewport: { width: 390, height: 844 },
    ...(process.env.ZV_RECORD_VIDEO === '1' ? {recordVideo:{dir:path.join(__dirname,'../../out/pwa-videos'),size:{width:390,height:844}}} : {}) });
  const page = await context.newPage(), errors = []; page.on('pageerror', e => errors.push(e.message)); await page.goto(config.url);
  await page.locator('#settings-open').click();
  await page.locator('#password').fill('incorrect-fixture-password');
  await page.locator('#sign-in').click();
  await page.waitForFunction(() => document.getElementById('sign-in-status').textContent === 'Sign-in failed.');
  assert.equal(await page.locator('#password').inputValue(), '');
  assert.equal(await page.locator('#send').isDisabled(), true);
  await page.setViewportSize({width:320,height:640});
  assert.ok(await page.locator('#settings').evaluate(el => el.scrollWidth <= el.clientWidth), 'password dialog overflow');
  await page.setViewportSize({width:390,height:844});
  await page.locator('#password').fill('fixture-password-only-123');
  await page.locator('#sign-in').click();
  await page.waitForFunction(() => !document.getElementById('send').disabled);
  assert.doesNotMatch(await page.locator('#empty').textContent(), /Sign in/);
  assert.equal(await page.locator('#stop').isVisible(), false);
  // Retain the already-live renderer without introducing executable message HTML.
  const rendered = await page.evaluate(() => {
    const el = window.renderMessage('# Heading\n**Bold** and `code`\n- first\n- second\n```js\n<script>window.attacked=true</script>\n```\n[unsafe](javascript:alert(1)) [safe](https://example.com)\n<img src=x onerror=alert(1)>');
    document.body.append(el);
    const result = { heading: el.querySelector('h3').textContent, bold: el.querySelector('b').textContent,
      bullets: el.querySelectorAll('ul li').length, code: el.querySelector('pre').textContent,
      links: [...el.querySelectorAll('a')].map(a => ({href:a.href,rel:a.rel})),
      executable: el.querySelectorAll('script,img,iframe').length, literal: el.textContent.includes('javascript:alert') };
    el.remove(); return result;
  });
  assert.equal(rendered.heading, 'Heading'); assert.equal(rendered.bold, 'Bold'); assert.equal(rendered.bullets, 2);
  assert.match(rendered.code, /<script>/); assert.equal(rendered.executable, 0); assert.equal(rendered.literal, true);
  assert.deepEqual(rendered.links, [{href:'https://example.com/',rel:'noopener noreferrer'}]);
  const stats = async () => (await page.request.get(config.url + '/__test/stats')).json();
  assert.equal(await page.locator('#mouth').count(), 0);
  const visuals = path.join(__dirname, '../../out/pwa-visuals', engine); fs.mkdirSync(visuals, {recursive:true});
  async function text(value) {
    await page.locator('#message').fill(value); await page.locator('#send').click();
    await page.waitForFunction(() => document.getElementById('send').disabled);
    await page.waitForFunction(() => !document.getElementById('send').disabled);
  }
  await text('What is my name?'); assert.match(await page.locator('#transcript').textContent(), /Alex/);
  await text('My project is Juniper'); await text('What project?'); assert.match(await page.locator('#transcript li').last().textContent(), /Juniper/);
  await page.evaluate(() => Object.defineProperty(navigator, 'clipboard', {configurable:true,
    value:{writeText:async text => {window.fixtureCopied=text;}}}));
  await page.locator('#transcript > li').last().locator('.copy-message').click();
  assert.equal(await page.evaluate(() => window.fixtureCopied), 'Juniper');
  await page.locator('#transcript').evaluate(el => { el.scrollTop = 0; el.dispatchEvent(new Event('scroll')); });
  await text('Another response while reading earlier messages');
  assert.equal(await page.locator('#new-replies').isVisible(), true);
  await page.locator('#new-replies').click();
  assert.equal(await page.locator('#new-replies').isVisible(), false);
  const [download] = await Promise.all([page.waitForEvent('download'), page.locator('#export').click()]);
  assert.match(download.suggestedFilename(), /^conversation-[0-9-]+\.md$/);
  const exported = fs.readFileSync(await download.path(), 'utf8');
  assert.match(exported, /^# Conversation /); assert.match(exported, /## You\n\nWhat is my name\?/); assert.match(exported, /## Assistant\n\n[^\n]*Juniper/);
  await page.locator('#mic').click(); assert.equal(await page.locator('#mic').getAttribute('aria-pressed'), 'true'); assert.equal(await page.locator('#talk').isDisabled(), true);
  await text('Text works while microphone is muted');
  await page.locator('#mic').click();
  await page.locator('#talk').focus(); await page.keyboard.down(' '); await page.waitForTimeout(500); await page.keyboard.up(' ');
  await page.waitForFunction(() => document.getElementById('activity').textContent.includes('Speaking'));
  await page.locator('#stop').click(); await page.waitForTimeout(300); assert.equal(await page.locator('#send').isDisabled(), false);
  assert.ok((await stats()).stt >= 1);
  // Cancel must be reachable while a microphone request is still pending.
  await page.evaluate(() => {
    window.pendingCapture = { original: navigator.mediaDevices.getUserMedia };
    navigator.mediaDevices.getUserMedia = () => new Promise(resolve => { window.pendingCapture.resolve = resolve; });
  });
  try {
    await page.locator('#talk').focus(); await page.keyboard.down(' ');
    await page.waitForFunction(() => !!window.pendingCapture.resolve);
    assert.equal(await page.locator('#stop').isVisible(), true);
    assert.equal(await page.locator('#stop').textContent(), 'Cancel recording');
    await page.locator('#stop').click(); await page.keyboard.up(' ');
    await page.evaluate(() => {
      const probe = window.pendingCapture;
      probe.context = new AudioContext(); probe.stream = probe.context.createMediaStreamDestination().stream;
      probe.resolve(probe.stream);
    });
    await page.waitForFunction(() => window.pendingCapture.stream.getTracks().every(track => track.readyState === 'ended'));
    assert.equal(await page.locator('#stop').isVisible(), false);
  } finally {
    await page.evaluate(async () => {
      const probe = window.pendingCapture; navigator.mediaDevices.getUserMedia = probe.original;
      probe.stream?.getTracks().forEach(track => track.stop()); await probe.context?.close(); delete window.pendingCapture;
    });
  }
  // Silent pre-roll has no gateway turn yet: capture-stop must clear Listening.
  await page.evaluate(() => {
    const media = navigator.mediaDevices, context = new AudioContext();
    const stream = context.createMediaStreamDestination().stream;
    const original = media.getUserMedia, enumerate = media.enumerateDevices;
    window.silentCapture = { context, stream, original, enumerate };
    media.getUserMedia = async () => stream;
  });
  try {
    await page.locator('#hands-free').click();
    await page.waitForFunction(() => document.getElementById('activity').textContent === 'Listening');
    await page.locator('#mic').click();
    assert.equal(await page.locator('#activity').textContent(), 'Ready \u00b7 Mic muted');
    assert.equal(await page.locator('#avatar').getAttribute('data-state'), 'idle');
    assert.equal(await page.locator('#hands-free').getAttribute('aria-pressed'), 'false');
    assert.equal(await page.locator('#talk').isDisabled(), true);
    assert.equal(await page.locator('#stop').isDisabled(), true);
    assert.equal(await page.evaluate(() => window.silentCapture.stream.getTracks().every(t => t.readyState === 'ended')), true);

    // A device enumeration that finishes after mute must not restore Listening.
    await page.locator('#mic').click();
    await page.evaluate(async () => {
      const probe = window.silentCapture;
      probe.stream = probe.context.createMediaStreamDestination().stream;
      const devices = await probe.enumerate.call(navigator.mediaDevices);
      navigator.mediaDevices.getUserMedia = async () => probe.stream;
      navigator.mediaDevices.enumerateDevices = () => new Promise(resolve => {
        probe.releaseDevices = () => resolve(devices);
      });
    });
    await page.locator('#hands-free').click();
    await page.waitForFunction(() => typeof window.silentCapture.releaseDevices === 'function');
    await page.locator('#mic').click();
    await page.evaluate(async () => {
      window.silentCapture.releaseDevices();
      await new Promise(requestAnimationFrame);
    });
    assert.equal(await page.locator('#activity').textContent(), 'Ready \u00b7 Mic muted');
    assert.equal(await page.locator('#avatar').getAttribute('data-state'), 'idle');
    assert.equal(await page.evaluate(() => window.silentCapture.stream.getTracks().every(t => t.readyState === 'ended')), true);
  } finally {
    await page.evaluate(async () => {
      const probe = window.silentCapture;
      navigator.mediaDevices.getUserMedia = probe.original;
      navigator.mediaDevices.enumerateDevices = probe.enumerate;
      probe.stream.getTracks().forEach(t => t.stop());
      await probe.context.close(); delete window.silentCapture;
    });
  }
  // Microphone mute must preserve a reply that is already playing.
  await page.locator('#message').fill('Reply while muted'); await page.locator('#send').click();
  await page.waitForFunction(() => document.getElementById('activity').textContent === 'Speaking \u00b7 Mic muted');
  await page.locator('#mic').click();
  assert.equal(await page.locator('#activity').textContent(), 'Speaking');
  await page.locator('#mic').click();
  assert.equal(await page.locator('#activity').textContent(), 'Speaking \u00b7 Mic muted');
  await page.locator('#stop').click();
  await page.waitForFunction(() => document.getElementById('activity').textContent === 'Ready \u00b7 Mic muted');
  await page.locator('#mic').click();
  await page.locator('#hands-free').click();
  await page.waitForFunction(() => document.getElementById('hands-free').getAttribute('aria-pressed') === 'true');
  await page.evaluate(() => { window.dispatchEvent(new Event('blur')); navigator.mediaDevices.dispatchEvent(new Event('devicechange')); });
  await page.waitForTimeout(300);
  assert.equal(await page.locator('#hands-free').getAttribute('aria-pressed'), 'true', 'focus loss or device list change ended hands-free');
  await page.locator('#mic').click();
  assert.equal(await page.locator('#hands-free').getAttribute('aria-pressed'), 'false');
  assert.equal(await page.locator('#talk').isDisabled(), true);
  await page.locator('#mic').click();
  await page.locator('#settings-open').click();
  const inputs = await page.locator('#input-device option').count(); assert.ok(inputs > 1);
  await page.locator('#input-device').selectOption({index:1});
  await page.locator('#settings button').last().click();
  const transcript = await page.locator('#transcript').textContent();
  await page.request.post(config.url + '/__test/disconnect'); await page.waitForFunction(() => document.getElementById('transcript').hidden);
  await page.waitForFunction(() => !document.getElementById('send').disabled);
  assert.equal(await page.locator('#transcript').textContent(), transcript);
  await page.locator('#settings-open').click(); await page.locator('#avatar-select').selectOption('orbit');
  await page.locator('#settings button').last().click();
  await page.locator('#volume').fill('65'); await page.locator('#volume').dispatchEvent('input');
  assert.equal(await page.locator('#volume-value').textContent(), '65%');
  assert.equal(await page.locator('#volume').isVisible(), true);
  await page.locator('#volume').focus(); await page.keyboard.press('ArrowRight');
  assert.equal(await page.locator('#volume-value').textContent(), '66%');
  await page.keyboard.press('ArrowLeft');
  await page.locator('#settings-open').focus(); await page.keyboard.press('Enter');
  assert.equal(await page.locator('#settings').isVisible(), true);
  await page.keyboard.press('Escape');
  assert.equal(await page.locator('#settings').isVisible(), false);
  assert.equal(await page.locator('#settings-open').evaluate(el => document.activeElement === el), true);
  for (const id of ['zenti', 'orbit']) {
    await page.locator('#settings-open').click(); await page.locator('#avatar-select').selectOption(id);
    await page.locator('#settings button').last().click();
    await page.waitForFunction(id => document.getElementById('portrait').src.endsWith(id + '-idle.png'), id);
    await page.locator('#message').fill('Animation fixture'); await page.locator('#send').click();
    await page.waitForFunction(id => document.getElementById('portrait').src.endsWith(id + '-speaking.png') &&
      Number(document.getElementById('voice-meter').style.getPropertyValue('--voice-level')) > 0.1, id);
    assert.equal(await page.locator('#avatar img').count(), 1, 'a second portrait would reintroduce ghosting');
    assert.equal(await page.locator('#portrait').evaluate(el => getComputedStyle(el).opacity), '1');
    assert.equal(await page.locator('#stop').textContent(), 'Stop audio');
    const before = await page.locator('#portrait').boundingBox();
    await page.screenshot({path:path.join(visuals, id + '-speaking-phone.png'),fullPage:true});
    if (process.env.ZV_RECORD_VIDEO === '1') await page.waitForTimeout(600);
    await page.locator('#stop').focus(); await page.locator('#stop').click();
    await page.waitForFunction(id => document.getElementById('portrait').src.endsWith(id + '-idle.png'), id);
    const after = await page.locator('#portrait').boundingBox();
    assert.equal(await page.locator('#stop').isVisible(), false);
    assert.equal(await page.locator('#talk').evaluate(el => document.activeElement === el), true);
    assert.ok(Math.abs(before.width - after.width) < 0.01 && Math.abs(before.height - after.height) < 0.01,
      'state changes must retain avatar dimensions (allow browser subpixel rounding)');
    await page.screenshot({path:path.join(visuals, id + '-idle-phone.png'),fullPage:true});
    // Explicit and system reduced motion both keep state poses and stop the meter.
    for (const preference of ['setting', 'system']) {
      await page.locator('#settings-open').click();
      await page.locator('#reduce-motion').setChecked(preference === 'setting');
      await page.locator('#settings button').last().click();
      await page.emulateMedia({reducedMotion: preference === 'system' ? 'reduce' : 'no-preference'});
      await page.locator('#message').fill('Reduced motion fixture'); await page.locator('#send').click();
      await page.waitForFunction(id => document.getElementById('avatar').dataset.state === 'speaking' &&
        document.getElementById('portrait').src.endsWith(id + '-speaking.png'), id);
      assert.equal(await page.locator('#avatar').evaluate(el => getComputedStyle(el).animationName), 'none');
      assert.equal(await page.locator('#voice-meter').evaluate(el => el.style.getPropertyValue('--voice-level')), '0.000');
      await page.locator('#stop').click();
      await page.waitForFunction(() => !document.getElementById('send').disabled);
    }
    await page.emulateMedia({reducedMotion:'no-preference'});
    await page.locator('#settings-open').click(); await page.locator('#reduce-motion').uncheck();
    await page.locator('#settings button').last().click();
  }
  await page.locator('#settings-open').click();
  await page.locator('#reduce-motion').check(); await page.locator('#voice-replies').uncheck();
  await page.locator('#settings button').last().click();
  assert.equal(await page.locator('#portrait').getAttribute('alt'), 'Orbit');
  assert.equal(await page.locator('#avatar').evaluate(el => getComputedStyle(el).animationName), 'none');
  await text('Silent response');
  for (const [width,height] of [[320,640],[390,844],[768,1024],[1024,768],[1280,800],[1440,900],[844,390],[390,380]]) {
    await page.setViewportSize({width,height});
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `overflow ${width}x${height}`);
    await page.locator('#message').focus(); assert.ok(await page.locator('#send').isVisible());
    await page.locator('#send').scrollIntoViewIfNeeded();
    const send = await page.locator('#send').boundingBox(), footer = await page.locator('footer').boundingBox();
    assert.ok(send.y >= 0 && send.y + send.height <= footer.y + 1, `composer behind controls ${width}x${height}`);
    assert.ok(footer.y >= 0 && footer.y + footer.height <= height + 1, `controls outside viewport ${width}x${height}`);
    assert.equal(await page.locator('#volume').isVisible(), true);
    await page.screenshot({ path:path.join(visuals, `orbit-${width}x${height}.png`), fullPage:true });
  }
  await page.screenshot({ path: path.join(visuals,'responsive-candidate.png'), fullPage:true });
  // Resume conversation flows at the normal phone size after the bounds sweep.
  await page.setViewportSize({width:390,height:844});
  await page.locator('#new-conversation').click(); await page.waitForFunction(() => !document.getElementById('send').disabled);
  assert.equal(await page.locator('#transcript li').count(),0); await text('What project?'); assert.match(await page.locator('#transcript li').last().textContent(),/No project/);
  await page.reload(); await page.waitForFunction(() => !document.getElementById('send').disabled);
  assert.equal(await page.locator('#portrait').getAttribute('alt'),'Orbit');
  await page.locator('#settings-open').click();
  assert.equal(await page.locator('#volume').inputValue(), '65');
  assert.equal(await page.locator('#volume-value').textContent(), '65%');
  await page.locator('#settings button').last().click();
  await page.evaluate(async () => { await navigator.serviceWorker.ready; });
  await context.setOffline(true); await page.reload();
  assert.equal(await page.locator('#portrait').getAttribute('alt'),'Orbit');
  assert.ok(await page.locator('#portrait').evaluate(img => img.complete && img.naturalWidth > 0));
  await context.setOffline(false); await page.reload();
  await page.waitForFunction(() => !document.getElementById('send').disabled);
  // A second tab can revoke the shared cookie session even though only one
  // WebSocket is allowed. The original tab must stop retrying and sign out.
  const other = await context.newPage(); await other.goto(config.url);
  await other.waitForFunction(() => !document.getElementById('logout').disabled);
  await other.locator('#settings-open').click(); await other.locator('#logout').click();
  await other.waitForFunction(() => document.getElementById('connection').textContent === 'Signed out');
  await page.waitForFunction(() => document.getElementById('connection').textContent === 'Signed out');
  assert.equal((await page.request.get(config.url + '/api/session')).status(), 401);
  assert.deepEqual(errors,[]);
});
