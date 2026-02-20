/*************************************************************
  VT220 / xterm-256 Terminal Emulator
  ESP32 N4R4 + ILI9341 240x320 TFT
  
  Wiring:
    GND  → target GND
    RX   → target TX
    TX   → target RX
    This can also apply to your Linux computer, if you are using UART to USB converter.
    It also applies to Raspberry Pi.
    This can also be used to monitor devices such as arduinos using same baud.

  Features:
    - xterm-256 colour (RGB565)
    - Cursor positioning
    - Clear screen / line
    - Cursor movement (ABCD)
    - Soft scroll
    - Cell framebuffer in PSRAM
 *************************************************************/

#include <TFT_eSPI.h>
#include <SPI.h>
#include "fonts/iosevka_ascii.h"
#include "fonts/iosevka_box.h"
#include "fonts/iosevka_blocks.h"
#include "fonts/iosevka_braille.h"

// Use Iosevka GFX fonts. Undef to use TFT_eSPI built-in font 1.
#define USE_IOSEVKA_FONT  1

TFT_eSPI tft = TFT_eSPI();

// ── Grid dimensions ──────────────────────────────────────────
#define CHAR_W       6    // Iosevka character width (pixels) - fixed for terminal
#define CHAR_H      12    // Iosevka character height (yAdvance from font)
#define FONT_NUM     1    // fallback font number

#define COLS        40    // (240 / 6)
#define ROWS        26    // (320 / 12) - adjusted for Iosevka yAdvance

// Change SCREEN_W, SCREEN_H if needed
#define SCREEN_W   240
#define SCREEN_H   320

// ── Login button ─────────────────────────────────────────────
#define LOGIN_BTN   32

// ── Default colours (xterm index) ────────────────────────────
#define DEFAULT_FG  7     // white
#define DEFAULT_BG  0     // black

// ── Cell structure ────────────────────────────────────────────
struct Cell {
  char     ch;
  uint8_t  fg;   // xterm-256 index
  uint8_t  bg;
};

// Allocated in PSRAM
Cell* screen = nullptr;
// ── Cursor ───────────────────────────────────────────────────
int16_t curX = 0, curY = 0;
uint8_t curFG = DEFAULT_FG;
uint8_t curBG = DEFAULT_BG;
bool    cursorVisible = true;
#define CURSOR_BLINK_MS  530
uint32_t cursorBlinkLast = 0;
bool     cursorBlinkOn   = true;

// ── Escape sequence parser ────────────────────────────────────
enum ParserState { S_NORMAL, S_ESC, S_CSI };
ParserState parserState = S_NORMAL;

#define MAX_PARAMS 8
#define MAX_INTER  4
int16_t  csiParams[MAX_PARAMS];
uint8_t  csiParamCount = 0;
char     csiInter[MAX_INTER];
uint8_t  csiInterCount = 0;

// ── xterm-256 standard colours (indices 0-15) ─────────────────
// Stored in flash
const uint32_t ANSI16[] PROGMEM = {
  0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
  0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
  0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
  0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
};

// ── RGB888 → RGB565 ───────────────────────────────────────────
inline uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ── xterm-256 index → RGB565 ──────────────────────────────────
uint16_t xterm256(uint8_t idx) {
  uint8_t r, g, b;
  if (idx < 16) {
    uint32_t c = pgm_read_dword(&ANSI16[idx]);
    r = (c >> 16) & 0xFF;
    g = (c >>  8) & 0xFF;
    b =  c        & 0xFF;
  } else if (idx < 232) {
    uint8_t i = idx - 16;
    r = (i / 36)       * 51;
    g = ((i / 6) % 6)  * 51;
    b = (i % 6)        * 51;
  } else {
    uint8_t v = (idx - 232) * 10 + 8;
    r = g = b = v;
  }
  return rgb888to565(r, g, b);
}

// ── Font lookup ───────────────────────────────────────────────
// Returns GFXfont* for given Unicode codepoint, or nullptr if not found
const GFXfont* getFontForChar(uint16_t codepoint) {
  if (codepoint >= 0x0020 && codepoint <= 0x007E) {
    return &IosevkaASCII;
  } else if (codepoint >= 0x2500 && codepoint <= 0x257F) {
    return &IosevkaBox;
  } else if (codepoint >= 0x2580 && codepoint <= 0x259F) {
    return &IosevkaBlocks;
  } else if (codepoint >= 0x2800 && codepoint <= 0x28FF) {
    return &IosevkaBraille;
  }
  return nullptr;
}

