#!/usr/bin/env python3
"""Send CMD 144 (CMD_SYNC_RIGHT_USB_OTG_STATUS) to the CoDi STM32 exactly as Android does.

Frame (AmoledisonThread.processMessage case 144, msg_common, MyByteBuffer; all big endian):
  "X!X!" | u32 length (18) | u32 cmd (144) | u32 sequence (1234) | u16 status
No checksum, no terminator, no wake pulse: Android only takes a PowerManager wakelock.
Android sends 2 then 1 on a right attach, 3 then 0 on a detach.
Exits 0 without sending while codiServer holds the tty: its cosmo_usb module sends 144 then.
"""
import argparse
import errno
import fcntl
import os
import select
import struct
import sys
import termios
import time

CMD_SYNC_RIGHT_USB_OTG_STATUS = 144
# msg_common.msg_ctr starts at 1234 and MSG_NEXT() is never called, so every Android frame carries it.
ANDROID_SEQUENCE = 1234


def frame(cmd, status, seq=ANDROID_SEQUENCE):
    payload = struct.pack(">H", status)
    return b"X!X!" + struct.pack(">III", 16 + len(payload), cmd, seq) + payload


def holders(dev):
    target = os.path.realpath(dev)
    me = str(os.getpid())
    found = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or pid == me:
            continue
        try:
            fds = os.listdir("/proc/%s/fd" % pid)
        except OSError:
            continue
        for fd in fds:
            try:
                if os.readlink("/proc/%s/fd/%s" % (pid, fd)) == target:
                    with open("/proc/%s/cmdline" % pid, "rb") as f:
                        cmd = f.read().replace(b"\0", b" ").decode(errors="replace").strip()
                    found.append("%s %s" % (pid, cmd))
                    break
            except OSError:
                continue
    return found


def configure_raw(fd):
    # Same as libserial_port.so: cfmakeraw + 115200 in and out.
    attrs = termios.tcgetattr(fd)
    old = [a[:] if isinstance(a, list) else a for a in attrs]
    iflag, oflag, cflag, lflag, _, _, cc = attrs
    iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
               termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF)
    oflag &= ~termios.OPOST
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, cc])
    return old


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("status", type=int, choices=range(4), metavar="STATUS",
                    help="0 detach, 1 attach, 2 attach begin, 3 detach begin")
    ap.add_argument("--dev", default="/dev/ttyS1")
    args = ap.parse_args()

    busy = holders(args.dev)
    if any("codiServer" in b for b in busy):
        # codiServer sends 144 itself (cosmo_usb.py)
        print("%s held by codiServer, left to it" % args.dev)
        return 0
    if busy:
        print("%s is busy (stop codiServer first):" % args.dev, file=sys.stderr)
        for b in busy:
            print("  " + b, file=sys.stderr)
        return 2
    try:
        fd = os.open(args.dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as e:
        if e.errno == errno.EBUSY:
            print("%s is busy" % args.dev, file=sys.stderr)
            return 2
        raise
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            print("%s is locked by another process" % args.dev, file=sys.stderr)
            return 2
        fcntl.ioctl(fd, termios.TIOCEXCL)
        old = configure_raw(fd)
        try:
            termios.tcflush(fd, termios.TCIFLUSH)
            out = frame(CMD_SYNC_RIGHT_USB_OTG_STATUS, args.status)
            print("tx " + " ".join("%02x" % b for b in out))
            os.write(fd, out)
            termios.tcdrain(fd)

            rx = b""
            deadline = time.monotonic() + 1.0
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                r, _, _ = select.select([fd], [], [], left)
                if r:
                    chunk = os.read(fd, 4096)
                    if chunk:
                        rx += chunk
            print("rx " + (" ".join("%02x" % b for b in rx) if rx else "(nothing in 1 s)"))
        finally:
            termios.tcsetattr(fd, termios.TCSANOW, old)
            fcntl.ioctl(fd, termios.TIOCNXCL)
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
