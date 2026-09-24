#!/usr/bin/env python3
"""Fail when the kernel blob in a packed Cosmo boot image is larger than the vendor's.

    ci/check-image-size.py boot-test.img [--limit BYTES]

The blob is the gzipped Image plus the appended dtb, i.e. the header's kernel_size field. MediaTek's LK
loads it into a fixed buffer, and the working Gemian image carries 11,610,550 bytes. A bigger blob is the
leading suspect for the silent first boot (notes: projects/cosmo/03-flashing.org), so it is a failure
until the device shows otherwise; the limit is the vendor's size, not a measured LK limit.

Only the header is read: v0, magic ANDROID!, kernel_size is the first u32 after it.
"""
import argparse
import struct
import sys

VENDOR_KERNEL_BLOB = 11_610_550
# Room to spare: the point of a lean config is to land well under, not to squeak past.
MARGIN = 0.90


def kernel_size(path):
    with open(path, "rb") as f:
        head = f.read(16)
    if len(head) < 16 or head[:8] != b"ANDROID!":
        raise ValueError(f"{path}: no ANDROID! header")
    return struct.unpack_from("<I", head, 8)[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--limit", type=int, default=VENDOR_KERNEL_BLOB)
    args = ap.parse_args(argv)

    try:
        size = kernel_size(args.image)
    except (OSError, ValueError) as e:
        print(f"FAIL: {e}")
        return 1

    print(f"kernel blob {size:,} bytes; vendor's {args.limit:,} ({size / args.limit:.0%})")
    if size > args.limit:
        print(f"FAIL: kernel blob is {size - args.limit:,} bytes over the vendor's; "
              "LK may not fit it")
        return 1
    if size > args.limit * MARGIN:
        print(f"note: under the limit but within {1 - MARGIN:.0%} of it; no room to spare")
    print("check-image-size: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
