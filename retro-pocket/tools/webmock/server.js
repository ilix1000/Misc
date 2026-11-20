// Mock Retro Pocket for developing the web UI on a PC.
//   node tools/webmock/server.js [port] [frame.ppm]
// Serves main/web/index.html and fakes the device API + WebSocket screen.
const http = require('http'), fs = require('fs'), path = require('path'), crypto = require('crypto');
const PORT = +process.argv[2] || 8080;
const ROOT = path.join(__dirname, '..', '..');
const log = [];                       // requests seen, for tests: GET /__log
let keys = 0;
let settings = { brightness: 80, rotation: 0, stick_rot: 0, swap_rb: 0, gb_palette: 0, nes_scale: 0, show_fps: 0, ui_anim: 1,
  ota_repo: 'ilix1000/Misc', ota_branch: 'main', ota_path: 'retro-pocket/firmware/ota', ota_has_token: false };
const games = [
  { name: 'Alpha Quest (USA)', path: '/roms/Alpha Quest (USA).zip', sys: 1, fav: true },
  { name: 'Bravo Racer (Europe)', path: '/roms/Bravo Racer (Europe).zip', sys: 1, fav: false },
  { name: 'Comet Puzzle (World)', path: '/Downloads/Comet Puzzle (World).gb', sys: 2, fav: false },
  { name: 'Delta Dungeon (USA) (Rev 1)', path: '/roms/gb/Delta Dungeon (USA) (Rev 1).zip', sys: 2, fav: true },
];
const files = { '/': [{ name: 'roms', dir: true, size: 0 }, { name: 'saves', dir: true, size: 0 }, { name: 'Downloads', dir: true, size: 0 }],
  '/roms': [{ name: 'Alpha Quest (USA).zip', dir: false, size: 131072 }, { name: 'gb', dir: true, size: 0 }] };
let ota = { phase: 'idle', percent: 0, latest: '', message: '', available: -1 };
let playing = '';

// Screen: RGB565 big-endian, from a PPM screenshot if given, else a gradient.
function frame() {
  let w = 160, h = 128, px;
  const ppm = process.argv[3];
  if (ppm && fs.existsSync(ppm)) {
    const b = fs.readFileSync(ppm), m = b.toString('latin1', 0, 20).match(/P6\s+(\d+)\s+(\d+)\s+255\s/);
    w = +m[1]; h = +m[2]; const off = m[0].length;
    px = i => [b[off + i * 3], b[off + i * 3 + 1], b[off + i * 3 + 2]];
  } else px = i => [(i % 160) * 1.5, (i / 160 | 0) * 2, 128];
  const out = Buffer.alloc(4 + w * h * 2);
  out.writeUInt16LE(w, 0); out.writeUInt16LE(h, 2);
  for (let i = 0; i < w * h; i++) { const [r, g, b] = px(i); out.writeUInt16BE(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3), 4 + i * 2); }
  return out;
}

