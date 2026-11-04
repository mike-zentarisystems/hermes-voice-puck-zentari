const {test} = require('node:test');
const assert = require('node:assert/strict');
const {chromium} = require('playwright');
const AxeBuilder = require('@axe-core/playwright').default;
const {startFixture} = require('./browser-fixture.cjs');
test('PWA: automated WCAG checks at phone/desktop widths and sign-in dialog', {timeout:60000}, async t => {
  const config = await startFixture(t), browser = await chromium.launch({headless:true});
  t.after(()=>browser.close());
  const context = await browser.newContext({ignoreHTTPSErrors:true}), page = await context.newPage();
  await page.goto(config.url);
  async function audit(state) {
    const result = await new AxeBuilder({page}).withTags(['wcag2a','wcag2aa','wcag21aa','wcag22aa']).analyze();
    assert.deepEqual(result.violations.map(x=>({id:x.id,impact:x.impact,targets:x.nodes.map(n=>n.target)})),[],state);
  }
  for (const width of [320,1280]) {
    await page.setViewportSize({width,height:800}); await audit(`signed out ${width}`);
  }
  await page.locator('#settings-open').click(); await audit('settings sign-in');
  await page.locator('#password').fill('fixture-password-only-123'); await page.locator('#sign-in').click();
  await page.waitForFunction(()=>!document.getElementById('send').disabled);
  await page.waitForFunction(()=>!document.getElementById('settings').open); await audit('signed in');
});
