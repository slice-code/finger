#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');
const readline = require('readline');

const PORT = process.env.PORT || '/dev/ttyUSB0';
const BAUD = parseInt(process.env.BAUD || '9600', 10);
const DB_PATH = path.join(__dirname, 'fingerprints.json');

function loadDB() {
  try { return JSON.parse(fs.readFileSync(DB_PATH, 'utf8')); }
  catch { return {}; }
}
function saveDB(db) {
  fs.writeFileSync(DB_PATH, JSON.stringify(db, null, 2));
}

const port = new SerialPort({ path: PORT, baudRate: BAUD, autoOpen: false });
let buf = '';
let readyResolve = null;
const readyPromise = new Promise((r) => { readyResolve = r; });

let onMatch = null;       // dipanggil saat event match/nomatch (mode auto)
let onEvent = null;       // dipanggil untuk event lain (enroll, dll)
let cmdResolve = null;    // response untuk perintah non-event

function handleLine(raw) {
  let msg;
  try { msg = JSON.parse(raw); } catch { console.log('[arduino]', raw); return; }

  if (msg.event === 'ready' && readyResolve) {
    readyResolve(msg); readyResolve = null; return;
  }

  if (msg.event === 'match' || msg.event === 'nomatch' || msg.event === 'autoscan_err') {
    if (onMatch) onMatch(msg);
    else console.log('[scan]', JSON.stringify(msg));
    return;
  }

  if (msg.ok === true || msg.ok === false) {
    // response perintah
    if (cmdResolve) { const r = cmdResolve; cmdResolve = null; r(msg); return; }
  }

  // event lain (place/remove/place_again/enrolled/err/...)
  if (onEvent) onEvent(msg);
  else console.log('[event]', JSON.stringify(msg));
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
function sendCmd(cmd) {
  return new Promise((resolve, reject) => {
    cmdResolve = resolve;
    send(cmd).catch(reject);
    setTimeout(() => { if (cmdResolve) { cmdResolve = null; reject(new Error('timeout: ' + cmd)); } }, 10000);
  });
}

// enroll: setelah nama dimasukkan, tunggu image jari dari module.
// jika module mengirim image valid -> lanjut daftar; jika kosong/bukan image -> tetap menunggu.
function enroll(id) {
  return new Promise((resolve) => {
    console.log('Menunggu jari di sensor... (letakkan jari untuk mendaftar)');
    onEvent = (msg) => {
      if (msg.event === 'waiting_clear') console.log('  Menunggu jari diangkat (jika masih menempel)...');
      else if (msg.event === 'waiting_finger') console.log('  Letakkan jari di sensor...');
      else if (msg.event === 'waiting_finger_2') console.log('  Letakkan jari yang SAMA lagi...');
      else if (msg.event === 'retry_create') console.log('  Gagal mencocokkan (kode ' + msg.code + '), mengulang capture (percobaan ' + msg.attempt + ')... letakkan jari lagi');
      else if (msg.event === 'dbg') console.log('  [debug] phase ' + msg.phase + ' kode ' + msg.code);
      else if (msg.event === 'diag') console.log('  [diag] ' + msg.step + ' -> kode ' + msg.code);
      else if (msg.event === 'image_ok_step1') console.log('  Image jari pertama diterima, memproses...');
      else if (msg.event === 'already_registered') {
        const name = loadDB()[String(msg.id)] || '(tidak dikenal)';
        console.log(`  Jari SUDAH TERDAFTAR! ID ${msg.id} - ${name} (confidence ${msg.confidence})`);
        console.log('  Pendaftaran dibatalkan. Gunakan ID lain atau hapus data lama.');
        onEvent = null;
        resolve({ ok: false, error: 'already_registered', id: msg.id });
      }
      else if (msg.event === 'remove') console.log('  Angkat jari...');
      else if (msg.event === 'image_ok_step2') console.log('  Image jari kedua diterima, membuat model...');
      else if (msg.event === 'enrolled') { onEvent = null; resolve({ ok: true, id: msg.id }); }
      else if (msg.event === 'err') { onEvent = null; resolve({ ok: false, error: msg.step, code: msg.code }); }
      else console.log('  ->', JSON.stringify(msg));
    };
    send('ENROLL ' + id);
  });
}

const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
rl.on('close', () => process.exit(0));
function ask(q) { return new Promise((r) => rl.question(q, r)); }

async function menuDaftar() {
  const db = loadDB();
  const count = (await sendCmd('COUNT')).count || 0;
  const id = count + 1;
  console.log(`\n--- DAFTAR SIDIK JARI (ID ${id}) ---`);
  const name = (await ask('Nama pemilik: ')).trim();
  if (!name) { console.log('Dibatalkan.'); return; }

  const res = await enroll(id);
  if (res.ok) {
    db[String(id)] = name;
    saveDB(db);
    console.log(`Sukses! "${name}" tersimpan dengan ID ${id}.`);
  } else {
    console.log('Gagal mendaftar:', res.error);
  }
}async function menuAutoScan() {
  const db = loadDB();
  console.log('\n--- AUTO SCAN ---');
  console.log('Mode auto-scan aktif. Letakkan jari untuk mencocokkan.');
  console.log('Tekan Ctrl+C untuk berhenti.\n');

  return new Promise((resolve) => {
    let stopped = false;
    const stopHandler = () => {
      if (stopped) return; stopped = true;
      process.stdin.pause();
      process.removeListener('SIGINT', sigHandler);
      onMatch = null;
      sendCmd('AUTO OFF').then(() => resolve()).catch(() => resolve());
    };
    const sigHandler = () => stopHandler();

    onMatch = (msg) => {
      if (msg.event === 'match') {
        const name = db[String(msg.id)] || '(tidak dikenal)';
        console.log(`[MATCH] ID ${msg.id} - ${name}  (confidence ${msg.confidence})`);
      } else if (msg.event === 'nomatch') {
        console.log('[NO MATCH] Sidik jari tidak dikenali');
      } else if (msg.event === 'autoscan_err') {
        console.log('[ERROR] kode', msg.code);
      }
    };

    process.stdin.setRawMode(true);
    process.stdin.resume();
    process.stdin.once('data', () => stopHandler());
    process.on('SIGINT', sigHandler);

    sendCmd('AUTO ON').catch(() => {});
  });
}

async function menuList() {
  const db = loadDB();
  const ids = Object.keys(db).sort((a, b) => a - b);
  console.log('\n--- DAFTAR TERDAFTAR ---');
  if (!ids.length) { console.log('(kosong)'); return; }
  for (const id of ids) console.log(`  ID ${id}: ${db[id]}`);
}

async function menuHapus() {
  const db = loadDB();
  const ids = Object.keys(db).sort((a, b) => a - b);
  if (!ids.length) { console.log('Data kosong.'); return; }
  for (const id of ids) console.log(`  ID ${id}: ${db[id]}`);
  const idStr = (await ask('ID yang dihapus (atau "all"): ')).trim();
  if (idStr.toLowerCase() === 'all') {
    await sendCmd('EMPTY');
    saveDB({});
    console.log('Semua data dihapus.');
    return;
  }
  const id = parseInt(idStr, 10);
  if (!id) { console.log('ID tidak valid.'); return; }
  const r = await sendCmd('DELETE ' + id);
  if (r.ok) { delete db[String(id)]; saveDB(db); console.log(`ID ${id} dihapus.`); }
  else console.log('Gagal hapus:', JSON.stringify(r));
}

function printMenu() {
  console.log('\n===== FINGER APP =====');
  console.log('1. Daftar sidik jari');
  console.log('2. Auto scan (cari kecocokan)');
  console.log('3. Lihat daftar terdaftar');
  console.log('4. Hapus data');
  console.log('5. Keluar');
}

async function main() {
  console.log(`Membuka ${PORT} @ ${BAUD} baud...`);
  await new Promise((res, rej) => port.open((e) => e ? rej(e) : res()));
  await Promise.race([readyPromise, new Promise((r) => setTimeout(r, 4000))]);
  console.log('Bridge siap.');

  while (true) {
    printMenu();
    const choice = (await ask('Pilih: ')).trim();
    try {
      if (choice === '1') await menuDaftar();
      else if (choice === '2') await menuAutoScan();
      else if (choice === '3') await menuList();
      else if (choice === '4') await menuHapus();
      else if (choice === '5') { console.log('Keluar.'); process.exit(0); }
      else console.log('Pilihan tidak valid.');
    } catch (e) {
      console.error('Error:', e.message);
    }
  }
}

main().catch((e) => { console.error('Gagal:', e.message); process.exit(1); });
