/*************************************************************
  VT220 / xterm-256 Terminal Emulator
  ESP32 N4R4 + ILI9341 240x320 TFT

  Wiring:
    GND  → target GND
    RX   → target TX
    TX   → target RX
    (Linux/RPi/Arduino over UART or USB-serial.)

  Features:
    - Envy GFX fonts (ASCII, box, blocks, braille)
    - xterm-256 colour (RGB565)
    - Cursor positioning, movement (ABCD), home (H/f)
    - Clear screen/line (J/K), erase to end/start
    - In-place line updates: \r to start of line, overwrite; tab (\t)
    - Bash-style: DECSC/DECRC (save/restore cursor), DEC private ?25h/?25l (cursor show/hide)
    - Hardware-backed scroll, blinking cursor, cell framebuffer in PSRAM
 *************************************************************/


// Graphics 
#include <TFT_eSPI.h>
#include <SPI.h>

// Include Envy GFX fonts (ASCII, box drawing, blocks, braille)
#include "fonts/envy_ascii.h"
#include "fonts/envy_box.h"
#include "fonts/envy_blocks.h"
#include "fonts/envy_braille.h"

#define USE_GFX_FONT  1   // Use Envy GFX fonts; undef for TFT_eSPI built-in font 1
#undef USE_GFX_FONT
#define TAB_STOPS    8   // Tab stop interval (bash-style)

TFT_eSPI tft = TFT_eSPI();

// ── Grid dimensions ──────────────────────────────────────────
#define CHAR_W       6    // Character width (pixels) - fixed for terminal
#define CHAR_H      12    // Character height (yAdvance from font)
#define FONT_NUM     1    // fallback font number

#define COLS        53    // (240 / 6)
#define ROWS        20    // (320 / 12) - adjusted for yAdvance

// Change SCREEN_W, SCREEN_H if needed
#define SCREEN_W   320
#define SCREEN_H   240


// Scroll


// ── Default colours (xterm index) ────────────────────────────
#define DEFAULT_FG  7     // white
#define DEFAULT_BG  0     // black

// ── Cell structure ────────────────────────────────────────────
struct Cell {
  char     ch;
  uint8_t  fg;   // xterm-256 index
  uint8_t  bg;
};

// Allocated in PSRAM - buffers
Cell* screen = nullptr;
Cell prev[COLS * ROWS];


// ── Cursor ───────────────────────────────────────────────────
int16_t curX = 0, curY = 0;
uint8_t curFG = DEFAULT_FG;
uint8_t curBG = DEFAULT_BG;
bool    cursorVisible = true;
#define CURSOR_BLINK_MS  530
uint32_t cursorBlinkLast = 0;
bool     cursorBlinkOn   = true;
// Saved cursor (DECSC/DECRC, bash prompt restore)
int16_t saveCurX = 0, saveCurY = 0;

// DEC private modes and terminal state
bool    decawm = true;     // DECAWM (?7): auto-wrap mode (default ON per VT220 RM)
bool    decckm = false;    // DECCKM (?1): cursor key application mode (default OFF)
bool    dectcem = true;    // DECTCEM (?25): text cursor enable mode (default visible)
bool    pendingWrap = false; // Internal: pending wrap after writing at right margin

// Scroll region (DECSTBM)
int16_t scrollTop = 0;
int16_t scrollBottom = ROWS - 1;

// Alternate screen buffers (1047/1048/1049)
Cell* mainScreen = nullptr;
Cell* altScreen = nullptr;
bool  altScreenActive = false;


// ── Escape sequence parser ────────────────────────────────────
enum ParserState { S_NORMAL, S_ESC, S_CSI };
ParserState parserState = S_NORMAL;
bool        csiPrivate = false;  // ESC[?... (DEC private mode)

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
    return &EnvyASCII;
  } else if (codepoint >= 0x2500 && codepoint <= 0x257F) {
    return &EnvyBox;
  } else if (codepoint >= 0x2580 && codepoint <= 0x259F) {
    return &EnvyBlocks;
  } else if (codepoint >= 0x2800 && codepoint <= 0x28FF) {
    return &EnvyBraille;
    }
  return nullptr;
}

