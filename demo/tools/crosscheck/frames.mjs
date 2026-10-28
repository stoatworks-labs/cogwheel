// The page's frames against the plugin's own harness, at the same settings.
//
//     node demo/tools/crosscheck/frames.mjs --cgtest build/cgtest [--port 8712] [--size 640x360]
//
// For each case below: `cgtest --out` renders the REAL plugin class (both
// halves, Apple's GL) for N frames at 60 fps into an RGBA32F target; the page,
// loaded in headless Chrome, runs window.cogwheelDemo.renderer.offline() --
// the port's frame logic and the plugin's shaders through this page's GL
// calls, driven exactly as cgtest drives the plugin -- and reads its target
// back the way cgtest's toBytes does. The two RGBA8 frames are compared pixel
// for pixel.
//
// This is the check on what crosscheck.sh cannot see: Sheet::Render's port
// (the passes, the blending, the per-run draws, the uniforms) and the frame
// logic. It needs Chrome and a built cgtest, so verify.sh does not run it;
// run it by hand after changing either, and update the numbers in the page's
// differences list and AGENTS.md if they move.
//
// It serves demo/ itself on --port and talks to Chrome over a pipe
// (--remote-debugging-pipe), so it opens no other port.

import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { dirname, extname, join, normalize } from 'node:path';
import { tmpdir } from 'node:os';
import { inflateSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const demo = join(here, '..', '..');

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const at = args.indexOf(name);
  return at >= 0 ? args[at + 1] : fallback;
};
const cgtest = option('--cgtest', null);
const port = Number(option('--port', '8712'));
const [width, height] = option('--size', '640x360').split('x').map(Number);
const chrome = option('--chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
if (!cgtest) {
  console.error('usage: frames.mjs --cgtest PATH [--port N] [--size WxH]');
  process.exit(2);
}

// name, cgtest arguments, the hook's arguments. Ids are ParamIds.
const CASES = [
  ['Classic Rosette, 900 frames', ['--preset', '1', '--frames', '900'], { presetIndex: 1, frames: 900 }],
  ['Three Lobes (creep)', ['--preset', '2', '--frames', '600'], { presetIndex: 2, frames: 600 }],
  ['Dense Web (negative)', ['--preset', '3', '--frames', '600'], { presetIndex: 3, frames: 600 }],
  ['Flower (outside)', ['--preset', '4', '--frames', '600'], { presetIndex: 4, frames: 600 }],
  ['Slipping Gear (skips, fade)', ['--preset', '5', '--frames', '600'], { presetIndex: 5, frames: 600 }],
  ['Wet Trail (fibre tip, fade)', ['--preset', '6', '--frames', '300'], { presetIndex: 6, frames: 300 }],
  ['Chalkboard (tooth, negative)', ['--preset', '7', '--frames', '600'], { presetIndex: 7, frames: 600 }],
  ['Show the Gears (overlay)', ['--preset', '8', '--frames', '450'], { presetIndex: 8, frames: 450 }],
  ['Fade by Figure', ['--preset', '1', '--frames', '900', '--set', '35=0.6', '--set', '36=1'],
    { presetIndex: 1, frames: 900, sets: [[35, 0.6], [36, 1]] }],
  ['Clear Paper, Cover', ['--preset', '1', '--frames', '600', '--set', '32=1', '--set', '23=1'],
    { presetIndex: 1, frames: 600, sets: [[32, 1], [23, 1]] }],
  ['Ink: Ink from Clip', ['--effect', '--preset', '1', '--frames', '600', '--set', '27=1'],
    { effect: true, clip: true, presetIndex: 1, frames: 600, sets: [[27, 1]] }],
  ['Ink: Paper from Clip', ['--effect', '--preset', '2', '--frames', '600', '--set', '31=1'],
    { effect: true, clip: true, presetIndex: 2, frames: 600, sets: [[31, 1]] }],
  // The negative control: the page at the defaults against the plugin with
  // Speed a hair faster (0.740, not 0.737). It must FAIL, or the comparison
  // above proves nothing.
  ['NEGATIVE: Speed 0.737 vs 0.740', ['--preset', '1', '--frames', '900', '--set', '8=0.74'],
    { presetIndex: 1, frames: 900 }, 'differ'],
];

//---------------------------------------------------------------------------
// PNG: cgtest writes 8-bit RGBA, filter 0 on every row; any filter is decoded.
//---------------------------------------------------------------------------
function decodePng(buffer) {
  let at = 8;
  let w = 0;
  let h = 0;
  const idat = [];
  while (at < buffer.length) {
    const length = buffer.readUInt32BE(at);
    const type = buffer.toString('latin1', at + 4, at + 8);
    const data = buffer.subarray(at + 8, at + 8 + length);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0);
      h = data.readUInt32BE(4);
      if (data[8] !== 8 || data[9] !== 6) throw new Error('not 8-bit RGBA');
    } else if (type === 'IDAT') {
      idat.push(data);
    }
    at += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const stride = w * 4;
  const out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y += 1) {
    const filter = raw[y * (stride + 1)];
    const row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x += 1) {
      const a = x >= 4 ? out[y * stride + x - 4] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= 4 && y > 0 ? out[(y - 1) * stride + x - 4] : 0;
      let v = row[x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a);
        const pb = Math.abs(p - b);
        const pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = v & 0xff;
    }
  }
  return { width: w, height: h, pixels: out };
}

