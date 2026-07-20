// User_Setup.h untuk TFT_eSPI - NodeMCU + ILI9341
// Letakkan file ini di: <Arduino>/libraries/TFT_eSPI/User_Setup.h
// (timpa file aslinya)

#ifndef USER_SETUP_H
#define USER_SETUP_H

// ── Driver ILI9341 ─────────────────────────────────────────────────
#define ILI9341_DRIVER

// ── Resolusi ───────────────────────────────────────────────────────
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ── Pin SPI untuk NodeMCU (ESP8266) ────────────────────────────────
// SCK  = D5 (GPIO14)
// MOSI = D7 (GPIO13)
// CS   = D8 (GPIO15)
// DC   = D3 (GPIO0)
// RST  = D4 (GPIO2)
// BL   = D6 (GPIO12)

#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15
#define TFT_DC     0
#define TFT_RST    2

// Backlight - set HIGH untuk hidupkan
#define TFT_BL    12

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
