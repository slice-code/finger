#!/usr/bin/env node
'use strict';

const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');

const PORT = process.env.PORT || '/dev/ttyUSB0';
const BAUD = parseInt(process.env.BAUD || '9600', 10);
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '3000', 10);
const HTTPS_PORT = parseInt(process.env.HTTPS_PORT || '443', 10);
const CERT_PATH = path.join(__dirname, 'certs', 'cert.pem');
const KEY_PATH = path.join(__dirname, 'certs', 'key.pem');
const DB_PATH = path.join(__dirname, 'fingerprints.json');
const CONFIG_PATH = path.join(__dirname, 'config.json');

function loadConfig() {
  try { return JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8')); }
  catch { return { apiBaseUrl: 'http://192.168.1.15:3004' }; }
}
function saveConfigFile(cfg) {
  const c = loadConfig();
  Object.assign(c, cfg);
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(c, null, 2));
  config = c;
  API_BASE = c.apiBaseUrl || 'http://192.168.1.15:3004';
}

let config = loadConfig();
let API_BASE = config.apiBaseUrl || 'http://192.168.1.15:3004';

// ---------- DB ----------
function loadDB() {
  try { return JSON.parse(fs.readFileSync(DB_PATH, 'utf8')); }
  catch { return {}; }
}
function saveDB(db) {
  fs.writeFileSync(DB_PATH, JSON.stringify(db, null, 2));
}

// ---------- Serial ----------
const port = new SerialPort({ path: PORT, baudRate: BAUD, autoOpen: false });
let buf = '';
let readyResolve = null;
let readyPromise = new Promise((r) => { readyResolve = r; });

let cmdResolve = null;     // response untuk perintah {ok:...}
let enrollActive = false;  // sedang dalam sesi enrol (event -> SSE)
let autoActive = false;    // sedang autoscan (event match/nomatch -> SSE)
let pendingEnroll = null;  // {id, name} selama enrol berlangsung
let readyInfo = null;      // payload event ready dari bridge

async function reconnectSerial() {
  try {
    if (port.isOpen) {
      await new Promise((r) => port.close(() => r()));
    }
  } catch {}
  buf = '';
  cmdResolve = null;
  enrollActive = false;
  autoActive = false;
  pendingEnroll = null;
  readyInfo = null;
  readyResolve = null;
  readyPromise = new Promise((r) => { readyResolve = r; });

  try {
    await new Promise((res, rej) => port.open((e) => e ? rej(e) : res()));
    await Promise.race([readyPromise, new Promise((r) => setTimeout(r, 5000))]);
    return { ok: true, info: readyInfo };
  } catch (e) {
    broadcast({ type: 'disconnected' });
    return { ok: false, error: e.message };
  }
}

// SSE clients
const sseClients = new Set();
function broadcast(obj) {
  const line = 'data: ' + JSON.stringify(obj) + '\n\n';
  for (const res of sseClients) {
    try { res.write(line); } catch {}
  }
}

function handleLine(raw) {
  let msg;
  try { msg = JSON.parse(raw); } catch { broadcast({ type: 'raw', text: raw }); return; }

  if (msg.event === 'ready' && readyResolve) {
    readyInfo = msg;
    readyResolve(msg); readyResolve = null;
    broadcast({ type: 'ready', info: msg });
    return;
  }

  if (msg.event === 'match' || msg.event === 'nomatch' || msg.event === 'autoscan_err') {
    if (msg.event === 'match') {
      const db = loadDB();
      const entry = db[String(msg.id)];
      msg.name = typeof entry === 'object' ? (entry.name || '(tidak dikenal)') : (entry || '(tidak dikenal)');
      msg.employeeId = typeof entry === 'object' ? (entry.employeeId || null) : null;

      if (msg.employeeId && config.kode_cabang) {
        const now = new Date();
        const time = now.toTimeString().slice(0, 8);
        apiPost('/api/finger/arduino/attendance', {
          employeeId: msg.employeeId,
          device_id: config.device_id || 'arduino-001',
          kode_cabang: config.kode_cabang,
          time,
        });
      }
    }
    broadcast({ type: 'scan', msg });
    return;
  }

  if (msg.ok === true || msg.ok === false) {
    if (cmdResolve) { const r = cmdResolve; cmdResolve = null; r(msg); return; }
  }

  // event enrol & lainnya
  if (msg.event === 'enrolled' && pendingEnroll) {
    const db = loadDB();
    db[String(msg.id)] = { name: pendingEnroll.name, employeeId: pendingEnroll.employeeId || null };
    saveDB(db);
    broadcast({ type: 'enroll_done', id: msg.id, name: pendingEnroll.name, employeeId: pendingEnroll.employeeId || null });
    pendingEnroll = null;
    enrollActive = false;
    return;
  }
  if (msg.event === 'err' && pendingEnroll) {
    broadcast({ type: 'enroll_failed', msg });
    pendingEnroll = null;
    enrollActive = false;
    return;
  }
  if (msg.event === 'already_registered' && pendingEnroll) {
    broadcast({ type: 'enroll_failed', msg });
    pendingEnroll = null;
    enrollActive = false;
    return;
  }
  broadcast({ type: 'event', msg });
}

