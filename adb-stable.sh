#!/usr/bin/env bash
# adb-stable.sh — watchdog untuk menjaga koneksi adb wireless tetap stabil
#
# Kenapa adb putus-putus:
#   - WiFi sleep / power-save di HP (Xiaomi/MIUI agresif)
#   - Service wireless debugging mati saat layar kunci dalam waktu lama
#   - Port wireless debugging bisa berganti setelah reconnect
#
# Cara kerja:
#   1. Koneksi TCP langsung ke IP:port (bukan mDNS yang flaky)
#   2. Setiap N detik, cek device; kalau "offline"/hilang -> reconnect
#   3. Port port yang sama tapi gagal -> re-scan mDNS untuk port baru
#   4. Bunyi/notifikasi bila HP benar-benar tak terjangkau (untuk cek manual)

set -u
DEV="${1:-192.168.1.5:33959}"          # IP:port target (default dari mDNS)
INTERVAL="${2:-10}"                    # detik antara cek
ADB="${ADB:-adb}"
LOG="/tmp/adb-stable.log"
PING_SCRIPT="/tmp/adb-ping-wifi.sh"

log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

# Cek adb server jalan
$ADB start-server >/dev/null 2>&1

# Ping WiFi langsung ke HP — bukti HP masih di jaringan
cat > "$PING_SCRIPT" <<'EOF'
#!/usr/bin/env bash
# Usage: wifi-ping.sh <ip> ; 0=alive, 1=mati/gagal
ip="$1"
ping -c 1 -W 2 "$ip" >/dev/null 2>&1
EOF
chmod +x "$PING_SCRIPT"

# Anti-sleep di sisi HP (dijalankan setiap kali berhasil konek)
apply_phone_keepalive() {
    local s="$1"
    # wifi_sleep_policy=2 : WiFi selalu nyala (jangan tidur saat layar mati)
    $ADB -s "$s" shell settings put global wifi_sleep_policy 2 >/dev/null 2>&1
    # Matikan doze / standby untuk wireless debugging
    $ADB -s "$s" shell dumpsys deviceidle whitelist +com.android.shell >/dev/null 2>&1
    $ADB -s "$s" shell settings put global stay_on_while_plugged_in 3 >/dev/null 2>&1
    # Buka kunci layar supaya service wireless debugging tidak disuspend
    $ADB -s "$s" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
}

reconnect() {
    local serial="$1" host="${1%:*}" port="${1##*:}"
    # Coba port yang sama dulu
    if "$PING_SCRIPT" "$host" >/dev/null 2>&1; then
        if $ADB connect "$host:$port" 2>&1 | grep -qi 'connected'; then
            log "reconnect OK: $host:$port"
            apply_phone_keepalive "$host:$port"
            return 0
        fi
    fi
    # Gagal — cek mDNS untuk port terbaru (port berubah tiap restart wireless debugging)
    local new
    new=$($ADB mdns services 2>/dev/null | grep "$host" | grep -o '[0-9]*\.[0-9]*\.[0-9]*\.[0-9]*:[0-9]*' | head -1)
    if [ -n "${new:-}" ] && [ "$new" != "$host:$port" ]; then
        if $ADB connect "$new" 2>&1 | grep -qi 'connected'; then
            log "reconnect via mDNS (port baru): $new"
            apply_phone_keepalive "$new"
            DEV="$new"
            return 0
        fi
    fi
    log "GAGAL reconnect: $host tak terjangkau / service wireless debugging mati"
    log "  -> bangunkan HP (sentuh layar) atau nyalakan ulang Debugging Nirkabel"
    log "  -> lalu jalankan ulang: adb-stable.sh"
    return 1
}

log "=== adb-stable watchdog dimulai: target=$DEV interval=${INTERVAL}s ==="

while true; do
    if ! $ADB devices | grep -q "^$DEV[[:space:]]device"; then
        log "device hilang/offline, coba reconnect..."
        reconnect "$DEV" || sleep 15   # jangan spam kalau HP mati total
    fi
    sleep "$INTERVAL"
done
