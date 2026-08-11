# Wiring ESP32 + FPM10A + ILI9341 LCD + Relay

## Relay 1-Channel 5V

| Relay | ESP32 |
|-------|-------|
| +     | 5V (VIN) |
| -     | GND |
| S     | GPIO13 |

## FPM10A (3.3V) via Relay

| FPM10A     | Koneksi |
|------------|---------|
| VCC (merah) | Relay COM |
| GND (hitam) | ESP32 GND |
| TX (kuning) | GPIO16 |
| RX (putih)  | GPIO17 |

| Relay Output | Koneksi |
|-------------|---------|
| NO          | ESP32 3.3V |
| COM         | FPM10A VCC |
| NC          | (kosong) |

## LCD ILI9341 (langsung ke ESP32)

| LCD | ESP32 |
|-----|-------|
| VCC | 3.3V |
| GND | GND |
| CS  | GPIO5 |
| DC  | GPIO2 |
| RST | GPIO4 |
| BL  | GPIO15 |
| SCK | GPIO18 |
| MOSI| GPIO23 |
| LED | 3.3V |

## Ringkasan

```
ESP32 3.3V ─────┬── LCD VCC
                ├── LCD LED/BL
                └── Relay NO

Relay COM ─── FPM10A VCC

ESP32 5V  ─── Relay +
ESP32 GND ─── Relay -
ESP32 D13 ─── Relay S
```

- **LCD** langsung 3.3V — backlight dikontrol via GPIO15
- **FPM10A** lewat relay — bisa di-reset via GPIO13
- Backlight LCD mati otomatis setelah 60 detik idle, nyala saat ada jari
