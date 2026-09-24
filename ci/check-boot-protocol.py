#!/usr/bin/env python3
"""Hold a packed boot image to the arm64 boot protocol.

    ci/check-boot-protocol.py boot-test.img

The protocol (Documentation/arch/arm64/booting.rst) says the Image must be placed at
`2 MiB-aligned base + text_offset`. Modern kernels set text_offset to 0, so the load
address itself has to be 2 MiB aligned.

The vendor's 4.4 kernel hides this: it declares text_offset 0x80000 and is loaded at
0x40080000, whose base is the aligned 0x40000000. Reusing that same load address for a
kernel with text_offset 0 puts the image 512 KiB past a 2 MiB boundary, and the early
page-table setup fails before any console, framebuffer or pstore exists. The device
simply goes quiet, which is what two attempts looked like before this check existed.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

ARM64_MAGIC = b"ARM\x64"
ALIGN = 2 * 1024 * 1024


def kernel_blob(img: bytes) -> bytes:
    if img[:8] != b"ANDROID!":
        raise ValueError("not an Android boot image")
    size, page = struct.unpack_from("<I", img, 8)[0], struct.unpack_from("<I", img, 36)[0]
    return img[page:page + size]


def check(path: Path) -> int:
    img = path.read_bytes()
    blob = kernel_blob(img)
    load = struct.unpack_from("<I", img, 12)[0]
    raw = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(blob) if blob[:2] == b"\x1f\x8b" else blob
    if raw[56:60] != ARM64_MAGIC:
        print(f"{path}: no arm64 Image magic at offset 56 — is this an arm64 kernel?")
        return 1
    text_offset, _image_size, flags = struct.unpack_from("<QQQ", raw, 8)
    base = load - text_offset
    print(f"load 0x{load:08x}  text_offset 0x{text_offset:x}  -> base 0x{base:08x}  flags 0x{flags:x}")
    if base % ALIGN:
        print(f"{path}: base 0x{base:08x} is not 2 MiB aligned "
              f"(0x{base % ALIGN:x} past the boundary). The kernel will die before it can say so.")
        return 1
    print("boot protocol: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(check(Path(sys.argv[1])))
