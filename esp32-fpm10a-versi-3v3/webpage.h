#ifndef WEBPAGE_H
#define WEBPAGE_H

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PJTKI Finger</title>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/select2/4.1.0-rc.0/css/select2.min.css" onerror="window.__cdnCssFail=1">
<style>
/* Offline-safe: no Google Fonts CDN (AP-only tanpa internet) */
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
font-family:ui-sans-serif,system-ui,-apple-system,'Segoe UI',sans-serif;
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
.log{max-height:200px;overflow-y:auto;font-family:ui-monospace,'Cascadia Code','SF Mono',Menlo,Consolas,monospace;font-size:11px;
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
.sync-row.on-device{opacity:.72;border-color:rgba(34,197,94,.35)}
.sync-row input{width:auto;margin:2px 0 0;padding:0;accent-color:var(--cyan)}
.sync-row .meta{flex:1;min-width:0}
.sync-row .nm{font-weight:700;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sync-row .id{font-size:11px;color:var(--dim);font-family:ui-monospace,Menlo,Consolas,monospace;margin-top:2px}
.sync-row .cab{font-size:10px;color:var(--cyan);font-weight:600;margin-top:3px}
.sync-row .st{font-size:10px;font-weight:700;margin-top:3px}
.sync-row .st.ok{color:var(--green)}
.sync-row .st.wait{color:var(--yellow)}
.sync-tools{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}
.sync-tools .btn{width:auto;flex:1;min-width:120px;margin-top:0}
.muted{font-size:13px;color:var(--dim);margin-bottom:8px;line-height:1.45}
.wifi-item{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;background:rgba(11,18,32,.55);
border:1.5px solid var(--border);border-radius:10px;margin-bottom:6px;cursor:pointer;transition:.15s}
.wifi-item:hover,.wifi-item.selected{border-color:var(--cyan);background:rgba(56,189,248,.08)}
.wifi-ssid{font-weight:700;font-size:13px}
.wifi-signal{font-size:11px;color:var(--dim2)}
.wifi-lock{color:var(--yellow);font-size:12px}
.emp-pick{max-height:240px;overflow:auto;border:1.5px solid var(--border);border-radius:10px;
background:rgba(11,18,32,.55);margin-top:6px}
.emp-pick:empty{display:none}
.emp-item{padding:10px 12px;border-bottom:1px solid var(--border);cursor:pointer;font-size:13px}
.emp-item:last-child{border-bottom:0}
.emp-item:hover,.emp-item.on{background:rgba(56,189,248,.12)}
.emp-item .eid{color:var(--dim);font-size:11px;margin-top:2px;font-family:ui-monospace,Menlo,Consolas,monospace}
.muted{font-size:12px;color:var(--dim);line-height:1.5;margin-bottom:10px}
.foot{text-align:center;padding:16px;color:var(--dim2);font-size:11px}
.select2-container .select2-selection--single{height:44px;padding:5px 8px;background:rgba(11,18,32,.7);
border:1.5px solid var(--border);border-radius:10px;margin-top:6px}
.select2-container .select2-selection--single:focus-within{border-color:var(--cyan);box-shadow:0 0 0 3px rgba(56,189,248,.15)}
.select2-container--default .select2-selection--single .select2-selection__rendered{color:var(--text);
font-size:13px;line-height:32px;padding-left:8px}
.select2-container--default .select2-selection--single .select2-selection__placeholder{color:var(--dim2)}
.select2-container--default .select2-selection--single .select2-selection__arrow{height:42px}
.select2-container--default .select2-selection--single .select2-selection__arrow b{border-color:var(--dim2) transparent transparent}
.select2-container--default.select2-container--disabled .select2-selection--single{opacity:.45;cursor:not-allowed}
.select2-container .select2-selection--single .select2-selection__clear{color:var(--dim)}
.select2-dropdown{background:var(--card);border:1px solid var(--border2);border-radius:10px;box-shadow:0 14px 36px rgba(0,0,0,.4)}
.select2-container--default .select2-search--dropdown .select2-search__field{background:rgba(11,18,32,.7);
border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:13px}
.select2-container--default .select2-search--dropdown .select2-search__field:focus{border-color:var(--cyan);outline:none}
.select2-container--default .select2-results__option{font-size:13px;color:var(--text);padding:9px 12px}
.select2-container--default .select2-results__option--highlighted[aria-selected]{background:linear-gradient(135deg,#38bdf8,#0e7490);color:#0f172a}
.select2-container--default .select2-results__option[aria-selected=true]{background:rgba(56,189,248,.16);color:var(--cyan)}
.select2-container--default .select2-results>.select2-results__options::-webkit-scrollbar{width:8px}
.select2-container--default .select2-results>.select2-results__options::-webkit-scrollbar-thumb{background:var(--border2);border-radius:4px}
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
    <p class="muted" id="ecache">Karyawan dicari langsung dari server (Select2 async). Cabang dari cache lokal device.</p>
    <div class="sync-tools" style="margin-top:8px;margin-bottom:4px">
      <button class="btn btn-o" id="eUpdateBtn" onclick="updateListData()">Update Cabang</button>
    </div>
    <label>Cabang</label>
    <select id="ebranch" onchange="loadEmpList(this.value)"><option value="">-- Pilih Cabang --</option></select>
    <label>Karyawan</label>
    <select id="eemp" disabled><option></option></select>
    <input id="eempQ" type="search" placeholder="Cari nama / ID (mode offline)" style="display:none;margin-top:8px" oninput="empNativeSearch()">
    <div class="muted" id="eempMeta" style="margin:6px 0 0">Pilih cabang dulu</div>
    <button class="btn btn-c" id="enrollBtn" onclick="startEnroll()" style="margin-top:10px" disabled>Mulai Daftar</button>
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
    <div class="muted" id="wserver" style="margin-top:10px">Web server: -</div>
  </div>
  <div class="card"><h3>Jaringan Tersimpan</h3>
    <div id="wsaved"></div>
    <button class="btn btn-r" id="wresetBtn" onclick="resetWifi()" style="display:none;margin-top:8px">Hapus Semua WiFi</button>
  </div>
  <div class="card"><h3>Scan Network</h3>
    <button class="btn btn-o" onclick="scanWifi()">Scan</button>
    <div id="wlist" style="margin-top:8px"></div>
  </div>
  <div class="card"><h3>Tambah Jaringan Baru</h3>
    <label>SSID</label><input id="wssid" placeholder="Pilih dari scan di atas" readonly>
    <label>Password</label><input id="wpass" type="password" placeholder="Password">
    <button class="btn btn-c" onclick="saveWifi()">Simpan & Hubungkan</button>
  </div>
</div>

<div class="page" id="p-setel">
  <div class="card"><h3>Pengaturan API</h3>
    <label>API Server URL (device)</label>
    <input id="sapi" placeholder="http://cks.slice-code.com">
    <p class="muted" style="margin-top:6px">Pakai <b>http://</b> untuk ESP32. Jika diisi https://, otomatis disimpan sebagai http:// (TLS sering gagal / GAGAL KIRIM).</p>
    <label>Kode Cabang</label><input id="scab" placeholder="CKS">
    <label>Device ID</label><input id="sdev" placeholder="arduino-001">
    <label>API Key (X-Device-Key)</label><input id="skey" type="password" placeholder="Isi key dari server PJTKI">
    <label style="display:flex;align-items:center;gap:8px;margin-top:12px"><input type="checkbox" id="siren" style="width:auto;margin:0"> Aktifkan gate sensor sentuh (T-OUT)</label>
    <label style="display:flex;align-items:center;gap:8px;margin-top:12px"><input type="checkbox" id="ssched" style="width:auto;margin:0"> Jadwal scan otomatis</label>
    <div style="display:flex;gap:10px;margin-top:8px">
      <div style="flex:1"><label>Mulai (jam)</label><input id="sstart" type="number" min="0" max="23" placeholder="5"></div>
      <div style="flex:1"><label>Selesai (jam)</label><input id="sendh" type="number" min="0" max="23" placeholder="0 (= tengah malam)"></div>
    </div>
    <div class="muted" style="margin-top:6px">Default: aktif 05:00–00:00 (tidur 00–05). Selesai &lt; mulai = lewat tengah malam.</div>
    <button class="btn btn-c" onclick="saveSettings()">Simpan Setelan</button>
  </div>
  <div class="card"><h3>Status</h3>
    <div style="font-size:13px;color:var(--dim)">
      <div>WiFi Mode: <span id="sfg-wmode" style="color:var(--text)">-</span></div>
      <div>IP: <span id="sfg-ip" style="color:var(--text)">-</span></div>
      <div>Fingerprint: <span id="sfg-fp" style="color:var(--text)">-</span></div>
      <div>Gate Sentuh: <span id="sfg-ir" style="color:var(--text)">-</span></div>
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
    <p class="muted">Daftar server dicocokkan dengan yang sudah terdaftar di device. Yang sudah ada di-skip (tidak ditulis ulang). Pilih yang belum ada, lalu sinkron. Setelan API URL + WiFi STA wajib.</p>
    <label>Filter cabang (kosong = semua)</label>
    <select id="syncCabang"><option value="">— Semua cabang —</option></select>
    <label>Cari nama / ID biodata</label>
    <input id="syncQ" type="text" placeholder="Contoh: Siti atau CKSLBK-TW-0001" onkeydown="if(event.key==='Enter')loadServerTemplates()">
    <div class="sync-tools">
      <button class="btn btn-o" onclick="loadServerTemplates()">Muat Daftar</button>
      <button class="btn btn-o" onclick="syncSelectPending()">Pilih yang belum ada</button>
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

<script src="https://code.jquery.com/jquery-3.7.1.min.js" onload="window.__jqOk=1" onerror="window.__cdnJsFail=1"></script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/select2/4.1.0-rc.0/js/select2.min.js" onload="window.__s2Ok=1" onerror="window.__cdnJsFail=1"></script>
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
return fetch(path,{
method:method||'GET',
credentials:'same-origin',
headers:{'Content-Type':'application/json'},
body:body?JSON.stringify(body):undefined
}).then(function(r){
if(r.status===401){
return Promise.reject(new Error('401 unauthorized — login/refresh browser'));
}
return r.text().then(function(t){
var d;
try{d=JSON.parse(t);}
catch(e){
return {ok:false,error:'bad_json',bytes:(t&&t.length)||0,http:r.status};
}
if(!r.ok&&d&&typeof d==='object'&&d.error===undefined)d.error='http_'+r.status;
if(!r.ok&&d&&typeof d==='object')d.ok=false;
return d;
})})}
function hasSelect2(){return !!(window.jQuery&&jQuery.fn&&jQuery.fn.select2&&window.__cdnJsFail!==1)}
function s2Init(id){
if(!hasSelect2())return;
var el=document.getElementById(id);
if(!el)return;
var ph=el.options&&el.options[0]?el.options[0].textContent:'';
if(jQuery(el).data('select2'))jQuery(el).select2('destroy');
jQuery(el).off('change.s2proxy');
jQuery(el).select2({placeholder:ph,allowClear:false,width:'100%',disabled:!!el.disabled});
if(id==='ebranch'){
jQuery(el).on('change.s2proxy',function(){loadEmpList(el.value)});
}
}
// Select2 async karyawan — cari per ketikan, server kirim max ~30 hasil (Malang aman).
// Fallback native bila CDN jQuery/Select2 gagal (AP-only tanpa internet).
var empLocalOnDev={};
var empS2Kode='';
var empS2Ready=false;
var empNativeTimer=null;
function setEmpSelectState(msg, enabled){
empS2Ready=!!enabled;
empS2Kode='';
var meta=document.getElementById('eempMeta');
if(meta)meta.textContent=msg||'';
var btn=document.getElementById('enrollBtn');
if(btn&&!enabled&&btn.dataset.enrolling!=='1')btn.disabled=true;
var q=document.getElementById('eempQ');
if(q){q.style.display='none';q.value='';}
s2InitEmp('');
}
function empNativeFill(kode,q,attempt){
attempt=attempt||0;
var sel=document.getElementById('eemp');
var meta=document.getElementById('eempMeta');
var url='/api/employees?kode_cabang='+encodeURIComponent(kode)+'&select2=1&limit=40&page=1&q='+encodeURIComponent(q||'');
api(url).then(function(d){
if(empS2Kode!==kode)return;
if(d&&d.retry&&(d.error==='https_busy'||d.error==='wifi_scanning')&&attempt<20){
if(meta)meta.textContent='Server sibuk... ('+(attempt+1)+')';
setTimeout(function(){empNativeFill(kode,q,attempt+1)},800);
return}
sel.innerHTML='<option value="">-- Pilih karyawan --</option>';
var rows=(d&&d.results)||[];
rows.forEach(function(r){
var id=String(r.id||'');
if(empLocalOnDev[id])return;
var opt=document.createElement('option');
opt.value=id;
opt.textContent=r.text||r.id||'';
sel.appendChild(opt);
});
sel.disabled=false;
empS2Ready=true;
if(meta)meta.textContent=(d&&d.source==='live'?'Server live':'Cache')+' · ketik cari · '+rows.length+' hasil · '+kode;
var btn=document.getElementById('enrollBtn');
if(btn&&btn.dataset.enrolling!=='1')btn.disabled=!sel.value;
}).catch(function(){
if(meta)meta.textContent='Gagal muat karyawan';
});
}
function empNativeSearch(){
if(!empS2Kode||hasSelect2())return;
clearTimeout(empNativeTimer);
var q=(document.getElementById('eempQ')||{}).value||'';
empNativeTimer=setTimeout(function(){empNativeFill(empS2Kode,q,0)},280);
}
function empNativeInit(kode){
var sel=document.getElementById('eemp');
var qel=document.getElementById('eempQ');
if(!sel)return;
sel.onchange=function(){
var btn=document.getElementById('enrollBtn');
if(btn&&btn.dataset.enrolling!=='1')btn.disabled=!this.value;
};
if(!kode){
sel.innerHTML='<option value="">-- Pilih cabang dulu --</option>';
sel.disabled=true;
if(qel){qel.style.display='none';qel.value='';}
empS2Ready=false;
return}
empS2Kode=kode;
empS2Ready=true;
sel.disabled=false;
sel.innerHTML='<option value="">-- Memuat... --</option>';
if(qel){qel.style.display='block';qel.value='';qel.placeholder='Cari nama / ID di '+kode;}
empNativeFill(kode,'',0);
}
function s2InitEmp(kode){
if(!hasSelect2()){empNativeInit(kode);return;}
var qel=document.getElementById('eempQ');
if(qel){qel.style.display='none';qel.value='';}
var $el=jQuery('#eemp');
if(!$el.length)return;
if($el.data('select2'))$el.select2('destroy');
$el.off('change.enroll');
$el.empty().append('<option></option>');
empS2Kode=kode||'';
if(!kode){
$el.prop('disabled',true);
$el.select2({placeholder:'Pilih cabang dulu',allowClear:false,width:'100%',disabled:true});
empS2Ready=false;
return}
$el.prop('disabled',false);
empS2Ready=true;
$el.select2({
placeholder:'Ketik nama atau ID karyawan...',
allowClear:true,
width:'100%',
minimumInputLength:0,
ajax:{
url:'/api/employees',
dataType:'json',
delay:280,
data:function(params){
return{
kode_cabang:kode,
q:params.term||'',
page:params.page||1,
limit:30,
select2:1
};
},
transport:function(params, success, failure){
var qs=jQuery.param(params.data||{});
var url=params.url+(params.url.indexOf('?')>=0?'&':'?')+qs;
var attempt=0;
function once(){
fetch(url,{credentials:'same-origin'}).then(function(r){return r.text()}).then(function(t){
var d;try{d=JSON.parse(t);}catch(e){failure(e);return}
if(d&&d.retry&&(d.error==='https_busy'||d.error==='wifi_scanning')&&attempt<20){
attempt++;
var meta=document.getElementById('eempMeta');
if(meta)meta.textContent='Server sibuk, coba lagi... ('+attempt+')';
setTimeout(once,800);
return}
success(d);
}).catch(failure)}
once();
},
processResults:function(data, params){
params.page=params.page||1;
if(data&&data.error){
var meta=document.getElementById('eempMeta');
if(meta)meta.textContent='Gagal: '+data.error;
return{results:[],pagination:{more:false}}
}
var rows=(data&&data.results)?data.results:[];
var out=[];
for(var i=0;i<rows.length;i++){
var r=rows[i];
var id=String(r.id||'');
if(empLocalOnDev[id])continue;
out.push({id:id,text:r.text||((r.nama||id)+' ('+id+')')});
}
var meta=document.getElementById('eempMeta');
if(meta){
var src=(data&&data.source==='live')?'server live':'cache';
if(data&&data.matched!=null)meta.textContent='Select2 · '+src+' · '+kode+' · '+out.length+(data.matched!=null?' / '+data.matched:'')+' · ketik untuk cari';
else meta.textContent='Select2 · '+src+' · ketik untuk cari di '+kode;
}
return{results:out,pagination:{more:!!(data&&data.more)}};
},
cache:true
}
});
$el.on('change.enroll',function(){
var btn=document.getElementById('enrollBtn');
if(btn&&btn.dataset.enrolling!=='1')btn.disabled=!this.value;
});
}

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
var e=document.getElementById('sfg-ir');if(e)e.textContent=d.irEnabled?(d.irFinger?'JARI':'ON') : 'OFF';
 autoOn=d.autoActive;
}).catch(()=>{})}