// ── Cell helpers ──────────────────────────────────────────────
inline Cell& cellAt(int16_t col, int16_t row) {
  return screen[row * COLS + col];
}

// Draw one character using Iosevka GFX fonts at fixed cell position
static inline void drawCharAt(int16_t x, int16_t y, char ch, uint16_t fg565, uint16_t bg565) {
#if USE_IOSEVKA_FONT
  uint16_t codepoint = (unsigned char)(ch ? ch : ' ');
  const GFXfont* font = getFontForChar(codepoint);
  
  if (font) {
    // Fill cell background first
    tft.fillRect(x, y, CHAR_W, CHAR_H, bg565);
    
    // Set font and colors
    tft.setFreeFont(font);
    tft.setTextColor(fg565, bg565);
    tft.setTextDatum(TL_DATUM); // Top-left datum for consistent positioning
    
    // Draw character at cell position
    // GFX fonts will render the glyph, and we've filled the background
    tft.drawChar(codepoint, x, y);
  } else {
    // Fallback: fill cell with background
    tft.fillRect(x, y, CHAR_W, CHAR_H, bg565);
  }
#else
  tft.setTextColor(fg565, bg565);
  tft.drawChar(ch ? ch : ' ', x, y, FONT_NUM);
#endif
}

void drawCell(int16_t col, int16_t row) {
  Cell& c = cellAt(col, row);
  int16_t x = col * CHAR_W;
  int16_t y = row * CHAR_H;
  drawCharAt(x, y, c.ch, xterm256(c.fg), xterm256(c.bg));
}

// Draw cursor (block) at current position
void drawCursorBlock() {
  if (!cursorVisible || !screen) return;
  int16_t x = curX * CHAR_W;
  int16_t y = curY * CHAR_H;
  uint16_t fg = xterm256(curFG);
  uint16_t bg = xterm256(curBG);
  tft.fillRect(x, y, CHAR_W, CHAR_H, bg);
  drawCharAt(x, y, cellAt(curX, curY).ch, bg, fg);  // inverted
}

void clearCell(int16_t col, int16_t row) {
  Cell& c = cellAt(col, row);
  c.ch = ' ';
  c.fg = curFG;
  c.bg = curBG;
  drawCell(col, row);
}

// ── Scroll buffer for hardware-backed scroll (one row of pixels) ─
#define SCROLL_ROW_PIXELS  (SCREEN_W * CHAR_H)
static uint16_t scrollRowBuf[SCROLL_ROW_PIXELS];

// ── Scroll up one row (hardware-backed: readRect + pushRect) ───
void scrollUp() {
  // Shift cell buffer up
  memmove(&screen[0], &screen[COLS], sizeof(Cell) * COLS * (ROWS - 1));
  for (int16_t col = 0; col < COLS; col++)
    cellAt(col, ROWS - 1) = {' ', curFG, curBG};

  // Copy pixel rows up: row N+1 -> row N
  for (int16_t row = 0; row < ROWS - 1; row++) {
    int16_t srcY = (row + 1) * CHAR_H;
    int16_t dstY = row * CHAR_H;
    tft.readRect(0, srcY, SCREEN_W, CHAR_H, scrollRowBuf);
    tft.pushRect(0, dstY, SCREEN_W, CHAR_H, scrollRowBuf);
  }
  // Clear and draw new bottom row
  tft.fillRect(0, (ROWS - 1) * CHAR_H, SCREEN_W, CHAR_H, xterm256(curBG));
  for (int16_t col = 0; col < COLS; col++)
    drawCell(col, ROWS - 1);
}

// ── Cursor movement ───────────────────────────────────────────
void moveCursor(int16_t col, int16_t row) {
  int16_t ox = curX, oy = curY;
  curX = constrain(col, 0, COLS - 1);
  curY = constrain(row, 0, ROWS - 1);
  if (ox != curX || oy != curY) {
    drawCell(ox, oy);  // erase cursor from old position
    drawCursorBlock();
    cursorBlinkLast = millis();
    cursorBlinkOn = true;
  }
}

void cursorAdvance() {
  curX++;
  if (curX >= COLS) {
    curX = 0;
    curY++;
    if (curY >= ROWS) {
      scrollUp();
      curY = ROWS - 1;
    }
  }
}

