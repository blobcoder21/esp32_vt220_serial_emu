#!/usr/bin/env python3
"""
VT220 terminal emulator test suite
ESP32 Wrover-E N4R4 + ILI9341
"""

import os
import sys
import time
import serial

TTY     = "/dev/ttyUSB0"
BAUD    = 115200
DELAY   = 0.05   # seconds between lines, tweak as needed

# ── helpers ───────────────────────────────────────────────────
def esc(seq: str) -> bytes:
    return f"\033{seq}".encode()

def csi(seq: str) -> bytes:
    return f"\033[{seq}".encode()

def write(tty: serial.Serial, data: str | bytes):
    if isinstance(data, str):
        data = data.encode()
    tty.write(data)
    tty.flush()

def cls(tty: serial.Serial):
    """Clear screen via ESC[2J + cursor home, then also fire /bin/cls"""
    write(tty, csi("2J"))
    write(tty, csi("H"))
    time.sleep(0.2)
    os.system("cls")   # your symlink

def pause(msg: str = ""):
    input(f"\n  [ENTER] {msg}> ")

# ── tests ─────────────────────────────────────────────────────

def test_scroll(tty: serial.Serial):
    print("TEST: scroll — sending 60 numbered lines to force multiple scrolls")
    cls(tty)
    for i in range(60):
        write(tty, f"scroll line {i:02d} {'.' * 20}\r\n")
        time.sleep(DELAY)
    pause("check scroll looked clean")

def test_colours_ansi16(tty: serial.Serial):
    print("TEST: ANSI 16 colours — fg and bg")
    cls(tty)
    for i in range(8):
        # normal fg
        write(tty, csi(f"{30+i}m"))
        write(tty, f" fg{i:02d} ".encode())
        # bright fg
        write(tty, csi(f"{90+i}m"))
        write(tty, f" fg{i+8:02d} ".encode())
        write(tty, csi("0m"))
        write(tty, b"\r\n")
        time.sleep(DELAY)

    write(tty, b"\r\n")
    for i in range(8):
        # normal bg
        write(tty, csi(f"{40+i}m"))
        write(tty, f" bg{i:02d} ".encode())
        # bright bg
        write(tty, csi(f"{100+i}m"))
        write(tty, f" bg{i+8:02d} ".encode())
        write(tty, csi("0m"))
        write(tty, b"\r\n")
        time.sleep(DELAY)

    write(tty, csi("0m"))
    pause("check 16 colours look right")

def test_colours_256(tty: serial.Serial):
    print("TEST: xterm-256 colour cube")
    cls(tty)
    # 6x6x6 colour cube indices 16-231
    for i in range(216):
        idx = 16 + i
        write(tty, csi(f"48;5;{idx}m"))
        write(tty, b"  ")
        if (i + 1) % 36 == 0:
            write(tty, csi("0m"))
            write(tty, b"\r\n")
        time.sleep(0.01)
    write(tty, csi("0m"))
    write(tty, b"\r\n")
    # greyscale ramp 232-255
    for i in range(24):
        idx = 232 + i
        write(tty, csi(f"48;5;{idx}m"))
        write(tty, b"  ")
        time.sleep(0.01)
    write(tty, csi("0m"))
    write(tty, b"\r\n")
    pause("check 256 colour cube and greyscale ramp")

def test_cursor(tty: serial.Serial):
    print("TEST: cursor movement and positioning")
    cls(tty)
    # draw a border-ish thing using cursor positioning
    write(tty, csi("1;1H")); write(tty, b"TL")   # top left
    write(tty, csi("1;38H")); write(tty, b"TR")  # top right
    write(tty, csi("12;1H")); write(tty, b"ML")  # mid left
    write(tty, csi("12;38H")); write(tty, b"MR") # mid right
    write(tty, csi("24;1H")); write(tty, b"BL")  # bottom left
    write(tty, csi("24;38H")); write(tty, b"BR") # bottom right
    # relative moves
    write(tty, csi("10;20H"))
    write(tty, b"CENTER")
    write(tty, csi("3A"))   # up 3
    write(tty, b"UP3  ")
    write(tty, csi("3B"))   # down 3
    write(tty, b"DN3  ")
    write(tty, csi("5C"))   # right 5
    write(tty, b"R5")
    write(tty, csi("5D"))   # left 5 (back)
    pause("check cursor positioning looks right")

def test_erase(tty: serial.Serial):
    print("TEST: erase sequences EL and ED")
    cls(tty)
    # fill screen with X
    for r in range(24):
        write(tty, b"X" * 40)
        write(tty, b"\r\n")
        time.sleep(0.01)
    time.sleep(0.3)
    # erase to end of line on row 5
    write(tty, csi("5;20H"))
    write(tty, csi("0K"))   # erase to end of line
    time.sleep(0.3)
    # erase to start of line on row 10
    write(tty, csi("10;20H"))
    write(tty, csi("1K"))   # erase to start
    time.sleep(0.3)
    # erase whole line on row 15
    write(tty, csi("15;1H"))
    write(tty, csi("2K"))   # erase whole line
    time.sleep(0.3)
    pause("check erases: partial line 5, partial line 10, full line 15")
    # now clear from cursor to end of screen
    write(tty, csi("12;1H"))
    write(tty, csi("0J"))
    pause("check bottom half cleared")

def test_cls(tty: serial.Serial):
    print("TEST: full clear ESC[2J")
    cls(tty)
    write(tty, b"if you can read this, clear failed\r\n")
    time.sleep(0.5)
    cls(tty)
    write(tty, b"clear worked\r\n")
    pause("check screen cleared properly")

def test_decsc(tty: serial.Serial):
    print("TEST: DECSC / DECRC save and restore cursor")
    cls(tty)
    write(tty, csi("5;10H"))
    write(tty, b"SAVED HERE")
    write(tty, esc("7"))          # DECSC save
    write(tty, csi("20;1H"))
    write(tty, b"moved away...")
    time.sleep(0.5)
    write(tty, esc("8"))          # DECRC restore
    write(tty, b" <-- restored")
    pause("check cursor restored to row 5 col 10")

# ── menu ──────────────────────────────────────────────────────

TESTS = {
    "1": ("Scroll (60 lines)",          test_scroll),
    "2": ("ANSI 16 colours",            test_colours_ansi16),
    "3": ("xterm-256 colour cube",      test_colours_256),
    "4": ("Cursor movement",            test_cursor),
    "5": ("Erase sequences (EL/ED)",    test_erase),
    "6": ("Full clear ESC[2J",          test_cls),
    "7": ("DECSC/DECRC save/restore",   test_decsc),
    "a": ("Run ALL tests",              None),
    "q": ("Quit",                       None),
}

def main():
    print(f"\nVT220 test suite — {TTY} @ {BAUD}")
    try:
        tty = serial.Serial(TTY, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: could not open {TTY}: {e}")
        sys.exit(1)

    # init line discipline
    os.system(f"stty -F {TTY} {BAUD} cs8 -cstopb -parenb raw -echo")
    time.sleep(0.1)

    while True:
        print("\n── tests ──────────────────────────")
        for k, (name, _) in TESTS.items():
            print(f"  {k}  {name}")
        choice = input("choice: ").strip().lower()

        if choice == "q":
            break
        elif choice == "a":
            for k, (name, fn) in TESTS.items():
                if fn:
                    print(f"\n{'─'*40}\n{name}\n{'─'*40}")
                    fn(tty)
        elif choice in TESTS and TESTS[choice][1]:
            TESTS[choice][1](tty)
        else:
            print("unknown choice")

    tty.close()
    print("bye")

if __name__ == "__main__":
    main()