function startEnroll(){
var empId='';
var empNameClean='';
if(hasSelect2()){
var $el=jQuery('#eemp');
empId=$el.length?String($el.val()||''):'';
if($el.length){
var d=$el.select2('data');
if(d&&d[0]&&d[0].text)empNameClean=String(d[0].text).replace(/\s*\([^)]*\)\s*$/,'');
}
}else{
var sel=document.getElementById('eemp');
empId=sel?String(sel.value||''):'';
if(sel&&sel.selectedIndex>=0){
empNameClean=String(sel.options[sel.selectedIndex].text||'').replace(/\s*\([^)]*\)\s*$/,'');
}
}
if(!empS2Kode){alert('Pilih cabang dulu');return}
if(!empId||!empId.trim()){alert('Pilih karyawan dulu');return}
if(!empNameClean)empNameClean=empId;
var enrollBtn=document.getElementById('enrollBtn');
enrollBtn.disabled=true;
enrollBtn.dataset.enrolling='1';
document.getElementById('eprog').innerHTML='<span style="color:var(--cyan)">Memulai...</span>';
addLog('elog2','cy','Mulai daftar: '+empNameClean+' ('+empId+')');
api('/api/enroll','POST',{name:empNameClean,employeeId:empId}).then(d=>{
enrollBtn.dataset.enrolling='0';
if(!d.ok){enrollBtn.disabled=false;
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">'+d.error+'</span>';
addLog('elog2','er','Error: '+d.error)}
}).catch(e=>{enrollBtn.dataset.enrolling='0';enrollBtn.disabled=false;
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">Gagal</span>';
addLog('elog2','er','Network error')})}

