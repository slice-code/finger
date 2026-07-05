'use strict';

const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');
const http = require('http');
const { SerialPort } = require('serialport');

const PORT = process.env.PORT || '/dev/ttyUSB0';
const BAUD = parseInt(process.env.BAUD || '9600', 10);
const DB_PATH = path.join(__dirname, 'fingerprints.json');
const CONFIG_PATH = path.join(__dirname, 'config.json');

function loadConfig() {
  try { return JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8')); }
  catch { return { apiBaseUrl: 'http://192.168.1.15:3004' }; }
}
function saveConfig(cfg) {
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
const readyPromise = new Promise((r) => { readyResolve = r; });

let cmdResolve = null;
let enrollActive = false;
let autoActive = false;
let pendingEnroll = null;
let readyInfo = null;

// windows to broadcast to
const wins = new Set();
function broadcast(channel, obj) {
  for (const w of wins) {
    try { if (!w.isDestroyed()) w.webContents.send(channel, obj); } catch {}
  }
}

function handleLine(raw) {
  let msg;
  try { msg = JSON.parse(raw); } catch { broadcast('bridge-event', { type: 'raw', text: raw }); return; }

  if (msg.event === 'ready' && readyResolve) {
    readyInfo = msg;
    readyResolve(msg); readyResolve = null;
    broadcast('bridge-event', { type: 'ready', info: msg });
    return;
  }

  if (msg.event === 'match' || msg.event === 'nomatch' || msg.event === 'autoscan_err') {
    if (msg.event === 'match') {
      const db = loadDB();
      const entry = db[String(msg.id)];
      msg.name = typeof entry === 'object' ? (entry.name || '(tidak dikenal)') : (entry || '(tidak dikenal)');
      msg.employeeId = typeof entry === 'object' ? (entry.employeeId || null) : null;

      if (msg.employeeId && config.kode_cabang) {
        apiPost('/api/finger/arduino/attendance', {
          employeeId: msg.employeeId,
          device_id: config.device_id || 'arduino-001',
          kode_cabang: config.kode_cabang,
        }).then(apiRes => {
          broadcast('bridge-event', { type: 'attendance', msg: apiRes });
        }).catch(err => {
          broadcast('bridge-event', { type: 'attendance', msg: { status: 'error', message: err.message } });
        });
      }
    }
    broadcast('bridge-event', { type: 'scan', msg });
    return;
  }

  if (msg.ok === true || msg.ok === false) {
    if (cmdResolve) { const r = cmdResolve; cmdResolve = null; r(msg); return; }
  }

  if (msg.event === 'enrolled' && pendingEnroll) {
    const db = loadDB();
    db[String(msg.id)] = { name: pendingEnroll.name, employeeId: pendingEnroll.employeeId || null };
    saveDB(db);
    broadcast('bridge-event', { type: 'enroll_done', id: msg.id, name: pendingEnroll.name, employeeId: pendingEnroll.employeeId || null });
    pendingEnroll = null;
    enrollActive = false;
    return;
  }
  if (msg.event === 'err' && pendingEnroll) {
    broadcast('bridge-event', { type: 'enroll_failed', msg });
    pendingEnroll = null;
    enrollActive = false;
    return;
  }
  if (msg.event === 'already_registered' && pendingEnroll) {
    broadcast('bridge-event', { type: 'enroll_failed', msg });
    pendingEnroll = null;
    enrollActive = false;
    return;
  }
  broadcast('bridge-event', { type: 'event', msg });
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

function apiGet(path) {
  return new Promise((resolve, reject) => {
    const url = new URL(path, API_BASE);
    http.get(url.toString(), (res) => {
      let data = '';
      res.on('data', (c) => data += c);
      res.on('end', () => { try { resolve(JSON.parse(data)); } catch { resolve(data); } });
    }).on('error', reject);
  });
}

function apiPost(path, body) {
  return new Promise((resolve, reject) => {
    const postData = JSON.stringify(body);
    const url = new URL(path, API_BASE);
    const options = {
      hostname: url.hostname,
      port: url.port,
      path: url.pathname + url.search,
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(postData),
      },
    };
    const req = http.request(options, (res) => {
      let data = '';
      res.on('data', (c) => data += c);
      res.on('end', () => { try { resolve(JSON.parse(data)); } catch { resolve(data); } });
    });
    req.on('error', reject);
    req.write(postData);
    req.end();
  });
}

// ---------- IPC ----------
ipcMain.handle('api:list', () => loadDB());
ipcMain.handle('api:count', async () => { try { return await sendCmd('COUNT'); } catch (e) { return { ok: false, error: e.message }; } });
ipcMain.handle('api:status', () => ({ ready: readyInfo, autoActive }));
ipcMain.handle('api:enroll', async (_e, body) => {
  const name = (body && body.name || '').trim();
  const employeeId = (body && body.employeeId) || null;
  if (!name) return { ok: false, error: 'name_empty' };
  if (enrollActive) return { ok: false, error: 'enroll_busy' };
  try {
    const c = await sendCmd('COUNT');
    const id = (c.count || 0) + 1;
    enrollActive = true;
    pendingEnroll = { id, name, employeeId };
    broadcast('bridge-event', { type: 'enroll_start', id, name, employeeId });
    send('ENROLL ' + id).catch(() => {});
    return { ok: true, id, name };
  } catch (e) { return { ok: false, error: e.message }; }
});
ipcMain.handle('api:autoscan/on', async () => {
  if (autoActive) return { ok: false, error: 'already_active' };
  try { const r = await sendCmd('AUTO ON'); autoActive = true; return r; }
  catch (e) { return { ok: false, error: e.message }; }
});
ipcMain.handle('api:autoscan/off', async () => {
  try { const r = await sendCmd('AUTO OFF'); autoActive = false; return r; }
  catch (e) { return { ok: false, error: e.message }; }
});
ipcMain.handle('api:delete', async (_e, body) => {
  const id = parseInt(body && body.id, 10);
  if (!id) return { ok: false, error: 'id_invalid' };
  try { const r = await sendCmd('DELETE ' + id); if (r.ok) { const db = loadDB(); delete db[String(id)]; saveDB(db); } return r; }
  catch (e) { return { ok: false, error: e.message }; }
});
ipcMain.handle('api:empty', async () => {
  try { const r = await sendCmd('EMPTY'); if (r.ok) saveDB({}); return r; }
  catch (e) { return { ok: false, error: e.message }; }
});
ipcMain.handle('api:branches', () => apiGet('/api/finger/branches'));
ipcMain.handle('api:employees', (_e, kodeCabang) => {
  const query = kodeCabang ? `?kode_cabang=${encodeURIComponent(kodeCabang)}` : '';
  return apiGet(`/api/finger/employees${query}`);
});
ipcMain.handle('api:getConfig', () => loadConfig());
ipcMain.handle('api:setConfig', (_e, cfg) => {
  saveConfig(cfg);
  return { ok: true };
});

// ---------- Window ----------
function createWindow() {
  const w = new BrowserWindow({
    width: 1180,
    height: 760,
    backgroundColor: '#0b1120',
    title: 'FPM10A • Finger Console',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  wins.add(w);
  w.on('closed', () => wins.delete(w));
  w.loadFile(path.join(__dirname, 'public', 'index.html'));
}

app.whenReady().then(async () => {
  console.log(`Membuka ${PORT} @ ${BAUD} baud...`);
  try {
    await new Promise((res, rej) => port.open((e) => e ? rej(e) : res()));
    await Promise.race([readyPromise, new Promise((r) => setTimeout(r, 5000))]);
    console.log('Bridge siap.');
  } catch (e) {
    console.error('Gagal buka serial:', e.message);
  }
  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', () => {
  try { if (port.isOpen) port.close(); } catch {}
});