function json(res, o, code = 200) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(o)); }
function body(req) { return new Promise(r => { const c = []; req.on('data', d => c.push(d)); req.on('end', () => r(Buffer.concat(c))); }); }

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x'), q = Object.fromEntries(u.searchParams);
  const b = await body(req);
  log.push({ method: req.method, path: u.pathname, query: q, size: b.length, body: b.length < 2000 ? b.toString() : '' });
  const route = req.method + ' ' + u.pathname;
  switch (route) {
    case 'GET /': res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(fs.readFileSync(path.join(ROOT, 'main/web/index.html')));
    case 'GET /__log': return json(res, { log, keys });
    case 'GET /api/info': return json(res, { version: '2.0.0', wifi: 'connected', ip: '192.168.1.42', ssid: 'HomeNetwork', rssi: -58, hotspot: false,
      ap_ssid: 'RetroPocket-1A2B', heap: 61234, psram: 7340032, uptime: 3725, sd: true, sd_total: 31914983424, sd_free: 29000000000,
      playing, screen_w: 160, screen_h: 128, nes: 2, gb: 2, ota });
    case 'GET /api/games': return json(res, { games, recent: [games[2], games[0]] });
    case 'POST /api/launch': playing = (games.find(g => g.path == q.path) || {}).name || q.path; return json(res, { ok: true });
    case 'POST /api/exit': playing = ''; return json(res, { ok: true });
    case 'POST /api/fav': { const g = games.find(g => g.path == q.path); if (g) g.fav = q.on == '1'; return json(res, { ok: true }); }
    case 'POST /api/rescan': return json(res, { ok: true });
    case 'GET /api/files': return files[q.dir] ? json(res, { files: files[q.dir] }) : json(res, { ok: false, error: 'folder not found' }, 404);
    case 'PUT /api/file': { const dir = q.path.replace(/\/[^/]+$/, '') || '/'; (files[dir] = files[dir] || []).push({ name: q.path.split('/').pop(), dir: false, size: b.length }); return json(res, { ok: true }); }
    case 'DELETE /api/file': { const dir = q.path.replace(/\/[^/]+$/, '') || '/'; files[dir] = (files[dir] || []).filter(f => f.name != q.path.split('/').pop()); return json(res, { ok: true }); }
    case 'POST /api/mkdir': case 'POST /api/rename': case 'POST /api/reboot': case 'POST /api/wifi/forget': case 'POST /api/wifi/hotspot': return json(res, { ok: true });
    case 'GET /api/settings': return json(res, settings);
    case 'POST /api/settings': Object.assign(settings, JSON.parse(b.toString())); return json(res, { ok: true });
    case 'GET /api/wifi/scan': return setTimeout(() => json(res, { networks: [{ ssid: 'HomeNetwork', rssi: -48, secure: true }, { ssid: 'Cafe Guest', rssi: -70, secure: false }] }), 600);
    case 'POST /api/wifi/connect': return json(res, { ok: true });
    case 'POST /api/ota/check': ota = q.install == '1' ? { ...ota, phase: 'downloading', percent: 35, message: 'Downloading...' } : { ...ota, latest: '2.0.1', available: 1, message: 'Update available: v2.0.1' }; return json(res, { ok: true });
    case 'POST /api/ota/upload': return b.length < 65536 ? json(res, { ok: false, error: 'that file is too small to be firmware' }, 400) : json(res, { ok: true });
    case 'GET /api/screenshot.bmp': res.writeHead(200, { 'Content-Type': 'image/bmp' }); return res.end(Buffer.alloc(10));
  }
  json(res, { ok: false, error: 'not found' }, 404);
});

// Minimal WebSocket (RFC 6455): text frames in, binary frames out.
server.on('upgrade', (req, sock) => {
  const key = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${key}\r\n\r\n`);
  let buf = Buffer.alloc(0);
  sock.on('data', d => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 6) {
      let len = buf[1] & 127, off = 2;
      if (len == 126) { len = buf.readUInt16BE(2); off = 4; }
      if (buf.length < off + 4 + len) break;
      const mask = buf.slice(off, off + 4), data = Buffer.from(buf.slice(off + 4, off + 4 + len)).map((x, i) => x ^ mask[i % 4]);
      const op = buf[0] & 15; buf = buf.slice(off + 4 + len);
      if (op == 8) return sock.end();
      const msg = data.toString();
      if (msg[0] == 'k') { const k = +msg.slice(1); if (k != keys) log.push({ ws: msg }); keys = k; }
      if (msg == 'f') {
        const f = frame();
        let hdr;   // shortest length encoding, as the spec requires
        if (f.length < 65536) { hdr = Buffer.alloc(4); hdr[1] = 126; hdr.writeUInt16BE(f.length, 2); }
        else { hdr = Buffer.alloc(10); hdr[1] = 127; hdr.writeBigUInt64BE(BigInt(f.length), 2); }
        hdr[0] = 0x82;
        sock.write(Buffer.concat([hdr, f]));
      }
    }
  });
  sock.on('error', () => {});
});
server.listen(PORT, () => console.log('mock device on http://localhost:' + PORT));
