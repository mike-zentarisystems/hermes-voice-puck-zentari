const {test} = require('node:test');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const path = require('node:path');
const {chromium, firefox, webkit} = require('playwright');
const AxeBuilder = require('@axe-core/playwright').default;
const engine = process.env.ZV_TEST_BROWSER || 'chromium';
test(`${engine}: static demo privacy, keyboard, cancellation, reset and narrow layouts`, {timeout:60000}, async t => {
  const root = path.resolve(__dirname,'../../dist/site');
  const http = require('node:http'), fs = require('node:fs');
  const server = http.createServer((req,res) => {
    const relative = req.url.replace(/^\/zenti-puck\//, '/');
    const name = relative === '/' ? '/index.html' : relative;
    const file = path.resolve(root,'.' + name);
    if (!file.startsWith(root + path.sep)) { res.writeHead(403).end(); return; }
    const types = {'.html':'text/html','.css':'text/css','.js':'text/javascript','.png':'image/png'};
    res.setHeader('Content-Type',types[path.extname(file)] || 'application/octet-stream');
    if (!fs.existsSync(file)) {res.writeHead(404).end(); return;}
    fs.createReadStream(file).pipe(res);
  });
  await new Promise(resolve => server.listen(0,'127.0.0.1',resolve)); t.after(() => new Promise(resolve => server.close(resolve)));
  const browser = await ({chromium,firefox,webkit}[engine]).launch({headless:true}); t.after(() => browser.close());
  const context = await browser.newContext({viewport:{width:1280,height:900}});
  const page = await context.newPage(), errors=[], external=[];
  page.on('pageerror',e=>errors.push(e.message));
  page.on('request',r=>{if (!r.url().startsWith(`http://127.0.0.1:${server.address().port}/`)) external.push(r.url());});
  await page.goto(`http://127.0.0.1:${server.address().port}/zenti-puck/`);
  await page.getByRole('button',{name:'What stays private?'}).click();
  await page.waitForFunction(()=>document.querySelector('#messages li:last-child').textContent.includes('never records'));
  await page.locator('#message').fill('<img src=x onerror=alert(1)>'); await page.locator('#send').click();
  await page.locator('#stop').click(); await page.waitForTimeout(800);
  assert.equal(await page.locator('#messages img').count(),0);
  assert.equal(await page.locator('#messages li').count(),4);
  await page.getByRole('button',{name:'Switch companion'}).click();
  assert.match(await page.locator('#portrait').getAttribute('alt'),/Orbit/);
  await page.getByRole('button',{name:'Start over'}).click(); assert.equal(await page.locator('#messages li').count(),1);
  for (const [width,height] of [[320,640],[390,380],[768,1024],[1440,900]]) {
    await page.setViewportSize({width,height});
    assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),`${width} overflow`);
  }
  await page.emulateMedia({reducedMotion:'reduce'});
  await page.locator('#message').focus(); await page.keyboard.type('Run locally'); await page.keyboard.press('Enter');
  await page.waitForFunction(()=>document.querySelector('#messages li:last-child').textContent.includes('Compose'));
  assert.equal(await page.evaluate(()=>localStorage.length+sessionStorage.length),0);
  assert.deepEqual(external,[]); assert.deepEqual(errors,[]);
  if (engine === 'chromium') {
    // This CSP forbids all frames; audit the frame-free document in place.
    const accessibility = await new AxeBuilder({page}).setLegacyMode().withTags(['wcag2a','wcag2aa','wcag21aa','wcag22aa']).analyze();
    assert.deepEqual(accessibility.violations.map(x=>({id:x.id,targets:x.nodes.map(n=>n.target)})),[]);
  }
  await page.setViewportSize({width:1280,height:1000});
  await page.screenshot({path:path.resolve(__dirname,`../../out/demo-${engine}.png`),fullPage:true});
});