// ── Put a character at cursor ─────────────────────────────────
void putChar(char ch) {
  Cell& c = cellAt(curX, curY);
  c.ch = ch;
  c.fg = curFG;
  c.bg = curBG;
  drawCell(curX, curY);
  cursorAdvance();
  drawCursorBlock();
  cursorBlinkLast = millis();
  cursorBlinkOn = true;
}

// ── CSI dispatch ──────────────────────────────────────────────
void dispatchCSI(char cmd) {
  int16_t p0 = csiParamCount > 0 ? csiParams[0] : 0;
  int16_t p1 = csiParamCount > 1 ? csiParams[1] : 0;

  switch (cmd) {

    // Cursor Up
    case 'A':
      moveCursor(curX, curY - max((int16_t)1, p0));
      break;

    // Cursor Down
    case 'B':
      moveCursor(curX, curY + max((int16_t)1, p0));
      break;

    // Cursor Forward
    case 'C':
      moveCursor(curX + max((int16_t)1, p0), curY);
      break;

    // Cursor Back
    case 'D':
      moveCursor(curX - max((int16_t)1, p0), curY);
      break;

    // Cursor Position  ESC[row;colH  (1-indexed)
    case 'H':
    case 'f':
      moveCursor(
        (p1 > 0 ? p1 : 1) - 1,
        (p0 > 0 ? p0 : 1) - 1
      );
      break;

    // Erase in Display
    case 'J':
      if (p0 == 0) {
        // Clear from cursor to end
        for (int16_t c = curX; c < COLS; c++) clearCell(c, curY);
        for (int16_t r = curY + 1; r < ROWS; r++)
          for (int16_t c = 0; c < COLS; c++) clearCell(c, r);
      } else if (p0 == 1) {
        // Clear from start to cursor
        for (int16_t r = 0; r < curY; r++)
          for (int16_t c = 0; c < COLS; c++) clearCell(c, r);
        for (int16_t c = 0; c <= curX; c++) clearCell(c, curY);
      } else if (p0 == 2) {
        // Clear entire screen
        tft.fillScreen(xterm256(curBG));
        for (int16_t r = 0; r < ROWS; r++)
          for (int16_t c = 0; c < COLS; c++) {
            cellAt(c, r) = {' ', curFG, curBG};
          }
        moveCursor(0, 0);
      }
      break;

    // Erase in Line
    case 'K':
      if (p0 == 0) {
        for (int16_t c = curX; c < COLS; c++) clearCell(c, curY);
      } else if (p0 == 1) {
        for (int16_t c = 0; c <= curX; c++) clearCell(c, curY);
      } else if (p0 == 2) {
        for (int16_t c = 0; c < COLS; c++) clearCell(c, curY);
      }
      break;

    // SGR — Select Graphic Rendition
    case 'm': {
      if (csiParamCount == 0) {
        // ESC[m = reset
        curFG = DEFAULT_FG;
        curBG = DEFAULT_BG;
        break;
      }
      uint8_t i = 0;
      while (i < csiParamCount) {
        int16_t p = csiParams[i];
        if (p == 0) {
          curFG = DEFAULT_FG;
          curBG = DEFAULT_BG;
        } else if (p == 39) {
          curFG = DEFAULT_FG;
        } else if (p == 49) {
          curBG = DEFAULT_BG;
        } else if (p >= 30 && p <= 37) {
          curFG = p - 30;
        } else if (p >= 40 && p <= 47) {
          curBG = p - 40;
        } else if (p >= 90 && p <= 97) {
          curFG = p - 90 + 8;  // bright fg
        } else if (p >= 100 && p <= 107) {
          curBG = p - 100 + 8; // bright bg
        } else if (p == 38 && i + 2 < csiParamCount && csiParams[i+1] == 5) {
          // ESC[38;5;nm — xterm-256 fg
          curFG = (uint8_t)min(255, max(0, csiParams[i + 2]));
          i += 2;
        } else if (p == 48 && i + 2 < csiParamCount && csiParams[i+1] == 5) {
          // ESC[48;5;nm — xterm-256 bg
          curBG = (uint8_t)min(255, max(0, csiParams[i + 2]));
          i += 2;
        }
        // 38;2;r;g;b truecolor — map to nearest 256 not implemented, skip
        i++;
      }
      break;
    }

    default:
      break;
  }
}

