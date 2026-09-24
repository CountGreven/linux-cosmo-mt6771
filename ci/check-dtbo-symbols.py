#!/usr/bin/env python3
"""Check our dtb satisfies the symbols the device's DTBO overlay demands.

    ci/check-dtbo-symbols.py [built.dtb] [dtbo-partition.img]

MediaTek's LK applies an overlay from the device's dtbo partition to whatever device tree it finds in the
boot image, and it is not optional. When the overlay cannot resolve, LK does not fall back — it asserts:

    ERROR: ufdt_overlay_do_fixups():No node __symbols__ in main dtb.
    ERROR: ufdt_overlay_apply():failed to perform fixups in overlay
    ufdt_apply_overlay() failed!
    panic (caller 0x5602c2d3): ASSERT at (app/mt_boot/fdt_op.c:212): 0

read out of expdb after a failed boot. The kernel never runs, nothing is logged, and the device simply
goes dark — which is what three attempts looked like.

So the overlay's requirements are a hard constraint on our device tree, and this turns them into a build
failure. It reads __fixups__ out of the real dtbo partition and __symbols__ out of our built dtb, and
reports every symbol the overlay needs that we do not provide.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_DTB = Path("/storage/kernel/build/ci/build/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb")
DEFAULT_DTBO = Path("/storage/kernel/cosmo-backups/dtbo.img")
FDT_MAGIC = 0xD00DFEED


def fdt_nodes(fdt: bytes):
    """Yield (path, {property: value}) for every node, walking the flattened tree."""
    magic, _totalsize, off_struct, off_strings = struct.unpack_from(">IIII", fdt, 0)
    if magic != FDT_MAGIC:
        raise ValueError("not an FDT")
    size_str = struct.unpack_from(">I", fdt, 32)[0]
    strings = fdt[off_strings:off_strings + size_str]
    pos, stack, props = off_struct, [], {}
    path = "/"
    while True:
        tok = struct.unpack_from(">I", fdt, pos)[0]
        pos += 4
        if tok == 1:                                    # FDT_BEGIN_NODE
            end = fdt.index(b"\0", pos)
            name = fdt[pos:end].decode()
            pos = (end + 4) & ~3
            stack.append((path, props))
            path = (path.rstrip("/") + "/" + name) if name else path
            props = {}
        elif tok == 2:                                  # FDT_END_NODE
            yield path, props
            path, props = stack.pop()
        elif tok == 3:                                  # FDT_PROP
            length, nameoff = struct.unpack_from(">II", fdt, pos)
            pos += 8
            end = strings.index(b"\0", nameoff)
            props[strings[nameoff:end].decode()] = fdt[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif tok == 4:                                  # FDT_NOP
            continue
        elif tok == 9:                                  # FDT_END
            return


def symbols_of(fdt: bytes, node: str) -> set[str]:
    for path, props in fdt_nodes(fdt):
        if path.endswith(node):
            return set(props)
    return set()


def overlays(dtbo: bytes):
    """Every FDT in an Android DTBO table (or the file itself if it is a bare FDT)."""
    if struct.unpack_from(">I", dtbo, 0)[0] == FDT_MAGIC:
        return [dtbo]
        # a bare overlay
    magic, _total, hdr_sz, e_sz, e_cnt, e_off = struct.unpack_from(">IIIIII", dtbo, 0)
    if magic != 0xD7B7AB1E:
        raise ValueError(f"not a DTBO table (magic 0x{magic:08x})")
    out, seen = [], set()
    for i in range(e_cnt):
        sz, off = struct.unpack_from(">II", dtbo, e_off + i * e_sz)[:2]
        if (off, sz) not in seen:
            seen.add((off, sz))
            out.append(dtbo[off:off + sz])
    return out


def main(argv: list[str]) -> int:
    dtb_path = Path(argv[1]) if len(argv) > 1 else DEFAULT_DTB
    dtbo_path = Path(argv[2]) if len(argv) > 2 else DEFAULT_DTBO
    if not dtb_path.exists():
        print(f"no built dtb at {dtb_path} — run the dtbs job first")
        return 1
    if not dtbo_path.exists():
        print(f"no dtbo image at {dtbo_path}; cannot check what the overlay needs (skipping)")
        return 0

    dtb = dtb_path.read_bytes()
    have = symbols_of(dtb, "__symbols__")
    if not have:
        print(f"{dtb_path.name} has no __symbols__ node. LK's overlay will fail to resolve and LK will "
              f"ASSERT before the kernel runs. Build this dtb with dtc -@.")
        return 1

    need = set()
    for ov in overlays(dtbo_path.read_bytes()):
        need |= symbols_of(ov, "__fixups__")
    missing = sorted(need - have)
    print(f"overlay needs {len(need)} symbols; our dtb provides {len(have)}; missing {len(missing)}")
    if missing:
        for s in missing:
            print(f"  missing: {s}")
        print("LK asserts on the first symbol it cannot resolve, so every one of these is fatal.")
        return 1
    print("dtbo symbols: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
