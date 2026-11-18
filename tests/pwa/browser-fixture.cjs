/* Isolated loopback fixture; speech adapters and credentials are synthetic. */
const { spawn } = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');
async function startFixture(t) {
  const server = spawn(process.env.PYTHON || 'python', [path.join(__dirname, 'browser_fixture.py')], { env: { ...process.env,
    ZV_GATEWAY_ROOT: path.resolve(__dirname, '../../services/voice-gateway') }, stdio: ['ignore', 'pipe', 'pipe'] });
  let stderr = ''; server.stderr.on('data', b => { stderr += b; }); t.after(() => server.kill());
  return await new Promise((resolve, reject) => {
    let line = ''; const timer = setTimeout(() => reject(Error(stderr || 'fixture timeout')), 15000);
    server.stdout.on('data', b => { line += b; if (line.includes('\n')) { clearTimeout(timer); resolve(JSON.parse(line.split('\n')[0])); } });
    server.on('exit', code => { clearTimeout(timer); reject(Error(`fixture exit ${code}: ${stderr}`)); });
  });
}
module.exports = { startFixture };
