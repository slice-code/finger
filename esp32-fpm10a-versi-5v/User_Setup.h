// User_Setup.h untuk TFT_eSPI - ESP32 + ILI9341
// Letakkan file ini di: <Arduino>/libraries/TFT_eSPI/User_Setup.h
// (timpa file aslinya)

#ifndef USER_SETUP_H
#define USER_SETUP_H

// ── Driver ILI9341 ─────────────────────────────────────────────────
#define ILI9341_DRIVER

// ── Resolusi ───────────────────────────────────────────────────────
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ── Pin SPI untuk ESP32 (VSPI) ──────────────────────────────────────
// SCK  = GPIO18 (VSPI CLK)
// MOSI = GPIO23 (VSPI MOSI)
// CS   = GPIO5
// DC   = GPIO2
// RST  = GPIO4
// BL   = GPIO15

#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_CS     5
#define TFT_DC     2
#define TFT_RST    4

// Backlight - set HIGH untuk hidupkan
#define TFT_BL    15

// ── SPI frequency ──────────────────────────────────────────────────
#define SPI_FREQUENCY  40000000  // 40 MHz
#define SPI_READ_FREQUENCY  20000000

// ── Font ───────────────────────────────────────────────────────────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

#endif
