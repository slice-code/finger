#!/usr/bin/env node
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');

const PORT = process.env.PORT || '/dev/ttyUSB0';
const BAUD = parseInt(process.env.BAUD || '9600', 10);
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '3000', 10);
const DB_PATH = path.join(__dirname, 'fingerprints.json');

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
const readyPromise = new Promise((r) => { readyResolve = r; });

let cmdResolve = null;     // response untuk perintah {ok:...}
let enrollActive = false;  // sedang dalam sesi enrol (event -> SSE)
let autoActive = false;    // sedang autoscan (event match/nomatch -> SSE)
let pendingEnroll = null;  // {id, name} selama enrol berlangsung
let readyInfo = null;      // payload event ready dari bridge

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
      msg.name = db[String(msg.id)] || '(tidak dikenal)';
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
    db[String(msg.id)] = pendingEnroll.name;
    saveDB(db);
    broadcast({ type: 'enroll_done', id: msg.id, name: pendingEnroll.name });
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
    if (!name) return sendJSON(res, { ok: false, error: 'name_empty' }, 400);
    if (enrollActive) return sendJSON(res, { ok: false, error: 'enroll_busy' }, 409);
    try {
      const c = await sendCmd('COUNT');
      const id = (c.count || 0) + 1;
      enrollActive = true;
      pendingEnroll = { id, name };
      broadcast({ type: 'enroll_start', id, name });
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
