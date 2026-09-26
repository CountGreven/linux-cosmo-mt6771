#!/usr/bin/env python3
"""Replay what the vendor's wmt_loader does (bionic binary, cannot run on mainline):
   tell the detect driver the chip id and let it create the wmt core (wmtd thread, /dev/stpwmt)."""
import fcntl, os, struct, sys, time
# _IOW('w', 1, int) / _IOR('w', 4, int): arm64 ioctl encoding, sizeof(int)=4
def _IOC(d, t, n, s): return (d << 30) | (s << 16) | (ord(t) << 8) | n
_IOC_NONE, _IOC_WRITE, _IOC_READ = 0, 1, 2
SET_CHIP_ID   = _IOC(_IOC_WRITE, 'w', 1, 4)
DO_MODULE_INIT = _IOC(_IOC_READ, 'w', 4, 4)
MODULE_CLEANUP = _IOC(_IOC_READ, 'w', 5, 4)   # the vendor loader calls this first: it unregisters the
                                              # sdio detect driver so hif_sdio_init can register its own
GET_SOC_CHIP_ID = _IOC(_IOC_READ, 'w', 3, 4)
fd = os.open("/dev/wmtdetect", os.O_RDWR)
try:
    print("soc chip id:", hex(fcntl.ioctl(fd, GET_SOC_CHIP_ID, 0)))
except OSError as e:
    print("GET_SOC_CHIP_ID:", e)
print("set chip id 0x6771:", fcntl.ioctl(fd, SET_CHIP_ID, 0x6771))
print("module cleanup:", fcntl.ioctl(fd, MODULE_CLEANUP, 0x6771))
print("do module init:", fcntl.ioctl(fd, DO_MODULE_INIT, 0x6771))
os.close(fd)
for _ in range(20):
    if os.path.exists("/dev/stpwmt"): print("/dev/stpwmt present"); break
    time.sleep(0.25)
else:
    print("/dev/stpwmt did not appear")
