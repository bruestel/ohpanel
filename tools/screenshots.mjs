#!/usr/bin/env node
/* Screenshots for the README and the docs, from the demo (tools/build_demo.py): every page of the demo's
   panel as the preview draws it, and pages of the web interface. Needs Chromium and Node 22 (no packages).

     tools/build_demo.py && tools/screenshots.mjs [out]      # default: docs/images/screens

   Serves build/demo itself, drives a headless Chromium over the DevTools protocol and writes PNGs. */
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {readFile, mkdir, writeFile} from 'node:fs/promises';
import {extname, join, resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DEMO = join(ROOT, 'build', 'demo');
const OUT = resolve(process.argv[2] || join(ROOT, 'docs', 'images', 'screens'));
const PORT = 8791, CDP = 9336, SCALE = 2;
const sleep = ms => new Promise(r => setTimeout(r, ms));

// the panel's pages: the file name and the preview's page id
const DISPLAY = [
  ['home', 'home'], ['buttons', 'kitchen'], ['light', 'counter'], ['thermostat', 'thermostat'],
  ['media', 'speaker'], ['favorites', 'radio'], ['cover', 'blind-kitchen'], ['awning', 'awning'],
  ['cover-group', 'blinds-front'], ['entities-cards', 'energy'], ['entities-rows', 'plants'],
  ['entities-list', 'washer'], ['waste', 'waste'], ['weather', 'weather'], ['qr', 'guest-wifi'], ['info', 'info'],
];
// the web interface: the file name, the address, the window
const WEB = [
  ['web-panel', '#panel', [1300, 900]], ['web-editor', '#panel/kitchen/edit', [1300, 900]],
  ['web-status', '#status', [1300, 900]], ['web-panel-phone', '#panel', [390, 844]],
];

const TYPES = {'.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json', '.bin': 'application/octet-stream'};
const server = createServer(async (req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  try {
    const body = await readFile(join(DEMO, path === '/' ? 'index.html' : path));
    res.writeHead(200, {'Content-Type': TYPES[extname(path)] || 'text/html'}).end(body);
  } catch { res.writeHead(404).end(); }
}).listen(PORT);

const chrome = spawn('chromium', ['--headless=new', `--remote-debugging-port=${CDP}`, '--user-data-dir=/tmp/ohpanel-shots',
  '--hide-scrollbars', '--window-size=1300,900', 'about:blank'], {stdio: 'ignore'});
await sleep(1500);
const list = await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json();
const ws = new WebSocket(list.find(t => t.type === 'page').webSocketDebuggerUrl);
let id = 0;
const pending = new Map();
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({id: i, method, params})); });
ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); } };
await new Promise(r => { ws.onopen = r; });
await send('Page.enable');
await send('Runtime.enable');
const evaluate = async expr => (await send('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true})).result.value;
const size = async ([w, h]) => send('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: SCALE, mobile: w < 600});
const open = async (hash, wait) => {
  await send('Page.navigate', {url: `http://127.0.0.1:${PORT}/${hash}`});
  await sleep(wait);
  await evaluate("document.querySelectorAll('div').forEach(d => { if (d.textContent.startsWith('OHpanel demo:')) d.remove(); })");
};
const shoot = async (name, clip) => {
  const r = await send('Page.captureScreenshot', clip ? {format: 'png', clip: {...clip, scale: 1}} : {format: 'png'});
  await writeFile(join(OUT, name + '.png'), Buffer.from(r.data, 'base64'));
  console.log(name);
};

await mkdir(OUT, {recursive: true});
await size([1300, 900]);
for (const [name, page] of DISPLAY) {
  await open(page === 'home' ? '#panel' : `#panel/${page}`, 3500);
  const r = await evaluate("(b => b && b.toJSON())(document.getElementById('vBox')?.getBoundingClientRect())");
  // the visible part: 450 of the design's 480 points (the panel's frame covers the rest)
  await shoot('display-' + name, r && {x: r.x, y: r.y, width: r.width * 450 / 480, height: r.height});
}
for (const [name, hash, win] of WEB) {
  await size(win);
  await open(hash, 3500);
  await shoot(name);
}
ws.close();
chrome.kill();
server.close();
