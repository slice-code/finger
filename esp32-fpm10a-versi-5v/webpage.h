#ifndef WEBPAGE_H
#define WEBPAGE_H

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PJTKI Finger</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Outfit:wght@400;500;600;700;800&family=IBM+Plex+Mono:wght@400;500&display=swap');
*{margin:0;padding:0;box-sizing:border-box}
:root{
/* PJTKI layout.js — teal #0e7490 + cyan #38bdf8 + slate #0f172a */
--bg:#0b1220;
--bg2:#0f172a;
--card:#132337;
--border:rgba(56,189,248,.16);
--border2:rgba(56,189,248,.28);
--cyan:#38bdf8;
--teal:#0e7490;
--teal-light:#a5e1ec;
--blue:#3b82f6;
--green:#22c55e;
--red:#ef4444;
--yellow:#f59e0b;
--dim:#94a3b8;
--dim2:#64748b;
--text:#e2e8f0;
--radius:14px;
}
html,body{min-height:100%}
body{
font-family:'Outfit',system-ui,sans-serif;
background:
radial-gradient(1000px 420px at 8% -8%,rgba(14,116,144,.28),transparent 55%),
radial-gradient(800px 380px at 100% 0%,rgba(56,189,248,.12),transparent 45%),
linear-gradient(180deg,#0b1220 0%,#0f172a 50%,#0b1424 100%);
color:var(--text);-webkit-font-smoothing:antialiased;
}
.shell{max-width:680px;margin:0 auto;padding-bottom:24px}
.topbar{
background:rgba(15,23,42,.88);backdrop-filter:blur(12px);
border-bottom:1px solid var(--border);
padding:14px 16px;display:flex;align-items:center;justify-content:space-between;
position:sticky;top:0;z-index:100;gap:10px;
}
.brand{display:flex;align-items:center;gap:10px}
.brand-mark{
width:40px;height:40px;border-radius:12px;
background:linear-gradient(145deg,#38bdf8,#0e7490);
box-shadow:0 8px 20px rgba(14,116,144,.4),inset 0 1px 0 rgba(255,255,255,.25);
display:grid;place-items:center;
}
.brand-mark svg{width:22px;height:22px}
.topbar h1{font-size:18px;font-weight:800;letter-spacing:-.3px;
background:linear-gradient(135deg,var(--cyan),var(--teal-light));
-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}
.topbar .sub{font-size:11px;color:var(--dim);margin-top:2px;font-weight:500}
.pill{display:inline-flex;align-items:center;gap:6px;padding:5px 11px;border-radius:999px;font-size:11px;font-weight:600;
background:rgba(19,35,55,.9);border:1px solid var(--border)}
.dot{width:8px;height:8px;border-radius:50%}
.dot-g{background:var(--green);box-shadow:0 0 8px rgba(34,197,94,.55)}
.dot-r{background:var(--red);box-shadow:0 0 8px rgba(239,68,68,.45)}
.dot-y{background:var(--yellow);box-shadow:0 0 8px rgba(245,158,11,.45)}
.tabs{display:flex;gap:4px;overflow-x:auto;padding:8px 12px 10px;scrollbar-width:none}
.tabs::-webkit-scrollbar{display:none}
.tab{flex:0 0 auto;min-width:72px;padding:10px 12px;text-align:center;cursor:pointer;font-size:12px;font-weight:700;
color:var(--dim2);border-radius:999px;border:1px solid transparent;transition:.2s;white-space:nowrap}
.tab:hover{color:var(--text);background:rgba(56,189,248,.06)}
.tab.on{color:#0f172a;background:linear-gradient(135deg,#67e8f9,#0e7490);box-shadow:0 8px 18px rgba(14,116,144,.3)}
.page{display:none;padding:0 14px;animation:rise .25s ease}.page.on{display:block}
@keyframes rise{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.hero{margin:4px 0 12px;padding:18px 16px;border-radius:18px;position:relative;overflow:hidden;
background:linear-gradient(135deg,rgba(14,116,144,.92),rgba(15,23,42,.95));
border:1px solid var(--border2);box-shadow:0 14px 36px rgba(0,0,0,.28)}
.hero::after{content:'';position:absolute;right:-24px;top:-30px;width:140px;height:140px;border-radius:50%;
border:14px solid rgba(56,189,248,.1);box-shadow:0 0 0 28px rgba(14,116,144,.07)}
.hero-kicker{font-size:10px;font-weight:700;letter-spacing:1.2px;text-transform:uppercase;color:var(--teal-light);margin-bottom:6px}
.hero h2{font-size:22px;font-weight:800;letter-spacing:-.4px;line-height:1.2;max-width:16ch;position:relative;z-index:1}
.hero .sub{margin-top:6px;color:rgba(226,232,240,.75);font-size:12px;line-height:1.45;max-width:36ch;position:relative;z-index:1}
.card{background:linear-gradient(180deg,rgba(19,35,55,.95),rgba(15,23,42,.92));border:1px solid var(--border);
border-radius:var(--radius);padding:16px;margin-bottom:12px;box-shadow:0 10px 24px rgba(0,0,0,.22)}
.card h3{font-size:11px;font-weight:700;color:var(--dim);margin-bottom:12px;text-transform:uppercase;letter-spacing:.7px;
display:flex;align-items:center;gap:8px}
.card h3::before{content:'';width:7px;height:7px;border-radius:50%;background:var(--cyan);box-shadow:0 0 0 3px rgba(56,189,248,.15)}
.stat{font-size:28px;font-weight:800;letter-spacing:-1px}
.stat-c{color:var(--cyan)}.stat-g{color:var(--green)}.stat-r{color:var(--red)}
.btn{display:inline-flex;align-items:center;justify-content:center;padding:11px 18px;border:none;border-radius:12px;
font-size:13px;font-weight:700;cursor:pointer;transition:.15s;width:100%;margin-top:8px;font-family:inherit}
.btn:active{transform:scale(.98)}
.btn-c{background:linear-gradient(135deg,#38bdf8,#0e7490);color:#0f172a;box-shadow:0 8px 20px rgba(14,116,144,.32)}
.btn-c:hover{filter:brightness(1.06)}
.btn-g{background:linear-gradient(135deg,#22d3ee,#0e7490);color:#0f172a}
.btn-r{background:linear-gradient(135deg,#f87171,#dc2626);color:#fff}
.btn-o{background:transparent;border:1.5px solid var(--border2);color:var(--text)}
.btn-o:hover{background:rgba(56,189,248,.08);border-color:var(--cyan);color:var(--cyan)}
.btn:disabled{opacity:.4;cursor:not-allowed;transform:none!important}
input,select{width:100%;padding:11px 12px;background:rgba(11,18,32,.7);border:1.5px solid var(--border);
border-radius:10px;color:var(--text);font-size:13px;margin-top:6px;outline:none;font-family:inherit}
input:focus,select:focus{border-color:var(--cyan);box-shadow:0 0 0 3px rgba(56,189,248,.15)}
label{font-size:12px;color:var(--dim);margin-top:10px;display:block;font-weight:600}
.scan-box{text-align:center;padding:28px 16px;border-radius:16px;border:2px solid var(--border);transition:.3s;
background:rgba(11,18,32,.45)}
.scan-box.active{border-color:rgba(56,189,248,.55);box-shadow:0 0 30px rgba(56,189,248,.15)}
.scan-box.ok{border-color:rgba(34,197,94,.55);box-shadow:0 0 30px rgba(34,197,94,.15)}
.scan-box.fail{border-color:rgba(239,68,68,.55);box-shadow:0 0 30px rgba(239,68,68,.15)}
.scan-icon{font-size:48px;margin-bottom:12px}
.scan-icon.ring,.scan-box.active .scan-icon{animation:pulse 1.5s infinite}
@keyframes pulse{0%,100%{opacity:.65;transform:scale(1)}50%{opacity:1;transform:scale(1.05)}}
.scan-badge{display:inline-block;padding:4px 14px;border-radius:999px;font-size:11px;font-weight:700;margin:8px 0;letter-spacing:.4px}
.badge-scan{background:rgba(56,189,248,.14);color:var(--cyan);border:1px solid rgba(56,189,248,.28)}
.badge-ok{background:rgba(34,197,94,.14);color:var(--green);border:1px solid rgba(34,197,94,.28)}
.badge-fail{background:rgba(239,68,68,.14);color:var(--red);border:1px solid rgba(239,68,68,.28)}
.badge-idle{background:rgba(148,163,184,.12);color:var(--dim);border:1px solid rgba(148,163,184,.2)}
.scan-name{font-size:18px;font-weight:700;margin:4px 0}
.log{max-height:200px;overflow-y:auto;font-family:'IBM Plex Mono',ui-monospace,monospace;font-size:11px;
background:rgba(11,18,32,.75);border:1px solid var(--border);border-radius:10px;padding:8px;margin-top:8px}
.log div{padding:3px 4px;border-bottom:1px solid rgba(56,189,248,.06)}
.log .t{color:var(--dim2)}.log .ok{color:var(--green)}.log .er{color:var(--red)}.log .cy{color:var(--cyan)}.log .wa{color:var(--yellow)}
table{width:100%;border-collapse:collapse;font-size:12px}
th{text-align:left;padding:9px 10px;color:var(--dim);border-bottom:1px solid var(--border);font-size:10px;text-transform:uppercase;letter-spacing:.5px}
td{padding:9px 10px;border-bottom:1px solid rgba(56,189,248,.07)}
.del-btn{background:none;border:none;color:var(--red);cursor:pointer;font-size:16px;padding:4px 8px;border-radius:6px}
.del-btn:hover{background:rgba(239,68,68,.12)}
.empty-state{text-align:center;padding:36px;color:var(--dim)}
.sync-row{display:flex;align-items:flex-start;gap:10px;padding:10px 12px;background:rgba(11,18,32,.55);
border:1.5px solid var(--border);border-radius:10px;margin-bottom:6px}
.sync-row input{width:auto;margin:2px 0 0;padding:0;accent-color:var(--cyan)}
.sync-row .meta{flex:1;min-width:0}
.sync-row .nm{font-weight:700;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sync-row .id{font-size:11px;color:var(--dim);font-family:'IBM Plex Mono',monospace;margin-top:2px}
.sync-row .cab{font-size:10px;color:var(--cyan);font-weight:600;margin-top:3px}
.sync-tools{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}
.sync-tools .btn{width:auto;flex:1;min-width:120px;margin-top:0}
.muted{font-size:13px;color:var(--dim);margin-bottom:8px;line-height:1.45}
.wifi-item{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;background:rgba(11,18,32,.55);
border:1.5px solid var(--border);border-radius:10px;margin-bottom:6px;cursor:pointer;transition:.15s}
.wifi-item:hover,.wifi-item.selected{border-color:var(--cyan);background:rgba(56,189,248,.08)}
.wifi-ssid{font-weight:700;font-size:13px}
.wifi-signal{font-size:11px;color:var(--dim2)}
.wifi-lock{color:var(--yellow);font-size:12px}
.muted{font-size:12px;color:var(--dim);line-height:1.5;margin-bottom:10px}
.foot{text-align:center;padding:16px;color:var(--dim2);font-size:11px}
</style>
</head>
<body>
<div class="shell">
<div class="topbar">
  <div class="brand">
    <div class="brand-mark" aria-hidden="true">
      <svg viewBox="0 0 24 24" fill="none"><path d="M12 3c-1.2 2.2-1.8 3.8-1.8 5.4 0 1.7.9 2.8 2.1 2.8 1.4 0 2.2-1.3 2.2-3.1 0-1.6-.6-3.2-2.5-5.1Z" stroke="#0c4a6e" stroke-width="1.7"/><path d="M7.2 8.2c-.4 1.5-.5 2.7-.2 3.8.5 1.7 1.8 2.7 3.4 2.7" stroke="#0c4a6e" stroke-width="1.7" stroke-linecap="round"/><path d="M16.8 8.5c.5 1.5.6 2.7.2 3.9-.5 1.6-1.8 2.5-3.3 2.5" stroke="#0c4a6e" stroke-width="1.7" stroke-linecap="round"/><path d="M5.4 12.4c-.2 1.4 0 2.7.7 3.9 1 1.8 2.8 2.8 5 2.8" stroke="#0c4a6e" stroke-width="1.7" stroke-linecap="round"/><path d="M18.5 12.6c.3 1.4.1 2.7-.6 3.9-1 1.7-2.8 2.7-4.9 2.7" stroke="#0c4a6e" stroke-width="1.7" stroke-linecap="round"/><path d="M12 14.2v6.2" stroke="#0c4a6e" stroke-width="1.7" stroke-linecap="round"/></svg>
    </div>
    <div>
      <h1>PJTKI Finger</h1>
      <div class="sub">Absensi sidik jari BLK</div>
    </div>
  </div>
  <div style="display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end">
    <span class="pill"><span class="dot" id="sdot"></span><span id="stxt">OFFLINE</span></span>
    <span class="pill" id="tpill">0 templates</span>
  </div>
</div>
<div class="tabs">
  <div class="tab on" onclick="go('dash')">Dashboard</div>
  <div class="tab" onclick="go('enroll')">Daftar</div>
  <div class="tab" onclick="go('data')">Data</div>
  <div class="tab" onclick="go('wifi')">WiFi</div>
  <div class="tab" onclick="go('setel')">Setelan</div>
  <div class="tab" onclick="go('akun')">Akun</div>
  <div class="tab" onclick="go('cadangan')">Cadangan</div>
</div>

<div class="page on" id="p-dash">
  <div class="hero">
    <div class="hero-kicker">Perangkat siap</div>
    <h2>Daftarkan &amp; absenkan TKI dengan satu sentuhan</h2>
    <p class="sub">Sensor FPM10A terhubung ke server PJTKI. Scan selalu aktif.</p>
  </div>
  <div class="card"><h3>Status</h3>
    <div style="display:flex;gap:12px">
      <div style="flex:1"><div class="stat stat-c" id="dcnt">-</div><div style="font-size:12px;color:var(--dim)">Templates</div></div>
      <div style="flex:1"><div class="stat stat-g" id="dscan">IDLE</div><div style="font-size:12px;color:var(--dim)">Scan Mode</div></div>
    </div>
  </div>
  <div class="card"><h3>WiFi Status</h3>
    <div style="display:flex;gap:12px">
      <div style="flex:1"><div class="stat" id="dwmode" style="font-size:18px">AP</div><div style="font-size:12px;color:var(--dim)">Mode</div></div>
      <div style="flex:1"><div style="font-size:14px;color:var(--text)" id="dwip">192.168.4.1</div><div style="font-size:12px;color:var(--dim)">IP</div></div>
    </div>
  </div>
  <div class="card"><h3>Quick Actions</h3>
    <button class="btn btn-c" onclick="go('enroll')">Daftar Sidik Jari Baru</button>
  </div>
  <div class="card"><h3>Activity Log</h3><div class="log" id="elog"></div></div>
</div>

<div class="page" id="p-enroll">
  <div class="card"><h3>Daftar Sidik Jari</h3>
    <p class="muted">Setelah berhasil, data ikut tersimpan ke server PJTKI via API register.</p>
    <label>Cabang</label>
    <select id="ebranch" onchange="loadEmpList(this.value)"><option value="">-- Pilih Cabang --</option></select>
    <label>Karyawan</label>
    <select id="eemp" disabled><option value="">-- Pilih Cabang dulu --</option></select>
    <button class="btn btn-o" onclick="loadBranchList()" style="margin-top:10px">Refresh Daftar</button>
    <button class="btn btn-c" id="enrollBtn" onclick="startEnroll()" style="margin-top:6px">Mulai Daftar</button>
  </div>
  <div class="card"><h3>Progress</h3>
    <div id="eprog" style="text-align:center;padding:20px;color:var(--dim)">Menunggu...</div>
  </div>
  <div class="card"><h3>Log</h3><div class="log" id="elog2"></div></div>
</div>

<div class="page" id="p-scan">
  <div class="card">
    <div class="scan-box active" id="sbox">
      <div class="scan-icon" id="sicon">&#x1f463;</div>
      <div class="scan-badge badge-scan" id="sbadge">SCANNING</div>
      <div class="scan-name" id="sname">Menempelkan jari...</div>
      <div style="font-size:12px;color:var(--dim)" id="sconf"></div>
    </div>
  </div>
  <div class="card"><h3>Scan Log</h3><div class="log" id="slog"></div></div>
</div>

<div class="page" id="p-data">
  <div class="card"><h3>Data Terdaftar (<span id="dcnt2">0</span>)</h3>
    <input id="dsearch" placeholder="Cari..." oninput="filterData()">
    <div style="overflow-x:auto;margin-top:8px">
      <table><thead><tr><th>ID</th><th>Nama</th><th>Karyawan</th><th></th></tr></thead>
      <tbody id="dtbody"></tbody></table>
    </div>
    <div class="empty-state" id="dempty">Belum ada data</div>
    <button class="btn btn-r" onclick="emptyAll()">Hapus Semua</button>
  </div>
</div>

<div class="page" id="p-wifi">
  <div class="card"><h3>WiFi Status</h3>
    <div style="display:flex;gap:12px">
      <div style="flex:1"><div class="stat" id="wmode" style="font-size:18px">AP</div><div style="font-size:12px;color:var(--dim)">Mode</div></div>
      <div style="flex:1"><div style="font-size:14px;color:var(--dim)" id="wsta">-</div><div style="font-size:12px;color:var(--dim)">Connected</div></div>
    </div>
  </div>
  <div class="card"><h3>Jaringan Tersimpan</h3>
    <div id="wsaved"></div>
    <button class="btn btn-r" id="wresetBtn" onclick="resetWifi()" style="display:none;margin-top:8px">Hapus Semua & Reboot</button>
  </div>
  <div class="card"><h3>Scan Network</h3>
    <button class="btn btn-o" onclick="scanWifi()">Scan</button>
    <div id="wlist" style="margin-top:8px"></div>
  </div>
  <div class="card"><h3>Tambah Jaringan Baru</h3>
    <label>SSID</label><input id="wssid" placeholder="Pilih dari scan di atas" readonly>
    <label>Password</label><input id="wpass" type="password" placeholder="Password">
    <button class="btn btn-c" onclick="saveWifi()">Simpan & Reboot</button>
  </div>
</div>

<div class="page" id="p-setel">
  <div class="card"><h3>Pengaturan API</h3>
    <label>API Server URL</label><input id="sapi" placeholder="http://192.168.1.15:3004">
    <label>Kode Cabang</label><input id="scab" placeholder="CKS">
    <label>Device ID</label><input id="sdev" placeholder="arduino-001">
    <button class="btn btn-c" onclick="saveSettings()">Simpan Setelan</button>
  </div>
  <div class="card"><h3>Status</h3>
    <div style="font-size:13px;color:var(--dim)">
      <div>WiFi Mode: <span id="sfg-wmode" style="color:var(--text)">-</span></div>
      <div>IP: <span id="sfg-ip" style="color:var(--text)">-</span></div>
      <div>Fingerprint: <span id="sfg-fp" style="color:var(--text)">-</span></div>
    </div>
  </div>
</div>

<div class="page" id="p-akun">
  <div class="card"><h3>Autentikasi Web</h3>
    <label>Username</label><input id="akuser" placeholder="Username login web">
    <label>Password Baru</label><input id="akpass" type="password" placeholder="Kosongkan jika tidak diubah">
    <button class="btn btn-c" onclick="saveCred()">Simpan</button>
  </div>
  <div class="card"><h3>WiFi AP</h3>
    <label>Password AP Baru</label><input id="akapass" type="password" placeholder="Kosongkan jika tidak diubah">
    <div style="font-size:12px;color:var(--dim);margin-top:4px">SSID: FPM10A-Bridge (tetap)</div>
  </div>
  <div class="card"><h3>NTP (Waktu)</h3>
    <label>NTP Server</label><input id="akntp" placeholder="id.pool.ntp.org">
    <label>UTC Offset (detik)</label><input id="akoff" type="number" placeholder="25200 (WIB)">
    <div style="font-size:12px;color:var(--dim);margin-top:4px">
      Waktu sekarang: <span id="aktime" style="color:var(--cyan)">--:--:--</span>
      <span id="aksync" style="color:var(--dim)">(belum sync)</span>
    </div>
    <button class="btn btn-c" onclick="saveCred()">Simpan & Reboot</button>
  </div>
</div>

<div class="page" id="p-cadangan">
  <div class="card"><h3>Sinkron dari Server PJTKI</h3>
    <p class="muted">Pilih anak (bisa lintas cabang) yang punya template hex di server, lalu tulis ke sensor. Yang sudah ada di device di-skip. Setelan API URL + WiFi STA wajib.</p>
    <label>Filter cabang (kosong = semua)</label>
    <select id="syncCabang"><option value="">— Semua cabang —</option></select>
    <label>Cari nama / ID biodata</label>
    <input id="syncQ" type="text" placeholder="Contoh: Siti atau CKSLBK-TW-0001" onkeydown="if(event.key==='Enter')loadServerTemplates()">
    <div class="sync-tools">
      <button class="btn btn-o" onclick="loadServerTemplates()">Muat Daftar</button>
      <button class="btn btn-o" onclick="syncSelectAll(true)">Pilih Semua</button>
      <button class="btn btn-o" onclick="syncSelectAll(false)">Batal Pilih</button>
    </div>
    <div id="syncList" style="margin-top:10px;max-height:280px;overflow-y:auto"></div>
    <button class="btn btn-c" id="syncBtn" onclick="syncFromServer()" disabled>Sinkron yang Dipilih</button>
    <div id="syncProg" style="margin-top:10px;text-align:center;color:var(--dim);font-size:13px"></div>
  </div>
  <div class="card"><h3>Cadangan / Backup</h3>
    <p style="font-size:13px;color:var(--dim);margin-bottom:8px">
      Unduh data metadata sidik jari (nama + ID karyawan) sebagai file JSON.
      Template biometric tetap aman di sensor FPM10A &mdash; tidak ikut diunduh.
    </p>
    <button class="btn btn-c" onclick="downloadBackup()">Unduh Metadata (.json)</button>
    <button class="btn btn-g" onclick="downloadBackupFull()" style="margin-top:4px">Unduh Lengkap + Template (.json)</button>
  </div>
  <div class="card"><h3>Pulihkan / Restore</h3>
    <p style="font-size:13px;color:var(--dim);margin-bottom:8px">
      Unggah file cadangan untuk memulihkan data nama &amp; ID &amp; template sidik jari.
      <strong style="color:var(--green)">Template di sensor TIDAK akan dihapus atau ditimpa.</strong>
      Template yang belum ada di sensor akan ditulis ulang dari data cadangan.
    </p>
    <label>Pilih file .json (metadata atau full)</label>
    <input type="file" id="restoreFile" accept=".json" onchange="handleRestoreFile(this)">
    <button class="btn btn-g" id="restoreBtn" onclick="startRestore()" disabled>Mulai Pulihkan</button>
    <div id="rprog" style="margin-top:10px;text-align:center;color:var(--dim);font-size:13px"></div>
    <div id="rresult" style="margin-top:8px"></div>
  </div>
  <div class="card"><h3>Log</h3><div class="log" id="rlog"></div></div>
</div>

<script>
var autoOn=false;
function go(s){document.querySelectorAll('.page').forEach(p=>p.classList.remove('on'));
document.getElementById('p-'+s).classList.add('on');
document.querySelectorAll('.tab').forEach((t,i)=>{t.classList.toggle('on',['dash','enroll','data','wifi','setel','akun','cadangan'][i]===s)});
if(s==='data')loadData();if(s==='wifi')loadWifiStatus();if(s==='setel')loadSettings();if(s==='akun')loadCred();if(s==='enroll')loadBranchList();if(s==='cadangan'){loadSyncCabang();loadServerTemplates()}}
function addLog(el,cls,txt){var d=document.getElementById(el);var m=document.createElement('div');
m.innerHTML='<span class="t">'+new Date().toLocaleTimeString()+'</span> <span class="'+cls+'">'+txt+'</span>';
d.prepend(m);if(d.children.length>50)d.lastChild.remove()}
function api(path,method,body){
return fetch(path,{method:method||'GET',headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined}).then(r=>r.json())}

function updStatus(){
api('/api/status').then(d=>{
document.getElementById('sdot').className='dot '+(d.ready?'dot-g':'dot-r');
document.getElementById('stxt').textContent=d.ready?(d.autoActive?'SCANNING':'SIAP'):'OFFLINE';
document.getElementById('tpill').textContent=d.count+' templates';
document.getElementById('dcnt').textContent=d.count;
document.getElementById('dscan').textContent=d.autoActive?'ACTIVE':'ENROLL';
document.getElementById('dscan').className='stat '+(d.autoActive?'stat-g':'stat-r');
document.getElementById('dwmode').textContent=d.wifiMode;
document.getElementById('dwmode').style.color=d.wifiMode==='STA'?'var(--green)':'var(--yellow)';
document.getElementById('dwip').textContent=d.wifiMode==='STA'?d.staIP:'192.168.4.1';
var e=document.getElementById('sfg-wmode');if(e)e.textContent=d.wifiMode;
var e=document.getElementById('sfg-ip');if(e)e.textContent=d.wifiMode==='STA'?d.staIP:'192.168.4.1';
var e=document.getElementById('sfg-fp');if(e)e.textContent=d.count+' templates | baud:'+d.baud;
autoOn=d.autoOn;
}).catch(()=>{})}

function startEnroll(){
var esel=document.getElementById('eemp');
var empId=esel.value;
var empName=esel.options[esel.selectedIndex]?esel.options[esel.selectedIndex].text:'';
var empNameClean=empName.replace(/\s*\([^)]*\)\s*$/,'');
if(!empId||!empId.trim()){alert('Pilih karyawan dulu');return}
document.getElementById('enrollBtn').disabled=true;
document.getElementById('eprog').innerHTML='<span style="color:var(--cyan)">Memulai...</span>';
addLog('elog2','cy','Mulai daftar: '+empNameClean+' ('+empId+')');
api('/api/enroll','POST',{name:empNameClean,employeeId:empId}).then(d=>{
if(!d.ok){document.getElementById('enrollBtn').disabled=false;
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">'+d.error+'</span>';
addLog('elog2','er','Error: '+d.error)}
}).catch(e=>{document.getElementById('enrollBtn').disabled=false;
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">Gagal</span>';
addLog('elog2','er','Network error')})}

function loadBranchList(){
var sel=document.getElementById('ebranch');
sel.innerHTML='<option value="">-- Loading cabang... --</option>';
api('/api/branches').then(d=>{
if(d.error){sel.innerHTML='<option value="">-- Error: '+d.error+' --</option>';return;}
var arr=Array.isArray(d)?d:(d.data||[]);
sel.innerHTML='<option value="">-- Pilih Cabang --</option>';
arr.forEach(function(b){
var opt=document.createElement('option');
opt.value=b.kode_cabang;
opt.textContent=b.nama_cabang+' ('+b.kode_cabang+')';
sel.appendChild(opt);
});
sel.innerHTML+='<option value="__all__">Semua Cabang</option>';
}).catch(function(e){
sel.innerHTML='<option value="">-- Gagal: '+e.message+' --</option>';
})}

function loadEmpList(kode){
var sel=document.getElementById('eemp');
if(!kode){sel.innerHTML='<option value="">-- Pilih Cabang dulu --</option>';sel.disabled=true;return;}
sel.innerHTML='<option value="">-- Loading... --</option>';
sel.disabled=false;
var url='/api/employees';
if(kode!=='__all__')url+='?kode_cabang='+encodeURIComponent(kode);
api(url).then(d=>{
var arr=Array.isArray(d)?d:(d.data||[]);
sel.innerHTML='<option value="">-- Pilih Karyawan --</option>';
var cnt=0;
arr.forEach(function(emp){
if(emp.finger_terdaftar)return;
var opt=document.createElement('option');
opt.value=emp.id;
opt.textContent=emp.nama+' ('+emp.id+')';
sel.appendChild(opt);
cnt++;
});
if(!cnt)sel.innerHTML='<option value="">-- Semua sudah terdaftar --</option>';
}).catch(function(){
sel.innerHTML='<option value="">-- Gagal memuat --</option>';
})}

function setScanState(cls,badge,name){
var b=document.getElementById('sbox');b.className='scan-box '+(cls||'');
document.getElementById('sbadge').className='scan-badge badge-'+(cls==='ok'?'ok':cls==='fail'?'fail':cls==='active'?'scan':'idle');
document.getElementById('sbadge').textContent=badge;
document.getElementById('sname').textContent=name;
document.getElementById('sconf').textContent=''}

function loadData(){
api('/api/list').then(d=>{
var keys=Object.keys(d);document.getElementById('dcnt2').textContent=keys.length;
var tb=document.getElementById('dtbody');tb.innerHTML='';
document.getElementById('dempty').style.display=keys.length?'none':'block';
keys.forEach(k=>{var e=d[k];var tr=document.createElement('tr');
tr.innerHTML='<td>'+k+'</td><td>'+e.name+'</td><td>'+(e.employeeId||'-')+'</td><td><button class="del-btn" onclick="delFP('+k+')">&times;</button></td>';
tb.appendChild(tr)})})
document.getElementById('dsearch').value='';filterData()}

function filterData(){
var q=document.getElementById('dsearch').value.toLowerCase();
document.querySelectorAll('#dtbody tr').forEach(r=>{r.style.display=r.textContent.toLowerCase().includes(q)?'':'none'})}

function delFP(id){if(!confirm('Hapus ID '+id+'?'))return;
api('/api/delete','POST',{id:id}).then(d=>{if(d.ok)loadData()})}

function emptyAll(){if(!confirm('Hapus SEMUA data?'))return;
api('/api/empty','POST').then(d=>{if(d.ok)loadData()})}

// WiFi functions
function loadWifiStatus(){
api('/api/wifi').then(d=>{
document.getElementById('wmode').textContent=d.mode;
document.getElementById('wmode').style.color=d.mode==='STA'?'var(--green)':'var(--yellow)';
document.getElementById('wsta').textContent=d.connected?d.staSSID+' ('+d.staIP+')':'Tidak terhubung';
document.getElementById('wresetBtn').style.display=d.savedCount>0?'block':'none';
var h='';
if(d.saved&&d.saved.length){d.saved.forEach(function(n,i){
h+='<div style="display:flex;align-items:center;justify-content:space-between;padding:8px 10px;background:var(--bg);border:1px solid var(--border);border-radius:8px;margin-bottom:4px">';
h+='<span style="font-weight:600;font-size:14px">'+n.ssid+'</span>';
if(d.connected&&d.staSSID===n.ssid)h+='<span style="color:var(--green);font-size:11px">TERSAMBUNG</span>';
h+='<button class="del-btn" onclick="deleteWifi(\''+n.ssid.replace(/'/g,"\\'")+'\')">&times;</button></div>';
});}else{h='<div style="text-align:center;padding:12px;color:var(--dim)">Belum ada jaringan tersimpan</div>';}
document.getElementById('wsaved').innerHTML=h;
}).catch(()=>{})}

function scanWifi(){
document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--dim)">Scanning...</div>';
api('/api/wifi/scan').then(networks=>{
var html='';
networks.forEach(n=>{
var signal=n.rssi>-50?'Excellent':n.rssi>-70?'Good':'Weak';
html+='<div class="wifi-item" onclick="selectWifi(\''+n.ssid.replace(/'/g,"\\'")+'\')">';
html+='<div><div class="wifi-ssid">'+n.ssid+'</div>';
html+='<div class="wifi-signal">'+signal+' ('+n.rssi+' dBm) '+(n.enc?'Secured':'Open')+'</div></div>';
html+='<div>'+(n.enc?'<span class="wifi-lock">&#x1f512;</span>':'')+'</div>';
html+='</div>';
});
document.getElementById('wlist').innerHTML=html||'<div style="text-align:center;padding:12px;color:var(--dim)">Tidak ada jaringan</div>';
}).catch(()=>{document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--red)">Gagal scan</div>'})}

function selectWifi(ssid){
document.getElementById('wssid').value=ssid;
document.querySelectorAll('.wifi-item').forEach(el=>el.classList.remove('selected'));
event.currentTarget.classList.add('selected');
}

function saveWifi(){
var ssid=document.getElementById('wssid').value.trim();
var pass=document.getElementById('wpass').value;
if(!ssid){alert('Pilih jaringan WiFi');return}
if(!confirm('Simpan WiFi "'+ssid+'" dan reboot?'))return;
api('/api/wifi','POST',{ssid:ssid,pass:pass}).then(d=>{
if(d.ok){alert('Tersimpan! Device akan reboot...');}
}).catch(()=>alert('Gagal menyimpan'))}

function deleteWifi(ssid){
if(!confirm('Hapus "'+ssid+'" dari daftar?'))return;
api('/api/wifi/delete','POST',{ssid:ssid}).then(d=>{
if(d.ok){loadWifiStatus();}
}).catch(()=>alert('Gagal menghapus'))}

function resetWifi(){
if(!confirm('Hapus WiFi credentials dan reboot ke AP mode?'))return;
api('/api/wifi/reset','POST').then(d=>{
if(d.ok){alert('Dihapus! Device akan reboot ke AP mode...');}
}).catch(()=>alert('Gagal'))}

function loadSettings(){
api('/api/settings').then(d=>{
document.getElementById('sapi').value=d.apiBaseUrl||'';
document.getElementById('scab').value=d.kode_cabang||'';
document.getElementById('sdev').value=d.device_id||'';
}).catch(()=>{})
updStatus()}

function saveSettings(){
var u=document.getElementById('sapi').value.trim();
var c=document.getElementById('scab').value.trim();
var dv=document.getElementById('sdev').value.trim();
if(!u){alert('API URL wajib diisi');return}
api('/api/settings','POST',{apiBaseUrl:u,kode_cabang:c,device_id:dv}).then(d=>{
if(d.ok){alert('Setelan tersimpan!');}
}).catch(()=>alert('Gagal menyimpan'))}

// ── Credentials ──
function loadCred(){
api('/api/credentials').then(d=>{
document.getElementById('akuser').value=d.webUser||'';
document.getElementById('akntp').value=d.ntpServer||'id.pool.ntp.org';
document.getElementById('akoff').value=d.utcOffset||25200;
document.getElementById('aktime').textContent=d.ntpTime||'--:--:--';
document.getElementById('aksync').textContent=d.ntpSynced?'(tersync)':'(belum sync)';
document.getElementById('aksync').style.color=d.ntpSynced?'var(--green)':'var(--dim)';
}).catch(()=>{})}

function saveCred(){
var body={};
var u=document.getElementById('akuser').value.trim();
var p=document.getElementById('akpass').value;
var ap=document.getElementById('akapass').value;
var ntp=document.getElementById('akntp').value.trim();
var off=parseInt(document.getElementById('akoff').value)||25200;
if(u)body.webUser=u;
if(p)body.webPass=p;
if(ap)body.apPass=ap;
if(ntp)body.ntpServer=ntp;
body.utcOffset=off;
if(!Object.keys(body).length){alert('Tidak ada perubahan');return}
if(!confirm('Simpan & reboot?'))return;
api('/api/credentials','POST',body).then(d=>{
if(d.ok){alert('Tersimpan! Rebooting...');}
}).catch(()=>alert('Gagal menyimpan'))}

// ── Backup / Restore ──

var syncRows=[];
function loadSyncCabang(){
api('/api/branches').then(function(d){
var sel=document.getElementById('syncCabang');
if(!sel)return;
var cur=sel.value||'';
var opts='<option value="">— Semua cabang —</option>';
var arr=Array.isArray(d)?d:(d.data||[]);
arr.forEach(function(b){
var k=b.kode_cabang||'';
var n=b.nama_cabang||b.nama||k;
if(!k)return;
opts+='<option value="'+k+'">'+n+' ('+k+')</option>';
});
sel.innerHTML=opts;
if(cur)sel.value=cur;
}).catch(function(){})}
function syncSelectedIds(){
return Array.from(document.querySelectorAll('#syncList input.sync-cb:checked')).map(function(el){return el.value}).filter(Boolean)
}
function syncUpdateBtn(){
var btn=document.getElementById('syncBtn');
if(!btn)return;
var n=syncSelectedIds().length;
btn.disabled=!n;
btn.textContent=n?('Sinkron yang Dipilih ('+n+')'):'Sinkron yang Dipilih';
}
function syncSelectAll(on){
document.querySelectorAll('#syncList input.sync-cb').forEach(function(el){el.checked=!!on});
syncUpdateBtn();
}
function renderSyncList(rows){
syncRows=rows||[];
var box=document.getElementById('syncList');
if(!box)return;
if(!syncRows.length){
box.innerHTML='<div class="empty-state" style="padding:20px">Tidak ada template hex di server (filter/cabang).</div>';
syncUpdateBtn();
return}
box.innerHTML=syncRows.map(function(r){
var id=r.employee_id||'';
var nm=r.nama||'(tanpa nama)';
var cab=r.kode_cabang||'-';
return '<label class="sync-row"><input class="sync-cb" type="checkbox" value="'+id+'" onchange="syncUpdateBtn()"><span class="meta"><div class="nm">'+nm+'</div><div class="id">'+id+'</div><div class="cab">Cabang: '+cab+'</div></span></label>';
}).join('');
syncUpdateBtn();
}
function loadServerTemplates(){
var cab=(document.getElementById('syncCabang').value||'').trim();
var q=(document.getElementById('syncQ').value||'').trim();
var path='/api/server/templates?';
if(cab)path+='kode_cabang='+encodeURIComponent(cab)+'&';
if(q)path+='q='+encodeURIComponent(q)+'&';
document.getElementById('syncProg').innerHTML='<span style="color:var(--cyan)">Memuat daftar...</span>';
document.getElementById('syncList').innerHTML='';
fetch(path).then(function(r){return r.json()}).then(function(d){
if(!d.success&&d.ok===false){
document.getElementById('syncProg').innerHTML='<span style="color:var(--red)">Gagal: '+(d.error||d.message||'load')+'</span>';
return}
var rows=d.data||[];
document.getElementById('syncProg').innerHTML='<span style="color:var(--dim)">'+rows.length+' template siap sync</span>';
renderSyncList(rows);
addLog('rlog','cy','Muat daftar server: '+rows.length);
}).catch(function(){
document.getElementById('syncProg').innerHTML='<span style="color:var(--red)">Gagal muat daftar</span>';
addLog('rlog','er','Gagal muat daftar server');
})}
function syncFromServer(){
var ids=syncSelectedIds();
if(!ids.length){alert('Pilih minimal 1 anak');return}
if(ids.length>30){alert('Maksimal 30 per batch');return}
if(!confirm('Sinkron '+ids.length+' template ke sensor?'))return;
var btn=document.getElementById('syncBtn');
btn.disabled=true;
document.getElementById('syncProg').innerHTML='<span style="color:var(--cyan);font-weight:600">Menyinkronkan '+ids.length+'...</span>';
addLog('rlog','cy','Sync selected: '+ids.length);
api('/api/sync/from-server','POST',{employeeIds:ids}).then(function(d){
syncUpdateBtn();
if(!d.ok){
document.getElementById('syncProg').innerHTML='<span style="color:var(--red)">Gagal: '+(d.error||'unknown')+'</span>';
addLog('rlog','er','Sync gagal: '+(d.error||''));
return}
var msg='OK: '+d.restored+' ditulis, '+d.skipped+' skip, '+d.failed+' gagal'+(d.noHex?(', '+d.noHex+' tanpa hex'):'');
document.getElementById('syncProg').innerHTML='<span style="color:var(--green);font-weight:700">'+msg+'</span>';
addLog('rlog','ok',msg);
updStatus();loadData();
}).catch(function(){
syncUpdateBtn();
document.getElementById('syncProg').innerHTML='<span style="color:var(--red)">Error jaringan</span>';
addLog('rlog','er','Sync network error');
})}

function downloadBackup(){
window.location.href='/api/backup';
addLog('rlog','cy','Mengunduh metadata...')}
function downloadBackupFull(){
window.location.href='/api/backup/full';
addLog('rlog','cy','Mengunduh metadata + template...')}

var restoreData=null;
function handleRestoreFile(input){
if(!input.files||!input.files[0])return;
var reader=new FileReader();
reader.onload=function(e){
try{restoreData=JSON.parse(e.target.result);
document.getElementById('restoreBtn').disabled=false;
var fps=restoreData.fingerprints||restoreData;
var cnt=Object.keys(fps).length;
var tpl=restoreData.templates?Object.keys(restoreData.templates).length:0;
var info='Data: '+cnt+' entries';
if(tpl>0)info+=', Template: '+tpl;
document.getElementById('rprog').innerHTML='Siap: '+info;
addLog('rlog','ok','File terbaca: '+info)}
catch(x){restoreData=null;document.getElementById('restoreBtn').disabled=true;
document.getElementById('rprog').innerHTML='<span style="color:var(--red)">File tidak valid</span>';
addLog('rlog','er','Gagal parse JSON')}};
reader.readAsText(input.files[0])}

function startRestore(){
if(!restoreData)return;
document.getElementById('restoreBtn').disabled=true;
document.getElementById('rresult').innerHTML='';
addLog('rlog','cy','Mulai pulihkan metadata...');
var fps=restoreData.fingerprints||restoreData;
var templates=restoreData.templates||{};
// First restore metadata
api('/api/restore','POST',{fingerprints:fps}).then(function(meta){
if(meta.ok){
addLog('rlog','ok','Metadata: '+meta.restored+' dipulihkan, '+meta.missing+' tanpa template');
var tplKeys=Object.keys(templates).filter(function(k){return templates[k]&&templates[k].length>0});
if(tplKeys.length==0){
document.getElementById('rprog').innerHTML='<span style="color:var(--green)">Selesai (tanpa template)</span>';
document.getElementById('restoreBtn').disabled=false;updStatus();return}
addLog('rlog','cy','Mulai pulihkan '+tplKeys.length+' template...');
var restored=0,failed=0,skipped=0;
var idx=0;
function nextTemplate(){
if(idx>=tplKeys.length){
var html='<div style="background:var(--green);color:#000;padding:12px;border-radius:8px;text-align:center;font-weight:700">';
html+='Metadata OK | Template: '+restored+' restored, '+skipped+' skipped, '+failed+' failed</div>';
document.getElementById('rresult').innerHTML=html;
document.getElementById('restoreBtn').disabled=false;updStatus();return}
var id=parseInt(tplKeys[idx]);
document.getElementById('rprog').innerHTML='Template ID:'+id+' ('+(idx+1)+'/'+tplKeys.length+')...';
api('/api/restore/template','POST',{id:id,data:templates[tplKeys[idx]]}).then(function(r){
if(r.ok&&r.status=='restored')restored++;
else if(r.ok&&r.status=='skipped')skipped++;
else failed++;
idx++;
nextTemplate();
}).catch(function(){failed++;idx++;nextTemplate()})}
nextTemplate()
}else{
document.getElementById('rprog').innerHTML='<span style="color:var(--red)">Gagal: '+(meta.error||'unknown')+'</span>';
document.getElementById('restoreBtn').disabled=false;
addLog('rlog','er','Restore metadata gagal')}
}).catch(function(e){
document.getElementById('rprog').innerHTML='<span style="color:var(--red)">Error</span>';
document.getElementById('restoreBtn').disabled=false;
addLog('rlog','er','Network error')})}

var es=new EventSource('/api/events');
es.onmessage=function(e){
try{var o=JSON.parse(e.data);handleEvent(o)}catch(x){}};

function handleEvent(o){
var t=o.event||o.type;
if(t==='enroll_start'){
document.getElementById('enrollBtn').disabled=true;
document.getElementById('eprog').innerHTML='<span style="color:var(--cyan)">ID: '+o.id+' | Letakkan jari...</span>';
addLog('elog2','cy','Enroll ID:'+o.id+' dimulai')}
else if(t==='waiting_finger')
document.getElementById('eprog').innerHTML='<span style="color:var(--yellow)">Letakkan jari di sensor...</span>';
else if(t==='image_ok_step1')
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Scan 1 OK</span>';
else if(t==='remove')
document.getElementById('eprog').innerHTML='Angkat jari...';
else if(t==='waiting_finger_2')
document.getElementById('eprog').innerHTML='<span style="color:var(--yellow)">Letakkan jari SAMA lagi...</span>';
else if(t==='image_ok_step2')
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Scan 2 OK | Membuat model...</span>';
else if(t==='register_server'){
var rs=(o.response&&o.response.status)||'error';
addLog('elog2',rs==='ok'?'ok':'er','Server DB: '+(rs==='ok'||rs==='updated'?'tersimpan':'gagal sync'))}
else if(t==='enrolled'){
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Berhasil! ID: '+o.id+'</span>';
document.getElementById('enrollBtn').disabled=false;
addLog('elog2','ok','Enrolled: '+o.name+' (ID:'+o.id+')');
var eb=document.getElementById('ebranch').value;
if(eb)loadEmpList(eb);
setTimeout(function(){document.getElementById('eprog').innerHTML='Menunggu...';},3000);
updStatus();loadData()}
else if(t==='enroll_fail'||t==='already_registered'){
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">Gagal: '+(o.id?'sudah ada ID:'+o.id:'')+'</span>';
document.getElementById('enrollBtn').disabled=false;
addLog('elog2','er','Enroll gagal')}
else if(t==='bad_image')
addLog('elog2','er','Gambar jelek step '+(o.step||'?'));
else if(t==='retry_create')
addLog('elog2','er','Create gagal, percobaan '+(o.attempt||'?'));
else if(t==='match'){
setScanState('ok','TERDETEKSI',o.name||'ID: '+o.id);
document.getElementById('sconf').textContent='Confidence: '+Math.round(o.confidence*100/256)+'%';
addLog('slog','ok','MATCH: '+(o.name||'?')+' ID:'+o.id)}
else if(t==='nomatch'){
setScanState('fail','TIDAK DIKENALI','Sidik jari tidak terdaftar');
addLog('slog','er','No match (code:'+o.code+')')}
else if(t==='autoscan_err')
addLog('slog','er','Scan error: '+(o.step||'')+' code:'+(o.code||''));
else if(t==='watchdog_reset'){
addLog('elog','er','WATCHDOG: Device restart');
updStatus()}
else if(t==='sync_progress'){
var st=o.status||'';
document.getElementById('syncProg').innerHTML='Sync '+(o.employeeId||'')+': '+st;
addLog('rlog',st==='restored'?'ok':'cy','Sync '+(o.employeeId||'')+' → '+st)}
else if(t==='sync_complete'){
addLog('rlog','ok','Sync selesai: '+(o.restored||0)+' ok, '+(o.skipped||0)+' skip, '+(o.failed||0)+' gagal');
updStatus()}
else if(t==='restore_progress')
document.getElementById('rprog').innerHTML='Memeriksa ID:'+o.id+' '+(o.template?'<span style="color:var(--green)">ada template</span>':'<span style="color:var(--red)">tanpa template</span>');
else if(t==='restore_complete'){
document.getElementById('rprog').innerHTML='<span style="color:var(--green)">Selesai</span>';
addLog('rlog','ok','Restore selesai: '+o.restored+' ok, '+o.missing+' tanpa template')}
else if(t==='attendance'){
var st=o.response||{};
var msg=st.status||'unknown';
var cols={checkin:'ok',checkout:'cy',not_found:'er',ignored:'t',error:'er'};
var labels={checkin:'ABSEN MASUK',checkout:'ABSEN PULANG',not_found:'TIDAK DIKENALI',ignored:'SUDAH ABSEN',error:'ERROR'};
setScanState(msg==='checkin'||msg==='checkout'?'ok':'fail',labels[msg]||msg.toUpperCase(),'');
addLog('slog',cols[msg]||'t','Attendance: '+msg+(o.response?' '+JSON.stringify(o.response):''))}
updStatus()}

updStatus();setInterval(updStatus,5000);
document.getElementById('ebranch').addEventListener('change',function(){loadEmpList(this.value);});

</script>
<div class="foot">PJTKI Finger · ESP32 + FPM10A</div>
</div>
</body>
</html>
)rawliteral";

#endif
