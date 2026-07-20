#ifndef WEBPAGE_H
#define WEBPAGE_H

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FPM10A Console</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0a0e14;--card:#141b24;--border:#1e2a38;--cyan:#00e5ff;--green:#00e676;--red:#ff1744;--yellow:#ffd600;--dim:#6b7280;--text:#e2e8f0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);min-height:100vh}
.topbar{background:#0d1520;border-bottom:1px solid var(--border);padding:12px 16px;display:flex;align-items:center;justify-content:space-between;position:sticky;top:0;z-index:100}
.topbar h1{font-size:18px;color:var(--cyan)}
.pill{display:inline-flex;align-items:center;gap:5px;padding:3px 10px;border-radius:12px;font-size:11px;background:var(--card);border:1px solid var(--border)}
.dot{width:7px;height:7px;border-radius:50%}
.dot-g{background:var(--green)}.dot-r{background:var(--red)}.dot-y{background:var(--yellow)}
.tabs{display:flex;background:var(--card);border-bottom:1px solid var(--border);overflow-x:auto}
.tab{flex:1;padding:12px 8px;text-align:center;cursor:pointer;font-size:13px;color:var(--dim);border-bottom:2px solid transparent;transition:.2s;white-space:nowrap}
.tab:hover{color:var(--text)}.tab.on{color:var(--cyan);border-color:var(--cyan)}
.page{display:none;padding:16px;max-width:600px;margin:0 auto}.page.on{display:block}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:12px}
.card h3{font-size:13px;color:var(--dim);margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}
.stat{font-size:28px;font-weight:700}
.stat-c{color:var(--cyan)}.stat-g{color:var(--green)}.stat-r{color:var(--red)}
.btn{display:inline-flex;align-items:center;justify-content:center;padding:10px 20px;border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer;transition:.15s;width:100%;margin-top:8px}
.btn-c{background:var(--cyan);color:#000}.btn-c:hover{filter:brightness(1.15)}
.btn-g{background:var(--green);color:#000}.btn-g:hover{filter:brightness(1.15)}
.btn-r{background:var(--red);color:#fff}.btn-r:hover{filter:brightness(1.15)}
.btn-o{background:transparent;border:1px solid var(--border);color:var(--text)}.btn-o:hover{background:var(--border)}
.btn:disabled{opacity:.4;cursor:not-allowed}
input,select{width:100%;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:14px;margin-top:6px;outline:none}
input:focus,select:focus{border-color:var(--cyan)}
label{font-size:13px;color:var(--dim);margin-top:10px;display:block}
.scan-box{text-align:center;padding:30px 16px;border-radius:16px;border:2px solid var(--border);transition:.3s}
.scan-box.active{border-color:var(--cyan);box-shadow:0 0 30px rgba(0,229,255,.15)}
.scan-box.ok{border-color:var(--green);box-shadow:0 0 30px rgba(0,230,118,.2)}
.scan-box.fail{border-color:var(--red);box-shadow:0 0 30px rgba(255,23,68,.2)}
.scan-icon{font-size:48px;margin-bottom:12px;animation:pulse 1.5s infinite}
@keyframes pulse{0%,100%{opacity:.6}50%{opacity:1}}
.scan-badge{display:inline-block;padding:4px 14px;border-radius:16px;font-size:12px;font-weight:700;margin:8px 0}
.badge-scan{background:rgba(0,229,255,.15);color:var(--cyan)}
.badge-ok{background:rgba(0,230,118,.15);color:var(--green)}
.badge-fail{background:rgba(255,23,68,.15);color:var(--red)}
.badge-idle{background:rgba(107,114,128,.15);color:var(--dim)}
.scan-name{font-size:20px;font-weight:700;margin:4px 0}
.log{max-height:200px;overflow-y:auto;font-family:'SF Mono',monospace;font-size:11px;background:var(--bg);border-radius:8px;padding:8px;margin-top:8px}
.log div{padding:2px 0;border-bottom:1px solid var(--border)}
.log .t{color:var(--dim)}.log .ok{color:var(--green)}.log .er{color:var(--red)}.log .cy{color:var(--cyan)}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;padding:8px;color:var(--dim);border-bottom:1px solid var(--border);font-size:11px;text-transform:uppercase}
td{padding:8px;border-bottom:1px solid var(--border)}
.del-btn{background:none;border:none;color:var(--red);cursor:pointer;font-size:16px;padding:4px 8px}
.empty-state{text-align:center;padding:40px;color:var(--dim)}
.wifi-item{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;margin-bottom:6px;cursor:pointer;transition:.15s}
.wifi-item:hover{border-color:var(--cyan)}
.wifi-item.selected{border-color:var(--cyan);background:rgba(0,229,255,.05)}
.wifi-ssid{font-weight:600;font-size:14px}
.wifi-signal{font-size:12px;color:var(--dim)}
.wifi-lock{color:var(--yellow);font-size:12px}
</style>
</head>
<body>
<div class="topbar">
  <h1>FPM10A</h1>
  <div style="display:flex;gap:6px">
    <span class="pill"><span class="dot" id="sdot"></span><span id="stxt">OFFLINE</span></span>
    <span class="pill" id="tpill">0 templates</span>
  </div>
</div>
<div class="tabs">
  <div class="tab on" onclick="go('dash')">Dashboard</div>
  <div class="tab" onclick="go('enroll')">Daftar</div>
  <div class="tab" onclick="go('scan')">Scan</div>
  <div class="tab" onclick="go('data')">Data</div>
  <div class="tab" onclick="go('wifi')">WiFi</div>
  <div class="tab" onclick="go('setel')">Setelan</div>
</div>

<div class="page on" id="p-dash">
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
    <button class="btn btn-g" onclick="go('scan')">Mulai Scan</button>
  </div>
  <div class="card"><h3>Activity Log</h3><div class="log" id="elog"></div></div>
</div>

<div class="page" id="p-enroll">
  <div class="card"><h3>Daftar Sidik Jari</h3>
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
    <div class="scan-box" id="sbox">
      <div class="scan-icon" id="sicon">&#x1f463;</div>
      <div class="scan-badge badge-idle" id="sbadge">IDLE</div>
      <div class="scan-name" id="sname">Tekan tombol untuk mulai</div>
      <div style="font-size:12px;color:var(--dim)" id="sconf"></div>
    </div>
    <button class="btn btn-g" id="scanBtn" onclick="toggleScan()">Mulai Scan</button>
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
  <div class="card"><h3>Scan Network</h3>
    <button class="btn btn-o" onclick="scanWifi()">Scan</button>
    <div id="wlist" style="margin-top:8px"></div>
  </div>
  <div class="card"><h3>Connect to WiFi</h3>
    <label>SSID</label><input id="wssid" placeholder="Network name" readonly>
    <label>Password</label><input id="wpass" type="password" placeholder="Password">
    <button class="btn btn-c" onclick="saveWifi()">Simpan & Reboot</button>
    <button class="btn btn-r" id="wresetBtn" onclick="resetWifi()" style="display:none">Hapus WiFi & Reboot</button>
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

<script>
var autoOn=false,scanRunning=false;
function go(s){document.querySelectorAll('.page').forEach(p=>p.classList.remove('on'));
document.getElementById('p-'+s).classList.add('on');
document.querySelectorAll('.tab').forEach((t,i)=>{t.classList.toggle('on',['dash','enroll','scan','data','wifi','setel'][i]===s)});
if(s==='data')loadData();if(s==='wifi')loadWifiStatus();if(s==='setel')loadSettings();if(s==='enroll')loadBranchList()}
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
document.getElementById('dscan').textContent=d.autoActive?'ACTIVE':'IDLE';
document.getElementById('dscan').className='stat '+(d.autoActive?'stat-g':'stat-r');
document.getElementById('dwmode').textContent=d.wifiMode;
document.getElementById('dwmode').style.color=d.wifiMode==='STA'?'var(--green)':'var(--yellow)';
document.getElementById('dwip').textContent=d.wifiMode==='STA'?d.staIP:'192.168.4.1';
var e=document.getElementById('sfg-wmode');if(e)e.textContent=d.wifiMode;
var e=document.getElementById('sfg-ip');if(e)e.textContent=d.wifiMode==='STA'?d.staIP:'192.168.4.1';
var e=document.getElementById('sfg-fp');if(e)e.textContent=d.count+' templates | baud:'+d.baud;
autoOn=d.autoOn;if(d.autoActive)go('scan');
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

function toggleScan(){
if(!scanRunning){api('/api/autoscan/on','POST').then(d=>{if(d.ok){scanRunning=true;
document.getElementById('scanBtn').textContent='Stop Scan';
document.getElementById('scanBtn').className='btn btn-r';
setScanState('active','MENUNGGU','Menempelkan jari...')}})}
else{api('/api/autoscan/off','POST').then(()=>{scanRunning=false;
document.getElementById('scanBtn').textContent='Mulai Scan';
document.getElementById('scanBtn').className='btn btn-g';
setScanState('','IDLE','Tekan tombol untuk mulai')})}}

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
document.getElementById('wresetBtn').style.display=d.hasSaved?'block':'none';
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
else if(t==='autoscan_on'){
scanRunning=true;setScanState('active','MENUNGGU','Menempelkan jari...');
document.getElementById('scanBtn').textContent='Stop Scan';
document.getElementById('scanBtn').className='btn btn-r'}
else if(t==='autoscan_off'){
scanRunning=false;setScanState('','IDLE','Tekan tombol untuk mulai');
document.getElementById('scanBtn').textContent='Mulai Scan';
document.getElementById('scanBtn').className='btn btn-g'}
else if(t==='autoscan_err')
addLog('slog','er','Scan error: '+(o.step||'')+' code:'+(o.code||''));
else if(t==='attendance'){
var st=o.response||{};
var msg=st.status||'unknown';
var cols={checkin:'ok',checkout:'cy',not_found:'er',ignored:'t',error:'er'};
var labels={checkin:'ABSEN MASUK',checkout:'ABSEN PULANG',not_found:'TIDAK DIKENALI',ignored:'SUDAH ABSEN',error:'ERROR'};
setScanState(msg==='checkin'||msg==='checkout'?'ok':'fail',labels[msg]||msg.toUpperCase(),'');
addLog('slog',cols[msg]||'t','Attendance: '+msg+(o.response?' '+JSON.stringify(o.response):''))}
updStatus()}

updStatus();setInterval(updStatus,5000);
</script>
</body>
</html>
)rawliteral";

#endif