port.on('data', (chunk) => {
  buf += chunk.toString();
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const raw = buf.slice(0, i).replace(/\r$/, '').trim();
    buf = buf.slice(i + 1);
    if (raw) handleLine(raw);
  }
});

function send(cmd) { return new Promise((r) => port.write(cmd + '\n', () => r())); }
function sendCmd(cmd, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    cmdResolve = resolve;
    send(cmd).catch(reject);
    setTimeout(() => { if (cmdResolve) { cmdResolve = null; reject(new Error('timeout: ' + cmd)); } }, timeoutMs);
  });
}

// ---------- HTTP ----------
function sendFile(res, filePath, contentType) {
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': contentType });
    res.end(data);
  });
}

function readBody(req) {
  return new Promise((resolve) => {
    let body = '';
    req.on('data', (c) => body += c);
    req.on('end', () => { try { resolve(body ? JSON.parse(body) : {}); } catch { resolve({}); } });
  });
}

function sendJSON(res, obj, status = 200) {
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

function httpModule(urlStr) {
  return urlStr.startsWith('https') ? https : http;
}

function apiProxy(path, res) {
  const url = new URL(path, API_BASE);
  const mod = httpModule(url.protocol);
  const opts = { rejectUnauthorized: false };
  mod.get(url.toString(), opts, (proxyRes) => {
    let data = '';
    proxyRes.on('data', (c) => data += c);
    proxyRes.on('end', () => {
      try {
        const json = JSON.parse(data);
        sendJSON(res, json);
      } catch { sendJSON(res, { success: false, error: 'invalid_response' }, 502); }
    });
  }).on('error', (err) => sendJSON(res, { success: false, error: err.message }, 502));
}

function apiPost(path, body, res) {
  const postData = JSON.stringify(body);
  const url = new URL(path, API_BASE);
  const mod = httpModule(url.protocol);
  const options = {
    hostname: url.hostname,
    port: url.port,
    path: url.pathname + url.search,
    method: 'POST',
    rejectUnauthorized: false,
    headers: {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(postData),
    },
  };
  const req = mod.request(options, (proxyRes) => {
    let data = '';
    proxyRes.on('data', (c) => data += c);
    proxyRes.on('end', () => {
      try {
        const json = JSON.parse(data);
        if (res) sendJSON(res, json);
        else broadcast({ type: 'attendance', msg: json });
      } catch {
        const err = { status: 'error', message: 'invalid_response' };
        if (res) sendJSON(res, err, 502);
        else broadcast({ type: 'attendance', msg: err });
      }
    });
  });
  req.on('error', (err) => {
    const errMsg = { status: 'error', message: err.message };
    if (res) sendJSON(res, errMsg, 502);
    else broadcast({ type: 'attendance', msg: errMsg });
  });
  req.write(postData);
  req.end();
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const p = url.pathname;

  if (req.method === 'GET' && p === '/') {
    return sendFile(res, path.join(__dirname, 'public', 'index.html'), 'text/html; charset=utf-8');
  }
  if (req.method === 'GET' && p === '/api/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
    });
    res.write('\n');
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (req.method === 'GET' && p === '/api/list') {
    return sendJSON(res, loadDB());
  }
  if (req.method === 'GET' && p === '/api/count') {
    try { const r = await sendCmd('COUNT'); return sendJSON(res, r); }
    catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'GET' && p === '/api/status') {
    return sendJSON(res, { ready: readyInfo, autoActive });
  }
  if (req.method === 'POST' && p === '/api/enroll') {
    const body = await readBody(req);
    const name = (body.name || '').trim();
    const employeeId = body.employeeId || null;
    if (!name) return sendJSON(res, { ok: false, error: 'name_empty' }, 400);
    if (enrollActive) return sendJSON(res, { ok: false, error: 'enroll_busy' }, 409);
    try {
      const c = await sendCmd('COUNT');
      const id = (c.count || 0) + 1;
      enrollActive = true;
      pendingEnroll = { id, name, employeeId };
      broadcast({ type: 'enroll_start', id, name, employeeId });
      send('ENROLL ' + id).catch(() => {});
      // enroll selesai dideteksi via event 'enrolled' / 'err' / 'already_registered' di handleLine
      return sendJSON(res, { ok: true, id, name });
    } catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'POST' && p === '/api/autoscan/on') {
    if (autoActive) return sendJSON(res, { ok: false, error: 'already_active' });
    try { const r = await sendCmd('AUTO ON'); autoActive = true; return sendJSON(res, r); }
    catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'POST' && p === '/api/autoscan/off') {
    try { const r = await sendCmd('AUTO OFF'); autoActive = false; return sendJSON(res, r); }
    catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'POST' && p === '/api/delete') {
    const body = await readBody(req);
    const id = parseInt(body.id, 10);
    if (!id) return sendJSON(res, { ok: false, error: 'id_invalid' }, 400);
    try { const r = await sendCmd('DELETE ' + id); if (r.ok) { const db = loadDB(); delete db[String(id)]; saveDB(db); } return sendJSON(res, r); }
    catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'POST' && p === '/api/empty') {
    try { const r = await sendCmd('EMPTY'); if (r.ok) saveDB({}); return sendJSON(res, r); }
    catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'POST' && p === '/api/reconnect') {
    try {
      const r = await reconnectSerial();
      return sendJSON(res, r);
    } catch (e) { return sendJSON(res, { ok: false, error: e.message }, 500); }
  }
  if (req.method === 'GET' && p === '/api/branches') {
    return apiProxy('/api/finger/branches', res);
  }
  if (req.method === 'GET' && p === '/api/employees') {
    const kodeCabang = url.searchParams.get('kode_cabang') || '';
    const query = kodeCabang ? `?kode_cabang=${encodeURIComponent(kodeCabang)}` : '';
    return apiProxy(`/api/finger/employees${query}`, res);
  }
  if (req.method === 'GET' && p === '/api/config') {
    return sendJSON(res, loadConfig());
  }
  if (req.method === 'POST' && p === '/api/config') {
    const body = await readBody(req);
    saveConfigFile(body);
    return sendJSON(res, { ok: true });
  }
  sendJSON(res, { ok: false, error: 'not_found' }, 404);
});

