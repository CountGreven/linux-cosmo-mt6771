#!/usr/bin/env python3
"""Exercise the MT6631 FM receiver through /dev/fm (fmradio_drv).

  fm-test.py info            chip id, powered state
  fm-test.py tune MHZ        power up on MHZ, print RSSI for 5 s, power down
  fm-test.py scan            power up, soft-mute tune over 87.5..108 MHz like the Android FM HAL,
                             print the channels the chip calls valid and the six strongest

Frequencies go to the driver in units of 100 kHz (875..1080). The headset cable is the antenna.
ioctl numbers from fmradio/inc/fm_ioctl.h: _IOWR(0xf5, n, <pointer type>), so the size field is 8
(4 for the two search ioctls, which are declared with int32_t).
"""
import fcntl
import os
import struct
import sys
import time


def iowr(n, size=8):
    return (3 << 30) | (size << 16) | (0xf5 << 8) | n


POWERUP, POWERDOWN, TUNE, GETRSSI, GETCHIPID, IS_UP, PRE_SEARCH, RESTORE_SEARCH, SOFT_MUTE_TUNE = (
    iowr(0), iowr(1), iowr(2), iowr(7), iowr(10), iowr(24), iowr(45, 4), iowr(46, 4), iowr(63))
BAND_UE, SPACE_100K = 1, 1


def call(fd, req, buf):
    b = bytearray(buf)
    fcntl.ioctl(fd, req, b)
    return bytes(b)


def rssi(fd):
    return struct.unpack("i", call(fd, GETRSSI, struct.pack("i", 0)))[0]


def powerup(fd, f):
    r = call(fd, POWERUP, struct.pack("BBBBBxH", 0, BAND_UE, SPACE_100K, 0, 0, f))
    err, freq = r[0], struct.unpack_from("H", r, 6)[0]
    print("powerup err %d freq %.1f MHz" % (err, freq / 10))
    return err


def main():
    act = sys.argv[1] if len(sys.argv) > 1 else "info"
    fd = os.open("/dev/fm", os.O_RDWR)
    try:
        chip = struct.unpack("H", call(fd, GETCHIPID, struct.pack("H", 0)))[0]
        up = struct.unpack("I", call(fd, IS_UP, struct.pack("I", 0)))[0]
        print("chip 0x%04x powered %d" % (chip, up))
        if act == "info":
            return
        f = int(round(float(sys.argv[2]) * 10)) if len(sys.argv) > 2 else 875
        if powerup(fd, f):
            return
        try:
            if act == "tune":
                for _ in range(5):
                    print("rssi %d dBm" % rssi(fd))
                    time.sleep(1)
            else:
                fcntl.ioctl(fd, PRE_SEARCH, 0)
                res = []
                for ch in range(875, 1081):
                    r = call(fd, SOFT_MUTE_TUNE, struct.pack("iHxxi", 0, ch, 0))
                    lvl, _, valid = struct.unpack("iHxxi", r)
                    res.append((lvl, ch, valid))
                fcntl.ioctl(fd, RESTORE_SEARCH, 0)
                print("valid:", " ".join("%.1f(%d)" % (c / 10, l) for l, c, v in res if v) or "none")
                print("strongest:", " ".join("%.1f(%d)" % (c / 10, l) for l, c, v in sorted(res, reverse=True)[:6]))
        finally:
            call(fd, POWERDOWN, struct.pack("i", 0))
            print("powered down")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