// ── Feed a byte into the parser ───────────────────────────────
void processByte(uint8_t b) {
  switch (parserState) {

    case S_NORMAL:
      if (b == 0x1B) {
        parserState = S_ESC;
      } else if (b == '\r') {
        drawCell(curX, curY);
        curX = 0;
        drawCursorBlock();
        cursorBlinkLast = millis();
        cursorBlinkOn = true;
      } else if (b == '\n') {
        drawCell(curX, curY);
        curY++;
        if (curY >= ROWS) {
          scrollUp();
          curY = ROWS - 1;
        }
        drawCursorBlock();
        cursorBlinkLast = millis();
        cursorBlinkOn = true;
      } else if (b == '\b') {
        if (curX > 0) {
          drawCell(curX, curY);
          curX--;
          drawCursorBlock();
          cursorBlinkLast = millis();
          cursorBlinkOn = true;
        }
      } else if (b >= 0x20 && b < 0x7F) {
        putChar((char)b);
      }
      break;

    case S_ESC:
      if (b == '[') {
        parserState = S_CSI;
        csiParamCount = 0;
        csiInterCount = 0;
        memset(csiParams, 0, sizeof(csiParams));
        memset(csiInter, 0, sizeof(csiInter));
      } else if (b == 'c') {
        // Full reset (RIS)
        curFG = DEFAULT_FG;
        curBG = DEFAULT_BG;
        moveCursor(0, 0);
        if (screen) {
          for (int16_t i = 0; i < COLS * ROWS; i++)
            screen[i] = {' ', DEFAULT_FG, DEFAULT_BG};
        }
        tft.fillScreen(xterm256(DEFAULT_BG));
        parserState = S_NORMAL;
      } else {
        // Unknown ESC sequence, bail
        parserState = S_NORMAL;
      }
      break;

    case S_CSI:
      if (b >= '0' && b <= '9') {
        if (csiParamCount == 0) csiParamCount = 1;
        csiParams[csiParamCount - 1] =
          csiParams[csiParamCount - 1] * 10 + (b - '0');
      } else if (b == ';') {
        csiParamCount++;
        if (csiParamCount >= MAX_PARAMS) csiParamCount = MAX_PARAMS - 1;
      } else if (b >= 0x20 && b <= 0x2F) {
        // Intermediate byte
        if (csiInterCount < MAX_INTER)
          csiInter[csiInterCount++] = b;
      } else if (b >= 0x40 && b <= 0x7E) {
        // Final byte — dispatch
        dispatchCSI((char)b);
        parserState = S_NORMAL;
      } else {
        // Unexpected, abort
        parserState = S_NORMAL;
      }
      break;
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  // Allocate framebuffer in PSRAM
  screen = (Cell*)ps_malloc(sizeof(Cell) * COLS * ROWS);
  if (!screen) {
    // Fallback to internal RAM — should not happen on N4R4
    screen = (Cell*)malloc(sizeof(Cell) * COLS * ROWS);
  }

  // Blank the buffer
  for (int16_t i = 0; i < COLS * ROWS; i++) {
    screen[i] = {' ', DEFAULT_FG, DEFAULT_BG};
  }

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
#if USE_IOSEVKA_FONT
  // GFX fonts are set per-character in drawCharAt()
  // Default to ASCII font for initial setup
  tft.setFreeFont(&IosevkaASCII);
#else
  tft.setTextFont(FONT_NUM);
  tft.setTextSize(1);
#endif

  Serial.begin(115200);

  pinMode(LOGIN_BTN, INPUT_PULLDOWN);
}

// ── Cursor blink (call from loop) ─────────────────────────────
void updateCursorBlink() {
  if (!cursorVisible || !screen) return;
  uint32_t now = millis();
  if (now - cursorBlinkLast >= CURSOR_BLINK_MS) {
    cursorBlinkLast = now;
    cursorBlinkOn = !cursorBlinkOn;
    if (cursorBlinkOn)
      drawCursorBlock();
    else
      drawCell(curX, curY);
  }
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  // Drain serial into parser
  while (Serial.available()) {
    processByte((uint8_t)Serial.read());
  }

  updateCursorBlink();

  // Login button — sends username then password on second press
  if (digitalRead(LOGIN_BTN) == HIGH) {
    Serial.print("pfetch\r");
    while (digitalRead(LOGIN_BTN) == HIGH);
  }
}
