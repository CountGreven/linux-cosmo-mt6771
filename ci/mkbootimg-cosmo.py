#!/usr/bin/env python3
"""Pack a Cosmo boot image: gzip(Image) + appended dtb, in the layout the device already uses.

    ci/mkbootimg-cosmo.py --image <Image> --dtb <dtb> --out boot-test.img [--ramdisk <file>]

Every constant here was read off the working DEBIAN_KDE image rather than chosen: header v0, page size
2048, ramdisk at 0x55000000, tags at 0x54000000, and a kernel blob that is a gzipped
Image with the device tree glued to the end (MediaTek's habit).

A ramdisk is optional. Without one the kernel will panic when it cannot mount a root filesystem — which is
fine for a first attempt, because the panic itself is the sign of life we are looking for.
"""
from __future__ import annotations

import argparse
import gzip
import struct
import sys
from pathlib import Path

PAGE = 2048
# 2 MiB aligned, and that alignment is the point rather than a detail.
#
# The vendor image loads at 0x40080000, and copying that was a mistake: its 4.4 kernel declares
# text_offset 0x80000, so its base is the aligned 0x40000000. A modern arm64 kernel declares
# text_offset 0, which makes the load address itself the base, and the boot protocol requires it to be
# 2 MiB aligned. At 0x40080000 the image sat 512 KiB past a boundary and died in early page-table setup,
# before any console, framebuffer or pstore existed — two silent boots with no evidence left behind.
# ci/check-boot-protocol.py fails the build rather than the device now.
KERNEL_ADDR = 0x40200000
RAMDISK_ADDR = 0x55000000
SECOND_ADDR = 0x40F00000
TAGS_ADDR = 0x54000000
# The stored cmdline of the working image. LK appends its own arguments (console=, androidboot.*,
# printk.disable_uart=1) at boot, so what we put here is a request, not the final word.
# panic=5 rather than panic=0, and the reason matters for the workflow: the console log lives in the
# ramoops buffer in DRAM, which survives a reset but not a power-down. With panic=0 a crashed kernel sits
# there until someone holds the power button, and a full power-off takes the log with it -- which is how
# one attempt's evidence was lost. Rebooting itself after five seconds gets us a warm reset and keeps the
# buffer, without depending on how the device was restarted.
CMDLINE = ("bootopt=64S3,32N2,64N2 log_buf_len=4M printk.disable_uart=0 "
           "console=tty0 earlycon keep_bootcon panic=5")


def pad(data: bytes, page: int = PAGE) -> bytes:
    remainder = len(data) % page
    return data + (b"\0" * (page - remainder) if remainder else b"")


def build(image: Path, dtb: Path, ramdisk: Path | None, cmdline: str) -> bytes:
    # gzip the Image, then append the dtb: this is what the vendor image contains.
    kernel = gzip.compress(image.read_bytes(), 6) + dtb.read_bytes()
    rd = ramdisk.read_bytes() if ramdisk else b""

    header = struct.pack(
        "<8sIIIIIIIIII",
        b"ANDROID!", len(kernel), KERNEL_ADDR, len(rd), RAMDISK_ADDR,
        0, SECOND_ADDR, TAGS_ADDR, PAGE, 0, 0,
    )
    header += b"\0" * 16                                  # product name
    header += cmdline.encode()[:511].ljust(512, b"\0")    # cmdline
    header += b"\0" * 32                                  # id (sha1; the bootloader does not check it)
    header += b"\0" * 1024                                # extra cmdline
    return pad(header) + pad(kernel) + (pad(rd) if rd else b"")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", type=Path, help="raw arm64 Image; gzipped here")
    ap.add_argument("--dtb", type=Path, help="device tree appended to the kernel")
    # For isolating a failure: pack a kernel blob that is already gzip+dtb (e.g. one lifted out of a
    # working image), so the packer and the slot can be tested without our kernel in the way.
    ap.add_argument("--kernel-blob", type=Path, help="pre-built gzip+dtb blob, used verbatim")
    ap.add_argument("--ramdisk", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cmdline", default=CMDLINE)
    args = ap.parse_args(argv)

    if args.kernel_blob:
        kernel = args.kernel_blob.read_bytes()
        rd = args.ramdisk.read_bytes() if args.ramdisk else b""
        header = struct.pack("<8sIIIIIIIIII", b"ANDROID!", len(kernel), KERNEL_ADDR, len(rd), RAMDISK_ADDR,
                             0, SECOND_ADDR, TAGS_ADDR, PAGE, 0, 0)
        header += b"\0" * 16 + args.cmdline.encode()[:511].ljust(512, b"\0") + b"\0" * 32 + b"\0" * 1024
        blob = pad(header) + pad(kernel) + (pad(rd) if rd else b"")
    else:
        if not (args.image and args.dtb):
            print("need --image and --dtb, or --kernel-blob", file=sys.stderr)
            return 2
        blob = build(args.image, args.dtb, args.ramdisk, args.cmdline)
    if len(blob) > 32 * 1024 * 1024:
        print(f"image is {len(blob)/1048576:.1f} MiB; the slot is 32 MiB", file=sys.stderr)
        return 1
    args.out.write_bytes(blob)
    print(f"{args.out}: {len(blob)/1048576:.1f} MiB"
          + (f", ramdisk {args.ramdisk.stat().st_size:,}" if args.ramdisk else ", no ramdisk"))
    print(f"cmdline: {args.cmdline}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
