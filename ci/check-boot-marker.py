#!/usr/bin/env python3
"""Check the early boot marker against the memory it writes to.

    ci/check-boot-marker.py

The marker is a handful of instructions at the top of primary_entry that write a ramoops-formatted
record across the pstore reservation, before the MMU, any console or any driver exists. After a failed
boot and a power cycle, Gemian's pstore exposes it: marker present means our kernel ran, marker absent
means LK never jumped. That one bit is the difference between debugging the kernel and debugging the
boot image, and nothing else we have can tell them apart.

Because it writes to physical memory with the MMU off, getting the address wrong corrupts whatever
lives there instead. So the constants in head.S are checked against the reservation in our own dtsi,
and the signature against the vendor kernel that has to read it back.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEAD_S = REPO / "arch/arm64/kernel/head.S"
DTSI = REPO / "arch/arm64/boot/dts/mediatek/mt6771.dtsi"
# fs/pstore/ram_core.c:46 in the vendor kernel: #define PERSISTENT_RAM_SIG (0x43474244) /* DBGC */
PERSISTENT_RAM_SIG = 0x43474244


def movz_movk(src: str, reg: str) -> int | None:
    """Reconstruct the constant a movz/movk pair builds into `reg`."""
    val, seen = 0, False
    for m in re.finditer(rf"\b(movz|movk)\s+[wx]{reg},\s*#(0x[0-9a-fA-F]+)(?:,\s*lsl\s*#(\d+))?", src):
        seen = True
        val |= int(m.group(2), 16) << int(m.group(3) or 0)
    return val if seen else None


def main() -> int:
    if not HEAD_S.exists():
        print(f"missing {HEAD_S}")
        return 1
    src = HEAD_S.read_text()
    if "cosmo boot marker" not in src.lower():
        print("head.S carries no boot marker")
        return 1

    block = src[src.lower().index("cosmo boot marker"):][:1600]
    start, end, sig = movz_movk(block, "5"), movz_movk(block, "6"), movz_movk(block, "7")
    if None in (start, end, sig):
        print(f"could not read the marker constants: start={start} end={end} sig={sig}")
        return 1

    # The reservation the marker writes into, from our own device tree.
    dtsi = DTSI.read_text()
    m = re.search(r"ramoops@[0-9a-f]+\s*\{[^}]*?reg\s*=\s*<\s*0\s+(0x[0-9a-f]+)\s+0\s+(0x[0-9a-f]+)", dtsi, re.S)
    if not m:
        print("no ramoops reg found in the dtsi")
        return 1
    base, size = int(m.group(1), 16), int(m.group(2), 16)

    print(f"marker writes 0x{start:08x}..0x{end:08x}, reservation 0x{base:08x}..0x{base + size:08x}")
    ok = True
    if sig != PERSISTENT_RAM_SIG:
        print(f"signature 0x{sig:08x} != PERSISTENT_RAM_SIG 0x{PERSISTENT_RAM_SIG:08x}; pstore will not find it")
        ok = False
    if start < base:
        print(f"marker starts 0x{base - start:x} bytes before the reservation")
        ok = False
    if end > base + size:
        print(f"marker runs 0x{end - (base + size):x} bytes past the reservation — it would corrupt memory we do not own")
        ok = False
    if start >= end:
        print("marker range is empty")
        ok = False
    print("boot marker: ok" if ok else "boot marker: FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
