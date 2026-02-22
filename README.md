# esp32_vt220_serial_emu

A VT220 / xterm-256 serial terminal emulator running on an ESP32, driven to a cheap ILI9341 TFT display. Designed to receive bash output over UART and display it in a proper terminal grid with colour, cursor control, and hardware-backed scrolling.

## Hardware

| Part | Details |
|------|---------|
| MCU | ESP32-WROVER-E N4R4 (4MB Flash, 4MB QSPI PSRAM) |
| Display | ILI9341 320×240 TFT (landscape, 40×26 character grid) |
| Interface | SPI — MOSI, MISO, CLK, CS, DC wired |

Total cost is minimal — the Wrover-E and an ILI9341 breakout are both cheap and widely available.

## Features

- **VT220 / xterm-256 compatible** escape sequence parser
- **xterm-256 colour** — full 256 colour palette rendered in RGB565
- **Hardware-backed scroll** via ILI9341 VSCRSADD register — no pixel copying, single register write per scroll event
- **Cell framebuffer in PSRAM** — screen buffer allocated with `ps_malloc`, keeps internal SRAM free
- **Cursor control** — movement (A/B/C/D), positioning (H/f), home
- **DEC private modes** — `?25h`/`?25l` cursor show/hide, DECSC/DECRC save/restore
- **Erase sequences** — ED (J) and EL (K) with all variants (to end, to start, full)
- **In-place line updates** — `\r` carriage return overwrites in place, bash prompt redraw works correctly
- **Tab stops** — bash-style 8-column tab stops
- **Blinking block cursor** — 530ms blink interval
- **Reverse Index (RI)** — `ESC M` scroll down
- **Full Reset (RIS)** — `ESC c`
- **Custom font support** — Envy Code R GFX fonts for ASCII, box drawing, block elements, and braille

## Wiring

```
ESP32          ILI9341
------         -------
GND     →      GND
3.3V    →      VCC
MOSI    →      SDI/MOSI
MISO    →      SDO/MISO
CLK     →      SCK
CS      →      CS
DC      →      DC/RS
RST     →      RST

UART (to host):
GND     →      target GND
RX      →      target TX
TX      →      target RX
```

## Usage

Flash to the ESP32-WROVER-E via Arduino IDE or `arduino-cli`. Connect UART to a Linux host, then on the host:

```bash
sudo python test_vt220.py
```

Or you can open and redirect programs to it as you wish, maybe it will work, maybe not :P. Make sure you use stty first!
If you wanna run Bash it's in the ```test_vt220.py```.

## Dependencies

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — configure `User_Setup.h` for ILI9341 and your SPI pins
- [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) — for GFX font support

Install both via Arduino Library Manager.

## Known Limitations

- Output only — no keyboard input implemented yet, interactive programs will hang waiting for stdin
- 40×26 grid — modern TUI apps expecting 80×24 minimum may render incorrectly
- Braille / box drawing characters require a host terminal that sends the correct Unicode codepoints
- Truecolor `38;2;r;g;b` sequences are silently ignored — 256 colour is the maximum
