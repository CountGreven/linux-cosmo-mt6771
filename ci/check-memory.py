#!/usr/bin/env python3
"""Sanity-check the memory map in the built Cosmo device tree.

    ci/check-memory.py [path/to/mt6771-planet-cosmo.dtb]

Asserts, on the compiled blob and not on the source:
  - a /memory node exists, with a base and a size;
  - every /reserved-memory child lies inside that memory range;
  - no two fixed reserved regions overlap;
  - a ramoops (pstore) region exists and is not no-map: a no-map pstore
    cannot be read back, which is the whole reason it is there.

A /memory size of zero is a failure: the kernel skips a zero-size entry, so
it would boot with no RAM unless the bootloader patches the node, and with no
size the upper bound of the reserved regions cannot be checked either.

Only dtc is needed (dtc -I dtb -O dts); no python libraries.
"""
import re
import subprocess
import sys

DEFAULT_DTB = "/storage/kernel/build/ci/build/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb"

failures = []
notes = []


def fail(msg):
    failures.append(msg)


def parse_dts(text):
    """Parse dtc's dts output into {'props': {}, 'kids': {name: node}}."""
    tokens = re.findall(r'"(?:[^"\\]|\\.)*"|<[^>]*>|[{};=]|[^\s{};=<>"]+', text)
    pos = 0

    def node():
        nonlocal pos
        n = {"props": {}, "kids": {}}
        while pos < len(tokens):
            t = tokens[pos]
            if t == "}":
                pos += 1
                assert tokens[pos] == ";"
                pos += 1
                return n
            if tokens[pos + 1] == "{":
                name = t
                pos += 2
                n["kids"][name] = node()
            elif tokens[pos + 1] == ";":
                n["props"][t] = True
                pos += 2
            else:
                assert tokens[pos + 1] == "=", tokens[pos:pos + 3]
                name = t
                pos += 2
                val = []
                while tokens[pos] != ";":
                    val.append(tokens[pos])
                    pos += 1
                pos += 1
                n["props"][name] = val
        return n

    # skip "/dts-v1/;" and any "/memreserve/" lines
    while tokens[pos] != "/":
        pos += 1
    pos += 2  # "/" "{"
    return node()


def cells(node, name):
    val = node["props"].get(name)
    if val is None or val is True:
        return None
    out = []
    for tok in val:
        if tok.startswith("<"):
            out += [int(c, 0) for c in tok[1:-1].split()]
    return out


def string(node, name):
    val = node["props"].get(name)
    if not val or val is True:
        return None
    return val[0].strip('"')


def pairs(flat, ac, sc):
    step = ac + sc
    if flat is None or len(flat) % step:
        return None
    out = []
    for i in range(0, len(flat), step):
        base = size = 0
        for c in flat[i:i + ac]:
            base = (base << 32) | c
        for c in flat[i + ac:i + step]:
            size = (size << 32) | c
        out.append((base, size))
    return out


def main():
    dtb = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DTB
    try:
        dts = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", dtb],
                             check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"check-memory: cannot read {dtb}: {getattr(e, 'stderr', e)}")
        return 2
    root = parse_dts(dts)
    ac = (cells(root, "#address-cells") or [2])[0]
    sc = (cells(root, "#size-cells") or [2])[0]

    # --- /memory ----------------------------------------------------------
    ranges = []
    for name, n in root["kids"].items():
        if string(n, "device_type") == "memory":
            p = pairs(cells(n, "reg"), ac, sc)
            if not p:
                fail(f"/{name}: no usable reg (need base and size)")
                continue
            ranges += p
    if not ranges:
        fail("no /memory node with a base and size")
    for b, s in ranges:
        if s == 0:
            fail(f"/memory at 0x{b:x} has size 0: the kernel ignores it, "
                 "and the upper bound of the reserved regions cannot be checked")
    ranges = [(b, s) for b, s in ranges if s]

    def inside(start, end):
        return any(start >= b and end <= b + s for b, s in ranges)

    # --- /reserved-memory -------------------------------------------------
    rm = root["kids"].get("reserved-memory")
    fixed = []  # (name, start, end)
    ramoops = []
    if rm is None:
        fail("no /reserved-memory node")
        rm = {"props": {}, "kids": {}}
    rac = (cells(rm, "#address-cells") or [ac])[0]
    rsc = (cells(rm, "#size-cells") or [sc])[0]
    for name, n in rm["kids"].items():
        reg = pairs(cells(n, "reg"), rac, rsc)
        if reg:
            for base, size in reg:
                if size == 0:
                    fail(f"{name}: zero-sized reg")
                    continue
                fixed.append((name, base, base + size))
                if not inside(base, base + size):
                    fail(f"{name}: 0x{base:x}-0x{base + size - 1:x} lies outside /memory")
        else:
            # dynamic pool: size (+ alignment, alloc-ranges) resolved by the kernel
            size = (pairs(cells(n, "size"), 0, rsc) or [(0, 0)])[0][1]
            if not size:
                fail(f"{name}: neither reg nor size")
                continue
            align = (pairs(cells(n, "alignment"), 0, rsc) or [(0, 0)])[0][1]
            if align & (align - 1):
                fail(f"{name}: alignment 0x{align:x} is not a power of two")
            windows = pairs(cells(n, "alloc-ranges"), rac, rsc)
            if not windows:
                notes.append(f"{name}: dynamic pool without alloc-ranges, not placeable statically")
            for wb, ws in windows or []:
                if ws < size:
                    fail(f"{name}: alloc-ranges 0x{wb:x}+0x{ws:x} smaller than size 0x{size:x}")
                elif not inside(wb, wb + ws):
                    fail(f"{name}: alloc-ranges 0x{wb:x}-0x{wb + ws - 1:x} not inside /memory")
        if string(n, "compatible") == "ramoops":
            ramoops.append((name, n, reg))

    fixed.sort(key=lambda r: r[1])
    for (na, sa, ea), (nb, sb, eb) in zip(fixed, fixed[1:]):
        if sb < ea:
            fail(f"{na} (0x{sa:x}-0x{ea - 1:x}) overlaps {nb} (0x{sb:x}-0x{eb - 1:x})")

    # --- ramoops ----------------------------------------------------------
    if not ramoops:
        fail("no ramoops region: pstore cannot read the boot log back")
    for name, n, reg in ramoops:
        if "no-map" in n["props"]:
            fail(f"{name}: ramoops is no-map, so pstore cannot map it to read it back")
        if not reg:
            fail(f"{name}: ramoops has no reg")
            continue
        sizes = {k: (cells(n, k) or [0])[0]
                 for k in ("record-size", "console-size", "ftrace-size", "pmsg-size")}
        if not any(sizes.values()):
            fail(f"{name}: ramoops has no record/console/ftrace/pmsg size")
        if sum(v for k, v in sizes.items() if k != "record-size") > reg[0][1]:
            fail(f"{name}: console+ftrace+pmsg exceed the region")

    for note in notes:
        print(f"note: {note}")
    for name, s, e in fixed:
        print(f"reserved: {name:<32} 0x{s:08x}-0x{e - 1:08x}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        print(f"check-memory: {len(failures)} failure(s) in {dtb}")
        return 1
    print(f"check-memory: ok ({dtb})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
