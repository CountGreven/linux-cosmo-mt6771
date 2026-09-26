#!/usr/bin/env python3
"""Replay what the vendor's wmt_launcher does over /dev/stpwmt: name the cfg file, list the ROM
patches, set the STP transport (BTIF on this SoC). Then `echo 1 > /dev/wmtWifi` turns wifi on."""
import fcntl, os, struct, sys, time
def _IOC(d, t, n, s): return (d << 30) | (s << 16) | (t << 8) | n
W, R = 1, 2
M = 0xa0
SET_STP_MODE   = _IOC(W, M, 5, 4)
SET_PATCH_NUM  = _IOC(W, M, 14, 4)
SET_PATCH_INFO = _IOC(W, M, 15, 8)
WMT_CFG_NAME   = _IOC(W | R, M, 21, 8)
QUERY_CHIPID   = _IOC(R, M, 22, 4)
GET_CHIP_INFO  = _IOC(R, M, 12, 4)
fd = os.open("/dev/stpwmt", os.O_RDWR)
def ioctl(name, cmd, arg):
    try:
        r = fcntl.ioctl(fd, cmd, arg); print(f"{name}: {r}"); return r
    except OSError as e:
        print(f"{name}: {e}"); return None
cfg = bytearray(b"WMT_SOC.cfg".ljust(256, b"\0"))
ioctl("WMT_CFG_NAME", WMT_CFG_NAME, cfg)
patches = ["ROMv4_be_patch_1_0_hdr.bin", "ROMv4_be_patch_1_1_hdr.bin"]
ioctl("SET_PATCH_NUM", SET_PATCH_NUM, len(patches))
for name in patches:
    # The vendor launcher (disassembled /vendor/bin/wmt_launcher, 0x2320-0x2490): lseek to 22, read the
    # 2-byte fw version, read 4 "patch info" bytes at offset 24. downloadSeq = info[0] & 0xf, and
    # addRess = info with byte 0 zeroed. So ROMv4_be_patch_1_1 (0x21) is downloaded first, 1_0 (0x22)
    # second; sending them in file order with the seq nibble left in the address made the ROM take an
    # alignment exception at 0x9802A right after the last fragment.
    with open(f"/vendor/firmware/{name}", "rb") as f:
        hdr = f.read(28)
    fw_ver = hdr[23] | (hdr[22] << 8)
    i = hdr[24] & 0xF
    addr = bytes([0]) + hdr[25:28]
    print(f"{name}: fw ver in patch 0x{fw_ver:04x}, seq {i}, addRess {addr.hex()}")
    info = bytearray(struct.pack("<I4s256s", i, addr, name.encode()))
    ioctl(f"SET_PATCH_INFO[{i}] {name}", SET_PATCH_INFO, info)
ioctl("SET_STP_MODE btif+fm_comm", SET_STP_MODE, 0x03 | (2 << 4))  # STP_BTIF_FULL | WMT_FM_COMM<<4
ioctl("QUERY_CHIPID", QUERY_CHIPID, 0)
os.close(fd)
print("launcher replay done")