// tangani event enrol selesai untuk update DB -- sudah diintegrasikan di handleLine

server.listen(HTTP_PORT, async () => {
  console.log(`Web GUI: http://localhost:${HTTP_PORT}`);
  console.log(`Membuka ${PORT} @ ${BAUD} baud...`);
  try {
    await new Promise((res, rej) => port.open((e) => e ? rej(e) : res()));
    await Promise.race([readyPromise, new Promise((r) => setTimeout(r, 5000))]);
    console.log('Bridge siap.');
  } catch (e) {
    console.error('Gagal buka serial:', e.message);
  }
});

if (fs.existsSync(CERT_PATH) && fs.existsSync(KEY_PATH)) {
  const sslOptions = {
    cert: fs.readFileSync(CERT_PATH),
    key: fs.readFileSync(KEY_PATH),
  };
  https.createServer(sslOptions, async (req, res) => {
    server.emit('request', req, res);
  }).listen(HTTPS_PORT, () => {
    console.log(`Web GUI (HTTPS): https://localhost:${HTTPS_PORT}`);
  });
} else {
  console.warn('SSL cert/key tidak ditemukan, HTTPS dinonaktifkan. Jalankan: openssl req -x509 -newkey rsa:2048 -keyout certs/key.pem -out certs/cert.pem -days 365 -nodes -subj "/CN=localhost"');
}
