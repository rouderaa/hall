#!/usr/bin/env python3
"""Display the live MT6835 angle from /dev/ttyACM1 in a big pygame window.

The ESP32 sketch (mt6835_angle.ino) prints one sample per second at 115200
baud, 8N1, in the form:

    123.456 deg  [ok]

A background thread reads and parses the serial stream; the pygame window
renders the latest angle with a very large font, plus the sensor's quality
status. Values older than STALE_AFTER seconds are shown in yellow.

Usage: python3 angle_display.py [port] [baud]
Defaults: /dev/ttyACM1 115200
Env:  ANGLE_DEBUG=1  -> print the on-screen text to stdout every second
"""
import os
import re
import sys
import termios
import threading
import time

import pygame

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

STALE_AFTER = 3.0            # seconds before a value is shown as stale
WIDTH, HEIGHT = 1200, 600
MAX_FONT = 190

# "123.456 deg  [ok]"  ->  group(1)=angle, group(2)=quality (optional)
LINE_RE = re.compile(rb"(-?\d+(?:\.\d+)?)\s*deg\s*(?:\[([^\]]*)\])?")

_font_cache = {}


def get_font(size):
    f = _font_cache.get(size)
    if f is None:
        f = pygame.font.Font(None, size)
        _font_cache[size] = f
    return f


def open_serial(port, baud):
    """Open an 8N1 serial port (same termios setup as dump_debug.py)."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    baud_const = getattr(termios, "B%d" % baud)
    attrs[0] |= termios.CREAD | termios.CLOCAL
    attrs[1] |= termios.CSIZE | termios.CREAD
    attrs[2] = (attrs[2] & ~termios.CBAUD) | baud_const
    attrs[3] &= ~(termios.CSTOPB | termios.PARENB | termios.CSIZE | termios.ICRNL)
    attrs[3] |= termios.CS8
    attrs[4] = 1             # VMIN: block until at least 1 byte arrives
    attrs[5] = 0             # VTIME: no fractional timeout
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


class Latest:
    """Thread-safe holder for the newest sample."""

    def __init__(self):
        self.lock = threading.Lock()
        self.angle = None
        self.quality = None
        self.stamp = None
        self.error = None

    def update(self, **kw):
        with self.lock:
            for k, v in kw.items():
                setattr(self, k, v)

    def snapshot(self):
        with self.lock:
            return self.angle, self.quality, self.stamp, self.error


def reader_loop(latest, port, baud):
    """Read lines forever; store each angle/quality sample in `latest`."""
    while True:
        try:
            fd = open_serial(port, baud)
        except PermissionError:
            latest.update(error="permission denied - add user to 'dialout' group")
            time.sleep(1.0)
            continue
        except OSError as exc:
            latest.update(error=f"{port} unavailable: {exc}")
            time.sleep(1.0)
            continue

        latest.update(error=None)
        buf = b""
        try:
            while True:
                buf += os.read(fd, 256)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    m = LINE_RE.search(line)
                    if m:
                        latest.update(
                            angle=float(m.group(1).decode()),
                            quality=m.group(2).decode() if m.group(2) else "ok",
                            stamp=time.monotonic(),
                        )
        except OSError:
            pass  # device unplugged -> fall through and retry
        finally:
            try:
                os.close(fd)
            except OSError:
                pass
        time.sleep(1.0)


def main():
    pygame.init()
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption(f"MT6835 Angle - {PORT}")
    mid_font = get_font(56)
    small_font = get_font(36)

    latest = Latest()
    threading.Thread(
        target=reader_loop, args=(latest, PORT, BAUD), daemon=True
    ).start()

    clock = pygame.time.Clock()
    running = True
    last_debug = 0.0
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                running = False

        angle, quality, stamp, error = latest.snapshot()
        now = time.monotonic()

        if angle is None:
            title_text = error or "waiting for data..."
            title_color = (140, 140, 140)
            sub_text = ""
            sub_color = (90, 90, 90)
        else:
            title_color = (230, 200, 60) if now - stamp > STALE_AFTER else (240, 240, 240)
            title_text = f"{angle:.3f} \u00b0"
            sub_text = quality or "ok"
            ok = sub_text.strip().lower() == "ok"
            sub_color = (110, 220, 110) if ok else (230, 120, 90)

        # Very big font; shrink only if the text would overflow the window.
        size = MAX_FONT
        title_surf = get_font(size).render(title_text, True, title_color)
        while title_surf.get_width() > WIDTH - 80 and size > 40:
            size -= 10
            title_surf = get_font(size).render(title_text, True, title_color)

        screen.fill((18, 18, 22))
        screen.blit(
            title_surf, title_surf.get_rect(midbottom=(WIDTH // 2, HEIGHT // 2 + 60))
        )
        if sub_text:
            sub_surf = mid_font.render(sub_text, True, sub_color)
            screen.blit(sub_surf, sub_surf.get_rect(midtop=(WIDTH // 2, HEIGHT // 2 + 80)))

        hint = small_font.render("ESC to quit", True, (80, 80, 80))
        screen.blit(hint, hint.get_rect(bottomright=(WIDTH - 12, HEIGHT - 8)))

        if os.environ.get("ANGLE_DEBUG") and now - last_debug > 1.0:
            last_debug = now
            print(f"RENDER {title_text!r} {sub_text!r}", flush=True)

        pygame.display.flip()
        clock.tick(30)

    pygame.quit()


if __name__ == "__main__":
    main()