//---------------------------------------------------------------------------
// A static server for demo/, and Chrome over a pipe.
//---------------------------------------------------------------------------
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
function serve() {
  const server = createServer(async (req, res) => {
    let path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (path.endsWith('/')) path += 'index.html';
    try {
      const body = await readFile(join(demo, path));
      res.writeHead(200, { 'Content-Type': TYPES[extname(path)] ?? 'application/octet-stream' });
      res.end(body);
    } catch {
      res.writeHead(404);
      res.end();
    }
  });
  return new Promise((resolve) => server.listen(port, '127.0.0.1', () => resolve(server)));
}

async function launch(profile) {
  const proc = spawn(chrome, [
    '--headless=new', '--no-first-run', '--no-default-browser-check', '--remote-debugging-pipe',
    `--user-data-dir=${profile}`, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });
  let id = 0;
  let buffer = '';
  const pending = new Map();
  proc.stdio[4].on('data', (chunk) => {
    buffer += chunk.toString('utf8');
    let at;
    while ((at = buffer.indexOf('\0')) >= 0) {
      const msg = JSON.parse(buffer.slice(0, at));
      buffer = buffer.slice(at + 1);
      const waiter = msg.id && pending.get(msg.id);
      if (waiter) {
        pending.delete(msg.id);
        if (msg.error) waiter.reject(new Error(JSON.stringify(msg.error)));
        else waiter.resolve(msg.result);
      }
    }
  });
  const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    id += 1;
    pending.set(id, { resolve, reject });
    proc.stdio[3].write(`${JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })}\0`);
  });
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }, sessionId);
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  return {
    evaluate,
    navigate: (url) => send('Page.navigate', { url }, sessionId),
    // Wait for Chrome to exit before the profile is removed: it writes to it
    // on the way out, and rm then meets a directory that is not empty.
    close: async () => {
      const exited = proc.exitCode !== null ? Promise.resolve() : new Promise((r) => proc.once('exit', r));
      try { await send('Browser.close'); } catch { /* gone */ }
      setTimeout(() => proc.kill(), 5000).unref();
      await exited;
    },
  };
}

//---------------------------------------------------------------------------
const work = await mkdtemp(join(tmpdir(), 'cogwheel-frames-'));
const server = await serve();
const browser = await launch(join(work, 'profile'));
let failed = 0;
try {
  await browser.navigate(`http://127.0.0.1:${port}/`);
  for (let i = 0; i < 150; i += 1) {
    if (await browser.evaluate('!!window.cogwheelDemo?.renderer').catch(() => false)) break;
    await new Promise((r) => setTimeout(r, 200));
  }
  const renderer = await browser.evaluate(`(() => { const c = document.querySelector('canvas'); const g = c.getContext('webgl2'); const d = g.getExtension('WEBGL_debug_renderer_info'); return d ? g.getParameter(d.UNMASKED_RENDERER_WEBGL) : g.getParameter(g.RENDERER); })()`);
  console.log(`page GL: ${renderer}; ${width}x${height}`);
  console.log('case                              identical   within 1   over 1   over 4   max');

  for (const [name, cgArgs, hook, expect] of CASES) {
    const png = join(work, 'cg.png');
    execFileSync(cgtest, ['--out', png, '--size', `${width}x${height}`, ...cgArgs], { stdio: 'ignore' });
    const plugin = decodePng(readFileSync(png));

    const b64 = await browser.evaluate(`window.cogwheelDemo.renderer.offline(${JSON.stringify({ ...hook, width, height })})`);
    const page = Buffer.from(b64, 'base64');

    let same = 0;
    let one = 0;
    let overOne = 0;
    let overFour = 0;
    let max = 0;
    const n = width * height;
    for (let p = 0; p < n; p += 1) {
      let d = 0;
      for (let c = 0; c < 4; c += 1) d = Math.max(d, Math.abs(plugin.pixels[p * 4 + c] - page[p * 4 + c]));
      if (d === 0) same += 1;
      else if (d === 1) one += 1;
      else overOne += 1;
      if (d > 4) overFour += 1;
      if (d > max) max = d;
    }
    const pct = (k) => `${(100 * k / n).toFixed(3)}%`.padStart(9);
    console.log(`${name.padEnd(32)} ${pct(same)}  ${pct(same + one)}  ${String(overOne).padStart(7)}  ${String(overFour).padStart(7)}  ${String(max).padStart(4)}`);
    // No more than one pixel in ten thousand more than a level apart.
    const close = overOne <= n * 0.0001;
    if (expect === 'differ' ? close : !close) {
      failed += 1;
      console.log(expect === 'differ' ? '  FAIL: the negative control passed' : '  FAIL: more than 0.01% of pixels over one level apart');
    }
  }
} finally {
  await browser.close();
  server.close();
  await rm(work, { recursive: true, force: true, maxRetries: 5 });
}
console.log(failed ? `${failed} case(s) failed` : 'every case within one level of 255 but for at most 0.01% of pixels, and the negative control caught');
process.exit(failed ? 1 : 0);
