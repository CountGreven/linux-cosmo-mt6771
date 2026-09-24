#!/usr/bin/env python3
"""Hold a packed boot image to the limits that actually exist.

    ci/check-image-size.py boot-test.img

There are two real constraints, and one that we invented.

The invented one: for most of this bring-up, this script compared the compressed blob against the
vendor's 11,610,550 bytes, on the theory that LK loads the kernel into a fixed buffer of that size. That
theory was wrong. It was disproved twice over -- a 1.7 MB image behaved identically to a 15.7 MB one, and
the real cause of both silences was LK asserting on the device tree. Holding the kernel to the vendor's
size cost us a config so lean (39 symbols from allnoconfig) that it was its own source of failure.

The real ones, from MediaTek's LK source and this device's partition table:

  1. decompress_kernel(..., MEMBASE - hdr->kernel_addr) -- the DECOMPRESSED image must fit between where
     it is loaded and LK's own link address. MEMBASE is 0x56000000 here, read out of the lk partition:
     LK compares its own PC against that literal while relocating itself.
  2. the boot image must fit the partition it is written to, which is 32 MiB.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

LK_MEMBASE = 0x56000000
PARTITION_SIZE = 32 * 1024 * 1024


def main(argv: list[str]) -> int:
    path = Path(argv[1])
    img = path.read_bytes()
    if img[:8] != b"ANDROID!":
        print(f"{path}: not an Android boot image")
        return 1
    ksize, kaddr = struct.unpack_from("<II", img, 8)
    page = struct.unpack_from("<I", img, 36)[0]
    blob = img[page:page + ksize]
    raw = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(blob) if blob[:2] == b"\x1f\x8b" else blob

    headroom = LK_MEMBASE - kaddr
    print(f"packed image      {len(img):>12,} bytes   (partition {PARTITION_SIZE:,})")
    print(f"compressed blob   {ksize:>12,} bytes")
    print(f"decompressed      {len(raw):>12,} bytes   (LK headroom {headroom:,} to MEMBASE 0x{LK_MEMBASE:08x})")

    ok = True
    if len(img) > PARTITION_SIZE:
        print(f"image is {len(img) - PARTITION_SIZE:,} bytes larger than the partition")
        ok = False
    if len(raw) > headroom:
        print(f"decompressed image overruns LK's own base by {len(raw) - headroom:,} bytes; "
              "decompress_kernel would fail and LK would spin")
        ok = False
    print("image size: ok" if ok else "image size: FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