function fmtAge(sec){
if(sec==null||sec<0)return 'belum ada';
if(sec<60)return sec+' dtk lalu';
if(sec<3600)return Math.floor(sec/60)+' mnt lalu';
if(sec<86400)return Math.floor(sec/3600)+' jam lalu';
return Math.floor(sec/86400)+' hari lalu'}
function updateCacheInfo(kode){
var q=kode?('?kode_cabang='+encodeURIComponent(kode)):'';
api('/api/cache/status'+q).then(function(d){
var el=document.getElementById('ecache');if(!el||!d||!d.ok)return;
var t='Cache lokal';
if(d.branchesCached)t+=' · cabang '+fmtAge(d.branchesAgeSec);
else t+=' · cabang belum di-sync';
if(d.kode_cabang){
if(d.employeesCached)t+=' · karyawan '+d.kode_cabang+' '+fmtAge(d.employeesAgeSec);
else t+=' · karyawan '+d.kode_cabang+' belum di-sync';
}
t+=' · auto '+d.refreshMinutes+' mnt';
el.textContent=t;
}).catch(function(){})}
function loadBranchList(attempt){
attempt=attempt||0;
var sel=document.getElementById('ebranch');
setEmpSelectState('-- Pilih Cabang dulu --', false);
sel.innerHTML='<option value="">-- Loading cabang... --</option>';
sel.disabled=true;
s2Init('ebranch');
api('/api/branches').then(d=>{
if(d&&d.error==='caching'&&d.retry&&attempt<40){
sel.innerHTML='<option value="">-- Mengunduh cabang... ('+(attempt+1)+') --</option>';
setTimeout(function(){loadBranchList(attempt+1)},1000);
return}
sel.disabled=false;
if(d&&(d.ok===false||d.error)){sel.innerHTML='<option value="">-- Error: '+(d.error||'load')+' --</option>';s2Init('ebranch');return;}
var arr=Array.isArray(d)?d:(d.data||[]);
sel.innerHTML='<option value="">-- Pilih Cabang --</option>';
arr.forEach(function(b){
var opt=document.createElement('option');
opt.value=b.kode_cabang;
opt.textContent=(b.nama_cabang||b.nama||b.kode_cabang)+' ('+b.kode_cabang+')';
sel.appendChild(opt);
});
sel.innerHTML+='<option value="__all__">Semua Cabang</option>';
if(!hasSelect2()){
sel.onchange=function(){loadEmpList(sel.value)};
}
s2Init('ebranch');
updateCacheInfo();
}).catch(function(e){
sel.disabled=false;
sel.innerHTML='<option value="">-- Gagal: '+e.message+' --</option>';
s2Init('ebranch');
})}