// ── Cell helpers ──────────────────────────────────────────────
inline Cell& cellAt(int16_t col, int16_t row) {
  return screen[row * COLS + col];
}

inline Cell& prevAt(int16_t col, int16_t row) {
  return prev[row * COLS + col];
}

// Draw one character using GFX fonts at fixed cell position
static inline void drawCharAt(int16_t x, int16_t y, char ch, uint16_t fg565, uint16_t bg565) {
#if USE_GFX_FONT
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
  Cell& p = prevAt(col, row);
  int16_t x = col * CHAR_W;
  int16_t y = row * CHAR_H;
  if (c.ch != p.ch || c.fg != p.fg || c.bg != p.bg) {
    drawCharAt(x, y, c.ch, xterm256(c.fg), xterm256(c.bg));
    p = c;  // mark as drawn
  }
}

// Redraw a single row (for in-place line updates, e.g. \r then overwrite, or ESC[K)
void drawRow(int16_t row) {
  for (int16_t col = 0; col < COLS; col++)
    drawCell(col, row);
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

// ── Input sequences (cursor / function keys) ─────────────────
void sendUp() {
  if (decckm) { Serial.write("\033O"); Serial.write('A'); }
  else { Serial.write("\033["); Serial.write('A'); }
}
void sendDown() {
  if (decckm) { Serial.write("\033O"); Serial.write('B'); }
  else { Serial.write("\033["); Serial.write('B'); }
}
void sendRight() {
  if (decckm) { Serial.write("\033O"); Serial.write('C'); }
  else { Serial.write("\033["); Serial.write('C'); }
}
void sendLeft() {
  if (decckm) { Serial.write("\033O"); Serial.write('D'); }
  else { Serial.write("\033["); Serial.write('D'); }
}
void sendHome() {
  if (decckm) { Serial.write("\033O"); Serial.write('H'); }
  else { Serial.write("\033["); Serial.write('H'); }
}
void sendEnd() {
  if (decckm) { Serial.write("\033O"); Serial.write('F'); }
  else { Serial.write("\033["); Serial.write('F'); }
}
void sendF1() { Serial.write("\033O"); Serial.write('P'); }
void sendF2() { Serial.write("\033O"); Serial.write('Q'); }
void sendF3() { Serial.write("\033O"); Serial.write('R'); }
void sendF4() { Serial.write("\033O"); Serial.write('S'); }
void sendInsert() { Serial.write("\033["); Serial.write("2~"); }
void sendDelete() { Serial.write("\033["); Serial.write("3~"); }

// ── Scroll up one row (within scroll region) ─────
void scrollUp() {
  int start = scrollTop;
  int end = scrollBottom;
  if (start >= end) return;
  // Move content up within region
  memmove(&screen[start * COLS], &screen[(start + 1) * COLS], sizeof(Cell) * COLS * (end - start));
  // Clear bottom line of region
  for (int16_t col = 0; col < COLS; col++)
    cellAt(col, end) = {' ', curFG, curBG};
  // Invalidate prev for region
  memset(&prev[start * COLS], 0x7F, sizeof(prev[0]) * COLS * (end - start + 1));
  // Redraw region
  for (int16_t row = start; row <= end; row++)
    drawRow(row);
  tft.fillRect(0, end * CHAR_H, SCREEN_W, CHAR_H, xterm256(curBG));
}

// ── Scroll down one row (within scroll region) ─────
void scrollDown() {
  int start = scrollTop;
  int end = scrollBottom;
  if (start >= end) return;
  // Move content down within region
  memmove(&screen[(start + 1) * COLS], &screen[start * COLS], sizeof(Cell) * COLS * (end - start));
  // Clear top line of region
  for (int16_t col = 0; col < COLS; col++)
    cellAt(col, start) = {' ', curFG, curBG};
  memset(&prev[start * COLS], 0x7F, sizeof(prev[0]) * COLS * (end - start + 1));
  for (int16_t row = start; row <= end; row++)
    drawRow(row);
  tft.fillRect(0, start * CHAR_H, SCREEN_W, CHAR_H, xterm256(curBG));
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
  if (pendingWrap) {
    pendingWrap = false;
    curX = 0;
    curY++;
    if (curY > scrollBottom) {
      scrollUp();
      curY = scrollBottom;
    }
  } else {
    curX++;
    if (curX >= COLS) {
      if (decawm) {
        pendingWrap = true;
        curX = COLS - 1; // Stay at right margin
      } else {
        curX = COLS - 1; // DECAWM off: stay at right margin
      }
    }
  }
}

// ── Put a character at cursor ─────────────────────────────────
void putChar(char ch) {
  // Resolve pending wrap from previous right-margin write
  if (pendingWrap) {
    pendingWrap = false;
    curX = 0;
    curY++;
    if (curY > scrollBottom) {
      scrollUp();
      curY = scrollBottom;
    }
  }

  Cell& c = cellAt(curX, curY);
  c.ch = ch;
  c.fg = curFG;
  c.bg = curBG;
  drawCell(curX, curY);
  prevAt(curX, curY).ch = 0x7F;
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

    // Erase in Display (bash: clear screen / clear to end)
    case 'J':
      if (p0 == 0) {
        for (int16_t c = curX; c < COLS; c++) clearCell(c, curY);
        for (int16_t r = curY + 1; r < ROWS; r++)
          for (int16_t c = 0; c < COLS; c++) clearCell(c, r);
      } else if (p0 == 1) {
        for (int16_t r = 0; r < curY; r++)
          for (int16_t c = 0; c < COLS; c++) clearCell(c, r);
        for (int16_t c = 0; c <= curX; c++) clearCell(c, curY);
      } else if (p0 == 2 || p0 == 3) {
        // 2 = clear all; 3 = clear all + scrollback (we only have screen)
        tft.fillScreen(xterm256(curBG));
        for (int16_t r = 0; r < ROWS; r++)
          for (int16_t c = 0; c < COLS; c++)
            cellAt(c, r) = {' ', curFG, curBG};
	memcpy(prev, screen, sizeof(Cell) * COLS * ROWS);
        moveCursor(0, 0);
	drawCursorBlock();
      }
      break;

    // Erase in Line (bash uses this for clear-to-end-of-line)
    case 'K':
      if (p0 == 0) {
        for (int16_t c = curX; c < COLS; c++) clearCell(c, curY);
      } else if (p0 == 1) {
        for (int16_t c = 0; c <= curX; c++) clearCell(c, curY);
      } else if (p0 == 2) {
        for (int16_t c = 0; c < COLS; c++) clearCell(c, curY);
      }
      break;

    // DECSTBM — Set scroll region (CSI t ; b r, 1-indexed)
    case 'r': {
      if (p0 == 0 && p1 == 0) {
        scrollTop = 0;
        scrollBottom = ROWS - 1;
      } else {
        scrollTop = (p0 > 0 ? p0 - 1 : scrollTop);
        scrollBottom = (p1 > 0 ? p1 - 1 : scrollBottom);
        if (scrollTop < 0) scrollTop = 0;
        if (scrollBottom >= ROWS) scrollBottom = ROWS - 1;
        if (scrollTop > scrollBottom) {
          // Invalid: reset to full screen
          scrollTop = 0;
          scrollBottom = ROWS - 1;
        }
      }
      break;
    }

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
          int16_t val = csiParams[i + 2];
          if (val < 0) val = 0;
          if (val > 255) val = 255;
          curFG = (uint8_t)val;
          i += 2;
        } else if (p == 48 && i + 2 < csiParamCount && csiParams[i+1] == 5) {
          // ESC[48;5;nm — xterm-256 bg
          int16_t val = csiParams[i + 2];
          if (val < 0) val = 0;
          if (val > 255) val = 255;
          curBG = (uint8_t)val;
          i += 2;
        }
        // 38;2;r;g;b truecolor — map to nearest 256 not implemented, skip
        i++;
      }
      break;
    }

    // DEC private mode (ESC[? ... h / l)
    case 'h':
    case 'l':
      if (csiPrivate) {
        bool set = (cmd == 'h');
        for (uint8_t k = 0; k < csiParamCount; k++) {
          int16_t p = csiParams[k];
          switch (p) {
            case 1:   // DECCKM — Cursor Key Application Mode
              decckm = set;
              break;
            case 7:   // DECAWM — Auto Wrap Mode (default ON)
              decawm = set;
              break;
            case 25:  // DECTCEM — Cursor visibility
              dectcem = set;
              cursorVisible = set;
              break;
            case 47:  // Alternate screen buffer (original DEC)
            case 1047: // Alternate screen buffer (xterm)
              if (set) {
                altScreenActive = true;
                screen = altScreen ? altScreen : mainScreen;
                if (altScreen) {
                  for (int16_t i = 0; i < COLS * ROWS; i++)
                    altScreen[i] = {' ', curFG, curBG};
                }
                memset(prev, 0x7F, sizeof(prev));
                for (int16_t row = 0; row < ROWS; row++) drawRow(row);
              } else {
                altScreenActive = false;
                screen = mainScreen ? mainScreen : altScreen;
                memset(prev, 0x7F, sizeof(prev));
                for (int16_t row = 0; row < ROWS; row++) drawRow(row);
              }
              break;
            case 1048: // Save/restore cursor
              if (set) {
                saveCurX = curX;
                saveCurY = curY;
              } else {
                moveCursor(saveCurX, saveCurY);
              }
              break;
            case 1049: // Alternate screen + save/restore cursor
              if (set) {
                saveCurX = curX;
                saveCurY = curY;
                altScreenActive = true;
                screen = altScreen ? altScreen : mainScreen;
                if (altScreen) {
                  for (int16_t i = 0; i < COLS * ROWS; i++)
                    altScreen[i] = {' ', curFG, curBG};
                }
                memset(prev, 0x7F, sizeof(prev));
                for (int16_t row = 0; row < ROWS; row++) drawRow(row);
              } else {
                altScreenActive = false;
                screen = mainScreen ? mainScreen : altScreen;
                moveCursor(saveCurX, saveCurY);
                memset(prev, 0x7F, sizeof(prev));
                for (int16_t row = 0; row < ROWS; row++) drawRow(row);
              }
              break;
            // Ignore unknown/safe modes (66 DECNKM, 67 DECBKM, etc.)
            default:
              break;
          }
        }
      }
      break;

    // Save cursor (DECSC) — also used by bash for prompt position
    // Restore cursor (DECRC) — handled in ESC branch

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
        // Carriage return: move to start of line (in-place line update)
        drawCell(curX, curY);
        curX = 0;
        drawCursorBlock();
        cursorBlinkLast = millis();
        cursorBlinkOn = true;
      } else if (b == '\n' || b == '\v' || b == '\f') {
        // Newline / vertical tab / form feed: next line, scroll if needed
        drawCell(curX, curY);
        // Resolve any pending wrap before moving to next line
        if (pendingWrap) {
          pendingWrap = false;
        }
        curX = 0; // Reset to start of line
        curY++;
        if (curY > scrollBottom) {
          scrollUp();
          curY = scrollBottom;
        }
        drawCursorBlock();
        cursorBlinkLast = millis();
        cursorBlinkOn = true;
      } else if (b == '\t') {
        // Tab: advance to next tab stop (bash-style)
        drawCell(curX, curY);
        curX = (curX / TAB_STOPS + 1) * TAB_STOPS;
        if (curX >= COLS) {
          curX = COLS - 1;
          cursorAdvance();  // wrap to next line if at end
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
      } else if (b < 0x20) {
        // Simulated keyboard input (lightweight, no physical keyboard needed)
        switch (b) {
          case 0x01: sendUp(); break;        // SOH = Cursor Up
          case 0x02: sendDown(); break;      // STX = Cursor Down
          case 0x03: sendRight(); break;     // ETX = Cursor Right
          case 0x04: sendLeft(); break;      // EOT = Cursor Left
          case 0x05: sendHome(); break;      // ENQ = Home
          case 0x06: sendEnd(); break;       // ACK = End
          case 0x11: sendF1(); break;        // DC1 = F1
          case 0x12: sendF2(); break;        // DC2 = F2
          case 0x13: sendF3(); break;        // DC3 = F3
          case 0x14: sendF4(); break;        // DC4 = F4
          case 0x15: Serial.write("\033[15~"); break; // NAK = F5
          case 0x16: Serial.write("\033[17~"); break; // SYN = F6
          case 0x17: Serial.write("\033[18~"); break; // ETB = F7
          case 0x18: Serial.write("\033[19~"); break; // CAN = F8
          case 0x19: Serial.write("\033[21~"); break; // EM = F10
          default: break; // Ignore other control chars
        }
      } else if (b == 0x7F) {
        sendDelete(); // DEL = Delete
      }
      break;

    case S_ESC:
      if (b == '[') {
        parserState = S_CSI;
        csiPrivate = false;
        csiParamCount = 0;
        csiInterCount = 0;
        memset(csiParams, 0, sizeof(csiParams));
        memset(csiInter, 0, sizeof(csiInter));
      } else if (b == '7') {
        // DECSC: save cursor (bash uses to restore prompt position)
        saveCurX = curX;
        saveCurY = curY;
        parserState = S_NORMAL;
      } else if (b == '8') {
        // DECRC: restore cursor
        moveCursor(saveCurX, saveCurY);
        parserState = S_NORMAL;
      } else if (b == 'M') {
        // RI: reverse index (scroll down one line, move cursor up)
        if (curY > 0) {
          curY--;
          drawCursorBlock();
          cursorBlinkLast = millis();
          cursorBlinkOn = true;
        }
        parserState = S_NORMAL;
      } else if (b == 'c') {
        // Full reset (RIS)
        curFG = DEFAULT_FG;
        curBG = DEFAULT_BG;
        moveCursor(0, 0);
        saveCurX = 0;
        saveCurY = 0;
        cursorVisible = true;
        decawm = true;
        decckm = false;
        dectcem = true;
        pendingWrap = false;
        scrollTop = 0;
        scrollBottom = ROWS - 1;
        altScreenActive = false;
        screen = mainScreen;
        if (screen) {
          for (int16_t i = 0; i < COLS * ROWS; i++)
            screen[i] = {' ', DEFAULT_FG, DEFAULT_BG};
        }
        memset(prev, 0x7F, sizeof(prev));
        tft.fillScreen(xterm256(DEFAULT_BG));
        parserState = S_NORMAL;
      } else {
        parserState = S_NORMAL;
      }
      break;

    case S_CSI:
      if (b == '?') {
        csiPrivate = true;
      } else if (b >= '0' && b <= '9') {
        if (csiParamCount == 0) csiParamCount = 1;
        csiParams[csiParamCount - 1] =
          csiParams[csiParamCount - 1] * 10 + (b - '0');
      } else if (b == ';') {
        csiParamCount++;
        if (csiParamCount >= MAX_PARAMS) csiParamCount = MAX_PARAMS - 1;
      } else if (b >= 0x20 && b <= 0x2F) {
        if (csiInterCount < MAX_INTER)
          csiInter[csiInterCount++] = b;
      } else if (b >= 0x40 && b <= 0x7E) {
        dispatchCSI((char)b);
        parserState = S_NORMAL;
      } else {
        parserState = S_NORMAL;
      }
      break;
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  // Allocate main and alternate screen buffers in PSRAM
  mainScreen = (Cell*)ps_malloc(sizeof(Cell) * COLS * ROWS);
  altScreen = (Cell*)ps_malloc(sizeof(Cell) * COLS * ROWS);
  if (!mainScreen) mainScreen = (Cell*)malloc(sizeof(Cell) * COLS * ROWS);
  if (!altScreen) altScreen = (Cell*)malloc(sizeof(Cell) * COLS * ROWS);
  screen = mainScreen;

  // Blank both buffers
  for (int16_t i = 0; i < COLS * ROWS; i++) {
    if (mainScreen) mainScreen[i] = {' ', DEFAULT_FG, DEFAULT_BG};
    if (altScreen) altScreen[i] = {' ', DEFAULT_FG, DEFAULT_BG};
  }
  // Prev starts zeroed — differs from screen so first render draws everything
  memset(prev, 0x7F, sizeof(prev));

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
#if USE_GFX_FONT
  tft.setFreeFont(&EnvyASCII);
#else
  tft.setTextFont(FONT_NUM);
  tft.setTextSize(1);
#endif

  Serial.begin(115200);

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

}
