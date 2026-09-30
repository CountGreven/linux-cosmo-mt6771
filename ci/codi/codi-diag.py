#!/usr/bin/env python3
"""Diagnostics for the cover display MCU (CoDi, STM32) over /dev/ttyS1.

  codi-diag.py info            versions and whatever status the MCU volunteers
  codi-diag.py listen [SECS]   enable touch reporting and print every frame (tap the cover)
  codi-diag.py display         ask for the home and battery screens, report the MCU's screen state
  codi-diag.py sleep           put the MCU in its deep stop mode (CMD 145)
  codi-diag.py raw CMD [HEX]   send one frame, print replies for 2 s

Frame: 58 21 58 21 | u32 length | u32 command | u32 session | payload, big endian.
Ids and payloads are from codi-app (codi_*_generated_functions.py) and the V1.1.1.16 firmware.
"""
import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import time

HEADER = b"\x58\x21\x58\x21"
NAMES = {
    2: "flash version", 3: "protocol version", 43: "codi status", 50: "charging",
    120: "key press", 127: "data change alert", 141: "sw version", 142: "usb status",
    143: "sleep status", 144: "right usb otg", 145: "deep sleep", 147: "touch", 148: "touch2",
}


def frame(cmd, payload=b"", session=1):
    return HEADER + struct.pack(">III", 16 + len(payload), cmd, session) + payload


def hexs(b):
    return " ".join("%02x" % x for x in b)


def decode(cmd, p):
    try:
        if cmd == 2:
            n = struct.unpack_from(">I", p)[0]
            return "version %r" % p[4:4 + n].decode("ascii", "replace")
        if cmd == 3:
            return "protocol %d.%d" % (p[0], p[1])
        if cmd == 43:
            return "mode %d screen %d data1 %d" % struct.unpack_from(">III", p)
        if cmd == 50:
            return "status %d measurement %d" % struct.unpack_from(">II", p)
        if cmd == 143:
            v = struct.unpack_from(">H", p)[0]
            return "%d (%s)" % (v, "going to sleep" if v else "awake")
        if cmd == 147:
            return "mode %d x %d y %d" % struct.unpack_from(">Bhh", p)
        if cmd == 148:
            return "press %d previous %d x %d y %d" % struct.unpack_from(">BBHH", p)
    except (struct.error, IndexError):
        pass
    return hexs(p)


class Link:
    def __init__(self, dev, force):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            if not force:
                sys.exit("%s is in use (codiServer?); stop it or pass --force" % dev)
        a = termios.tcgetattr(self.fd)
        a[0] = 0
        a[1] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = b""
        self.rx = 0

    def send(self, cmd, payload=b""):
        f = frame(cmd, payload)
        print("-> %3d %-18s %s" % (cmd, NAMES.get(cmd, ""), hexs(f)))
        os.write(self.fd, f)

    def frames(self, secs):
        end = time.time() + secs
        while True:
            left = end - time.time()
            if left <= 0:
                return
            if select.select([self.fd], [], [], min(left, 0.2))[0]:
                d = os.read(self.fd, 4096)
                self.rx += len(d)
                self.buf += d
            while True:
                i = self.buf.find(HEADER)
                if i < 0:
                    self.buf = self.buf[-3:]
                    break
                if len(self.buf) < i + 16:
                    break
                n, cmd, _ = struct.unpack_from(">III", self.buf, i + 4)
                if n < 16 or n > 65536:
                    self.buf = self.buf[i + 4:]
                    continue
                if len(self.buf) < i + n:
                    break
                yield cmd, self.buf[i + 16:i + n]
                self.buf = self.buf[i + n:]

    def show(self, secs):
        seen = []
        for cmd, p in self.frames(secs):
            seen.append(cmd)
            print("<- %3d %-18s %s" % (cmd, NAMES.get(cmd, "?"), decode(cmd, p)))
            sys.stdout.flush()
        return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["info", "listen", "display", "sleep", "raw"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--dev", default="/dev/ttyS1")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    ln = Link(a.dev, a.force)

    if a.action == "info":
        for cmd in (0, 1, 141):
            ln.send(cmd)
        seen = ln.show(3)
        if not seen:
            print("VERDICT: no answer (%d stray bytes). MCU asleep, held in reset, or the link is dead; "
                  "run codi-reset and try again" % ln.rx)
        else:
            print("VERDICT: MCU alive, %d frames" % len(seen))
    elif a.action == "listen":
        secs = float(a.args[0]) if a.args else 15
        ln.send(146, struct.pack(">BB", 1, 1))          # touch reporting on, absolute
        print("tap and swipe the cover display now (%d s)" % secs)
        seen = ln.show(secs)
        ln.send(146, struct.pack(">BB", 0, 1))
        ln.show(0.5)
        t = [c for c in seen if c in (147, 148)]
        print("VERDICT: %d touch frames, %d other frames%s" %
              (len(t), len(seen) - len(t), "" if t else " (touch not seen)"))
    elif a.action == "display":
        ln.send(93)                                      # home screen
        ln.show(2)
        ln.send(95)                                      # battery level screen
        seen = ln.show(4)
        st = [c for c in seen if c == 43]
        print("VERDICT: NEEDS EYES: the cover should show the battery level now. "
              "The MCU %s its screen state; it cannot sense the panel itself." %
              ("reported" if st else "did not report"))
    elif a.action == "sleep":
        ln.send(145)
        seen = ln.show(3)
        print("VERDICT: %s" % ("MCU announced sleep" if 143 in seen else "no sleep announcement seen"))
    else:
        if not a.args:
            sys.exit("raw needs a command id")
        ln.send(int(a.args[0], 0), bytes.fromhex("".join(a.args[1:])))
        ln.show(2)


if __name__ == "__main__":
    main()