var empLoadSeq=0;
function loadEmpList(kode){
var seq=++empLoadSeq;
var enrollBtn=document.getElementById('enrollBtn');
if(!kode){
setEmpSelectState('Pilih cabang dulu', false);
updateCacheInfo();
return}
if(enrollBtn&&enrollBtn.dataset.enrolling!=='1')enrollBtn.disabled=true;
var meta=document.getElementById('eempMeta');
if(meta)meta.textContent='Select2 live · ketik nama/ID di '+kode;
empLocalOnDev={};
api('/api/list').then(function(local){
if(seq!==empLoadSeq)return;
empLocalOnDev=localEmpSet(local||{});
}).catch(function(){});
s2InitEmp(kode);
updateCacheInfo(kode==='__all__'?'':kode);
}

function refreshEnrollLists(){return updateListData()}
function updateListData(){
var el=document.getElementById('ecache');
var btn=document.getElementById('eUpdateBtn');
var kode=document.getElementById('ebranch').value||'';
if(btn){btn.disabled=true;btn.textContent='Updating...'}
setEmpSelectState(kode?'Memuat ulang cabang...':'Pilih cabang dulu', false);
if(el)el.innerHTML='<span style="color:var(--cyan)">Update cache cabang...</span>';
addLog('elog2','cy','Update cabang');
return api('/api/cache/refresh','POST',{kode_cabang:kode}).then(function(d){
if(btn){btn.disabled=false;btn.textContent='Update Cabang'}
if(!d||d.ok===false){
if(el)el.innerHTML='<span style="color:var(--red)">Gagal update: '+(d&&d.error?d.error:'unknown')+'</span>';
addLog('elog2','er','Update cabang gagal');
return}
var msg='Cabang '+(d.branches?'OK':'fail')+' · karyawan via Select2 live';
if(el)el.innerHTML='<span style="color:var(--green)">'+msg+'</span>';
addLog('elog2','ok',msg);
var keep=kode;
loadBranchList();
setTimeout(function(){
var sel=document.getElementById('ebranch');
if(keep&&sel){
sel.value=keep;
if(window.jQuery&&jQuery(sel).data('select2'))jQuery(sel).val(keep).trigger('change.select2');
loadEmpList(keep);
}else updateCacheInfo();
},250);
}).catch(function(){
if(btn){btn.disabled=false;btn.textContent='Update Cabang'}
if(el)el.innerHTML='<span style="color:var(--red)">Gagal update (jaringan / WiFi)</span>';
addLog('elog2','er','Update cabang network error');
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
api('/api/delete','POST',{id:id}).then(d=>{
if(d.ok){
loadData();
var k=document.getElementById('ebranch')&&document.getElementById('ebranch').value;
if(k)loadEmpList(k);
addLog('elog','ok','Hapus ID '+id+' — cache karyawan di-update');
}})}

function emptyAll(){if(!confirm('Hapus SEMUA data?'))return;
api('/api/empty','POST').then(d=>{
if(d.ok){
loadData();
var k=document.getElementById('ebranch')&&document.getElementById('ebranch').value;
if(k)loadEmpList(k);
}})}

// WiFi functions
function loadWifiStatus(){
api('/api/wifi').then(d=>{
document.getElementById('wmode').textContent=d.mode;
document.getElementById('wmode').style.color=d.mode==='STA'?'var(--green)':'var(--yellow)';
document.getElementById('wsta').textContent=d.connected?d.staSSID+' ('+d.staIP+')':'Tidak terhubung';
var ws=document.getElementById('wserver');
if(ws){
if(d.mode==='STA'&&d.staIP){
ws.innerHTML='Web server lewat <b>WiFi</b> (bukan AP): <a href="http://'+d.staIP+'/" style="color:var(--cyan)">http://'+d.staIP+'/</a>';
}else{
ws.textContent='Web server lewat Access Point: http://192.168.4.1/';
}
}
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

function renderWifiScanList(networks,meta){
networks=networks||[];
networks.sort(function(a,b){return(b.rssi||-999)-(a.rssi||-999)});
var head='<div class="muted" style="margin-bottom:8px">';
if(meta&&meta.status==='scanning'){
head+='Live · '+networks.length+' AP · ch'+(meta.channel||'?')+' pass '+(meta.pass||1)+'/2 (2.4GHz)';
}else{
head+='Selesai · '+networks.length+' AP (2.4GHz)';
}
head+='</div>';
if(!networks.length){
document.getElementById('wlist').innerHTML=head+'<div style="text-align:center;padding:12px;color:var(--dim)">'+(meta&&meta.status==='scanning'?'Mencari…':'Tidak ada jaringan 2.4GHz')+'</div>';
return}
var html=head;
networks.forEach(function(n){
var ssid=n.ssid||'';
var label=ssid?ssid:(n.hidden?'(hidden)':'?');
var signal=n.rssi>-50?'Excellent':n.rssi>-70?'Good':n.rssi>-85?'Fair':'Weak';
var safe=ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'");
html+='<div class="wifi-item" onclick="selectWifi(\''+safe+'\')">';
html+='<div><div class="wifi-ssid">'+label+(n.hidden&&ssid?' <span style="color:var(--dim);font-size:11px">hidden</span>':'')+'</div>';
html+='<div class="wifi-signal">'+signal+' ('+n.rssi+' dBm) ch'+(n.channel||'?')+' '+(n.enc?'Secured':'Open');
if(n.bssid)html+=' · '+n.bssid;
html+='</div></div>';
html+='<div>'+(n.enc?'<span class="wifi-lock">&#x1f512;</span>':'')+'</div></div>';
});
document.getElementById('wlist').innerHTML=html}

function scanWifi(){
document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--dim)">Scan live 2.4GHz… daftar terisi bertahap</div>';
var tries=0;
function poll(){
api('/api/wifi/scan').then(function(res){
var list=Array.isArray(res)?res:(res&&res.networks)||null;
if(res&&res.status==='scanning'){
tries=0;
renderWifiScanList(list,{status:'scanning',channel:res.channel,pass:res.pass});
setTimeout(poll,700);
return}
if(list){
renderWifiScanList(list,{status:'done'});
return}
document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--red)">Gagal scan</div>';
}).catch(function(){
tries++;
if(tries>40){document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--red)">Gagal scan</div>';return}
document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--dim)">Reconnect AP… ('+tries+') — hasil tetap digabung</div>';
setTimeout(poll,1000);
})}
poll()}

function selectWifi(ssid){
document.getElementById('wssid').value=ssid;
document.getElementById('wssid').readOnly=false;
document.querySelectorAll('.wifi-item').forEach(el=>el.classList.remove('selected'));
if(event&&event.currentTarget)event.currentTarget.classList.add('selected');
}

function saveWifi(){
var ssid=document.getElementById('wssid').value.trim();
var pass=document.getElementById('wpass').value;
if(!ssid){alert('Pilih jaringan WiFi');return}
if(!confirm('Simpan & hubungkan ke WiFi "'+ssid+'"? (tanpa reboot)'))return;
api('/api/wifi','POST',{ssid:ssid,pass:pass}).then(d=>{
if(d.ok){
if(d.msg==='connected')alert('Tersambung ke '+d.staSSID+' ('+d.staIP+')');
else alert('WiFi tersimpan, tapi gagal connect. Device tetap mode AP.');
loadWifiStatus();updStatus();
}else alert('Gagal: '+(d.error||''));
}).catch(()=>alert('Gagal menyimpan'))}

function deleteWifi(ssid){
if(!confirm('Hapus "'+ssid+'" dari daftar?'))return;
api('/api/wifi/delete','POST',{ssid:ssid}).then(d=>{
if(d.ok){loadWifiStatus();}
}).catch(()=>alert('Gagal menghapus'))}

function resetWifi(){
if(!confirm('Hapus semua WiFi tersimpan dan kembali ke mode AP? (tanpa reboot)'))return;
api('/api/wifi/reset','POST').then(d=>{
if(d.ok){alert('WiFi dihapus. Device mode AP (192.168.4.1).');loadWifiStatus();updStatus();}
}).catch(()=>alert('Gagal'))}

function loadSettings(){
api('/api/settings').then(d=>{
document.getElementById('sapi').value=d.apiBaseUrl||'';
document.getElementById('scab').value=d.kode_cabang||'';
document.getElementById('sdev').value=d.device_id||'';
document.getElementById('skey').value=d.api_key||'';
document.getElementById('siren').checked=d.ir_enabled!==false;
document.getElementById('ssched').checked=d.scan_schedule!==false;
document.getElementById('sstart').value=(d.scan_start_hour!=null?d.scan_start_hour:5);
document.getElementById('sendh').value=(d.scan_end_hour!=null?d.scan_end_hour:0);
}).catch(()=>{})
updStatus()}

function saveSettings(){
var u=document.getElementById('sapi').value.trim();
var c=document.getElementById('scab').value.trim();
var dv=document.getElementById('sdev').value.trim();
var ak=document.getElementById('skey').value.trim();
var ir=document.getElementById('siren').checked;
var sch=document.getElementById('ssched').checked;
var sh=parseInt(document.getElementById('sstart').value,10);if(isNaN(sh))sh=5;
var eh=parseInt(document.getElementById('sendh').value,10);if(isNaN(eh))eh=0;
if(sh<0)sh=0;if(sh>23)sh=23;if(eh<0)eh=0;if(eh>23)eh=23;
if(!u){alert('API URL wajib diisi');return}
// Device ESP32: https hampir selalu gagal (RAM/TLS) — normalisasi ke http
if(/^https:\/\//i.test(u)){
u='http://'+u.replace(/^https:\/\//i,'');
document.getElementById('sapi').value=u;
}
api('/api/settings','POST',{apiBaseUrl:u,kode_cabang:c,device_id:dv,api_key:ak,ir_enabled:ir,scan_schedule:sch,scan_start_hour:sh,scan_end_hour:eh}).then(d=>{
if(d.ok){
if(d.apiBaseUrl)document.getElementById('sapi').value=d.apiBaseUrl;
alert(d.https_rewritten?'Tersimpan sebagai HTTP (HTTPS tidak stabil di ESP32):\n'+d.apiBaseUrl:'Setelan tersimpan!');
}
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
function loadSyncCabang(attempt){
attempt=attempt||0;
api('/api/branches').then(function(d){
if(d&&d.error==='caching'&&d.retry&&attempt<40){
setTimeout(function(){loadSyncCabang(attempt+1)},1000);
return}
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
function syncSelectPending(){
document.querySelectorAll('#syncList input.sync-cb').forEach(function(el){
el.checked=el.getAttribute('data-ondev')!=='1';
});
syncUpdateBtn();
}
function localEmpSet(list){
var s={};
Object.keys(list||{}).forEach(function(k){
var e=list[k]&&list[k].employeeId;
if(e)s[String(e)]=true;
});
return s;
}
function renderSyncList(rows, localSet){
syncRows=rows||[];
var box=document.getElementById('syncList');
if(!box)return;
if(!syncRows.length){
box.innerHTML='<div class="empty-state" style="padding:20px">Tidak ada template hex di server (filter/cabang).</div>';
syncUpdateBtn();
return}
var pending=0,onDev=0;
box.innerHTML=syncRows.map(function(r){
var id=r.employee_id||'';
var nm=r.nama||'(tanpa nama)';
var cab=r.kode_cabang||'-';
var already=!!(localSet&&localSet[id]);
if(already)onDev++; else pending++;
return '<label class="sync-row'+(already?' on-device':'')+'"><input class="sync-cb" type="checkbox" value="'+id+'" data-ondev="'+(already?'1':'0')+'" onchange="syncUpdateBtn()"><span class="meta"><div class="nm">'+nm+'</div><div class="id">'+id+'</div><div class="cab">Cabang: '+cab+'</div><div class="st '+(already?'ok':'wait')+'">'+(already?'Sudah di device — skip':'Belum di device — perlu sync')+'</div></span></label>';
}).join('');
syncUpdateBtn();
var sum=document.getElementById('syncProg');
if(sum)sum.innerHTML='<span style="color:var(--dim)">'+pending+' belum ada, '+onDev+' sudah terdaftar di device</span>';
}
function loadServerTemplates(){
var cab=(document.getElementById('syncCabang').value||'').trim();
var q=(document.getElementById('syncQ').value||'').trim();
var path='/api/server/templates?';
if(cab)path+='kode_cabang='+encodeURIComponent(cab)+'&';
if(q)path+='q='+encodeURIComponent(q)+'&';
document.getElementById('syncProg').innerHTML='<span style="color:var(--cyan)">Memuat & mencocokkan daftar...</span>';
document.getElementById('syncList').innerHTML='';
Promise.all([
fetch(path).then(function(r){return r.json()}),
api('/api/list').catch(function(){return {}})
]).then(function(arr){
var d=arr[0], local=arr[1]||{};
if(!d.success&&d.ok===false){
document.getElementById('syncProg').innerHTML='<span style="color:var(--red)">Gagal: '+(d.error||d.message||'load')+'</span>';
return}
var rows=d.data||[];
var set=localEmpSet(local);
renderSyncList(rows, set);
addLog('rlog','cy','Muat daftar server: '+rows.length+' (cocok lokal)');
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
var msg='OK: '+d.restored+' ditulis, '+d.skipped+' sudah ada (skip), '+d.failed+' gagal'+(d.noHex?(', '+d.noHex+' tanpa hex'):'');
document.getElementById('syncProg').innerHTML='<span style="color:var(--green);font-weight:700">'+msg+'</span>';
addLog('rlog','ok',msg);
updStatus();loadData();
setTimeout(loadServerTemplates, 1200);
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
var rs=o.status||(o.response&&o.response.status)||(o.ok?'ok':'error');
var hexOk=o.hex===true||o.hex==='true';
addLog('elog2',rs==='ok'||rs==='updated'?'ok':'er','Server DB: '+(rs==='ok'||rs==='updated'?'tersimpan':'gagal sync')+(hexOk?' + hex':' tanpa hex'))}
else if(t==='enrolled'){
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Berhasil! ID: '+o.id+'</span>';
var ebtn=document.getElementById('enrollBtn');
if(ebtn)ebtn.dataset.enrolling='0';
addLog('elog2','ok','Enrolled: '+o.name+' (ID:'+o.id+')');
var eb=document.getElementById('ebranch').value;
if(eb)loadEmpList(eb);
else if(ebtn)ebtn.disabled=false;
setTimeout(function(){document.getElementById('eprog').innerHTML='Menunggu...';},3000);
updStatus();loadData()}
else if(t==='enroll_fail'||t==='already_registered'){
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">Gagal: '+(o.id?'sudah ada ID:'+o.id:'')+'</span>';
var ebtn=document.getElementById('enrollBtn');
if(ebtn){ebtn.dataset.enrolling='0';ebtn.disabled=false}
addLog('elog2','er','Enroll gagal')}
else if(t==='bad_image')
addLog('elog2','er','Gambar jelek step '+(o.step||'?'));
else if(t==='retry_create')
addLog('elog2','er','Create gagal, percobaan '+(o.attempt||'?'));
else if(t==='match'){
setScanState('ok','TERDETEKSI',o.name||'ID: '+o.id);
document.getElementById('sconf').textContent='Confidence: '+Math.round(o.confidence*100/256)+'%';
addLog('slog','ok','MATCH: '+(o.name||'?')+' ID:'+o.id);
if(window._scanResetT)clearTimeout(window._scanResetT);
window._scanResetT=setTimeout(function(){
if(autoOn)setScanState('active','MENUNGGU','Menempelkan jari...');
},2800)}
else if(t==='nomatch'){
setScanState('fail','TIDAK DIKENALI','Sidik jari tidak terdaftar');
addLog('slog','er','No match (code:'+o.code+')');
if(window._scanResetT)clearTimeout(window._scanResetT);
window._scanResetT=setTimeout(function(){
if(autoOn)setScanState('active','MENUNGGU','Menempelkan jari...');
},2800)}
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
// Device emit: {event,ok,status} — bukan {response:{status}}
var msg=o.status||(o.response&&o.response.status)||'unknown';
var cols={checkin:'ok',checkout:'cy',not_found:'er',ignored:'t',error:'er',offline:'wa',ok:'ok'};
var labels={checkin:'ABSEN MASUK',checkout:'ABSEN PULANG',not_found:'TIDAK DIKENALI',ignored:'SUDAH ABSEN',error:'GAGAL KIRIM',offline:'OFFLINE',ok:'BERHASIL'};
var okUi=(msg==='checkin'||msg==='checkout'||msg==='ignored'||msg==='ok');
setScanState(okUi?'ok':(msg==='offline'?'active':'fail'),labels[msg]||msg.toUpperCase(),'');
addLog('slog',cols[msg]||'t','Attendance: '+msg);
if(window._scanResetT)clearTimeout(window._scanResetT);
window._scanResetT=setTimeout(function(){
if(autoOn)setScanState('active','MENUNGGU','Menempelkan jari...');
},2800)}
updStatus()}

updStatus();setInterval(updStatus,5000);
(function(){
var ebtn=document.getElementById('enrollBtn');
if(ebtn)ebtn.disabled=true;
setEmpSelectState('Pilih cabang dulu', false);
s2Init('ebranch');
})();

</script>
<div class="foot">PJTKI Finger · ESP32 + FPM10A</div>
</div>
</body>
</html>
)rawliteral";

#endif
