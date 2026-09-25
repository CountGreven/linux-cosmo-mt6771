#!/usr/bin/env python3
"""Translate the Cosmo's vendor LCM tables into the mainline driver's C, and check the driver carries it.

    ci/nt36672-init-table.py emit                      # print the generated C functions
    ci/nt36672-init-table.py check [driver.c]          # exit 1 unless the driver carries exactly that

The panel's init table is 300-odd DSI writes. Typed by hand, one wrong byte is a panel that stays dark or
shows the wrong gamma, and nobody would find it by reading. So the table is never typed: it is parsed out
of the vendor's own LCM driver and the driver's functions must equal what comes out of this script.

Source: ci/vendor/aeon_nt36672_fhd_dsi_vdo_x600_xinli.c, a byte-identical copy of
drivers/misc/mediatek/lcm/aeon_nt36672_fhd_dsi_vdo_x600_xinli/aeon_nt36672_fhd_dsi_vdo_x600_xinli.c in
Planet's 4.4 tree (github.com/CountGreven/cosmo-linux-kernel-4.4, a741a2e89bcc). The directory name says
x600_xinli, but its .name is "aeon_nt36672_fhd_dsi_vdo_x800_datong": the panel LK reports on this device.
It is kept here so GitHub Actions can run the check without the vendor tree; when the vendor tree is
present (COSMO_VENDOR_TREE), the copy is also compared against it.

Translation rules, all from the vendor, none invented:
- Only the live preprocessor branch is read (#if 1 / #if 0 / #else / #endif; anything else is refused).
- A row is {cmd, count, {params}}; count must equal the number of params.
- REGFLAG_DELAY rows carry milliseconds in count and become mipi_dsi_msleep().
- Packet type follows the vendor's DSI_set_cmdq_V2 (mt6771 ddp_dsi.c:2693-2758): cmd < 0xB0 is sent as
  DCS (0x05/0x15/0x39), anything else as generic (0x13/0x23/0x29). mipi_dsi_dcs_write_seq_multi and
  mipi_dsi_generic_write_seq_multi pick the same packet types by length, so the wire bytes match.
- A parameterless standard DCS command (sleep in/out, display on/off) uses the mipi_dsi helper, which
  sends the identical short packet. With a parameter it stays a raw write, because the helper would not
  send the parameter.
"""
from __future__ import annotations

import difflib
import hashlib
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VENDOR_COPY = REPO / "ci/vendor/aeon_nt36672_fhd_dsi_vdo_x600_xinli.c"
VENDOR_SHA256 = "1b5b354a0f50820cc9280d15929365a1c8e9652ce7b3389b2e619a03352cd171"
VENDOR_REL = ("drivers/misc/mediatek/lcm/aeon_nt36672_fhd_dsi_vdo_x600_xinli/"
              "aeon_nt36672_fhd_dsi_vdo_x600_xinli.c")
DRIVER = REPO / "drivers/gpu/drm/panel/panel-novatek-nt36672.c"

# the C functions the driver must carry, and the vendor table each is generated from
FUNCTIONS = {
    "planet_cosmo_init_cmds": "init_setting",
    "planet_cosmo_off_cmds": "lcm_suspend_setting",
}

DELAY = 0xFFFC          # REGFLAG_DELAY
END = 0xFFFD            # REGFLAG_END_OF_TABLE
FLAGS = {"REGFLAG_DELAY": DELAY, "REGFLAG_END_OF_TABLE": END,
         "REGFLAG_UDELAY": 0xFFFB, "REGFLAG_RESET_LOW": 0xFFFE, "REGFLAG_RESET_HIGH": 0xFFFF}

DCS_HELPERS = {
    0x10: "mipi_dsi_dcs_enter_sleep_mode_multi",
    0x11: "mipi_dsi_dcs_exit_sleep_mode_multi",
    0x28: "mipi_dsi_dcs_set_display_off_multi",
    0x29: "mipi_dsi_dcs_set_display_on_multi",
}

WIDTH = 100


class TableError(Exception):
    pass


@dataclass
class Row:
    cmd: int
    count: int
    params: list[int] = field(default_factory=list)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _live_lines(body: str) -> str:
    """Keep only the lines the preprocessor would, for literal #if 0/1 conditionals."""
    out, stack = [], []          # stack of (this branch live, parent live)
    for line in body.splitlines():
        s = line.strip()
        live = all(b for b, _ in stack)
        if s.startswith("#"):
            directive = s[1:].strip()
            m = re.fullmatch(r"if\s+([01])", directive)
            if m:
                stack.append((m.group(1) == "1", live))
            elif directive == "else":
                if not stack:
                    raise TableError("#else without #if")
                this, parent = stack.pop()
                stack.append((not this, parent))
            elif directive == "endif":
                if not stack:
                    raise TableError("#endif without #if")
                stack.pop()
            else:
                raise TableError(f"unsupported preprocessor line in table: {s}")
            continue
        if live:
            out.append(line)
    if stack:
        raise TableError("unterminated #if in table")
    return "\n".join(out)


def _num(tok: str) -> int:
    tok = tok.strip()
    if tok in FLAGS:
        return FLAGS[tok]
    try:
        return int(tok, 0)
    except ValueError:
        raise TableError(f"not a number: {tok!r}") from None


def extract_table(src: str, name: str) -> list[Row]:
    """The rows of `static struct LCM_setting_table <name>[]`, up to REGFLAG_END_OF_TABLE."""
    m = re.search(r"struct\s+LCM_setting_table\s+" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{", src)
    if not m:
        raise TableError(f"table {name} not found")
    depth, i = 1, m.end()
    while depth:
        if i >= len(src):
            raise TableError(f"table {name} is not closed")
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    body = _live_lines(_strip_comments(src[m.end():i - 1]))

    rows, ended = [], False
    for rm in re.finditer(r"\{\s*([^,{}]+?)\s*,\s*([^,{}]+?)\s*,\s*\{([^{}]*)\}\s*\}", body):
        cmd, count = _num(rm.group(1)), _num(rm.group(2))
        params = [_num(p) for p in rm.group(3).split(",") if p.strip()]
        if ended:
            raise TableError(f"{name}: row after REGFLAG_END_OF_TABLE: {rm.group(0)}")
        if cmd == END:
            ended = True
            continue
        if cmd == DELAY:
            if params:
                raise TableError(f"{name}: delay row with parameters: {rm.group(0)}")
        elif cmd in FLAGS.values():
            raise TableError(f"{name}: unsupported flag row: {rm.group(0)}")
        elif cmd > 0xFF or count != len(params) or any(p > 0xFF for p in params):
            raise TableError(f"{name}: malformed row: {rm.group(0)}")
        rows.append(Row(cmd, count, params))
    leftover = re.sub(r"\{\s*[^{}]*?\{[^{}]*\}\s*\}", "", body)
    if re.sub(r"[\s,]", "", leftover):
        raise TableError(f"{name}: text the parser did not understand: {leftover.strip()[:80]!r}")
    if not ended:
        raise TableError(f"{name}: no REGFLAG_END_OF_TABLE")
    return rows


def _call(fn: str, args: list[str]) -> list[str]:
    """`fn(args...);` wrapped to WIDTH columns, continuation aligned under the first argument."""
    head = f"\t{fn}("
    col = len(head.expandtabs(8))
    indent = "\t" * (col // 8) + " " * (col % 8)
    lines, cur = [], head
    for n, a in enumerate(args):
        piece = a + (");" if n == len(args) - 1 else ",")
        sep = "" if cur in (head, indent) else " "
        if cur not in (head, indent) and len((cur + sep + piece).expandtabs(8)) > WIDTH:
            lines.append(cur)
            cur, sep = indent, ""
        cur += sep + piece
    lines.append(cur)
    return lines


def emit_function(fname: str, rows: list[Row]) -> str:
    out = [f"static void {fname}(struct mipi_dsi_multi_context *ctx)", "{"]
    for r in rows:
        if r.cmd == DELAY:
            out.append(f"\tmipi_dsi_msleep(ctx, {r.count});")
        elif not r.params and r.cmd in DCS_HELPERS:
            out.append(f"\t{DCS_HELPERS[r.cmd]}(ctx);")
        else:
            fn = "mipi_dsi_dcs_write_seq_multi" if r.cmd < 0xB0 else "mipi_dsi_generic_write_seq_multi"
            out += _call(fn, ["ctx"] + [f"0x{b:02x}" for b in [r.cmd] + r.params])
    out.append("}")
    return "\n".join(out) + "\n"


def generate(src: str) -> dict[str, str]:
    return {f: emit_function(f, extract_table(src, t)) for f, t in FUNCTIONS.items()}


def _function_in(text: str, fname: str) -> str | None:
    m = re.search(r"^static void " + re.escape(fname) + r"\(.*?^\}\n", text, flags=re.S | re.M)
    return m.group(0) if m else None


def compare(driver_text: str, expected: dict[str, str]) -> list[str]:
    """Problems found; empty means the driver carries exactly the generated functions."""
    problems = []
    for fname, want in expected.items():
        have = _function_in(driver_text, fname)
        if have is None:
            problems.append(f"{fname}: not found in the driver")
        elif have != want:
            diff = difflib.unified_diff(want.splitlines(), have.splitlines(),
                                        "generated", "driver", lineterm="")
            problems.append(f"{fname}: differs from the vendor table\n" + "\n".join(diff))
    return problems


def _vendor_source() -> str:
    if sha256(VENDOR_COPY) != VENDOR_SHA256:
        raise TableError(f"{VENDOR_COPY} changed: sha256 {sha256(VENDOR_COPY)}, expected {VENDOR_SHA256}")
    tree = os.environ.get("COSMO_VENDOR_TREE", "/storage/kernel/cosmo-linux-kernel-4.4")
    original = Path(tree) / VENDOR_REL
    if original.exists():
        if original.read_bytes() != VENDOR_COPY.read_bytes():
            raise TableError(f"{VENDOR_COPY} is not a copy of {original}")
        print(f"vendor copy matches {original}", file=sys.stderr)
    return VENDOR_COPY.read_text()


def main(argv: list[str]) -> int:
    cmd = argv[1] if len(argv) > 1 else "check"
    try:
        funcs = generate(_vendor_source())
    except TableError as e:
        print(f"nt36672-init-table: {e}", file=sys.stderr)
        return 1
    if cmd == "emit":
        print("\n".join(funcs.values()), end="")
        return 0
    if cmd != "check":
        print(__doc__, file=sys.stderr)
        return 2
    driver = Path(argv[2]) if len(argv) > 2 else DRIVER
    if not driver.exists():
        print(f"nt36672-init-table: {driver} does not exist", file=sys.stderr)
        return 1
    problems = compare(driver.read_text(), funcs)
    for p in problems:
        print(p)
    if problems:
        return 1
    counts = {f: sum(1 for l in funcs[f].splitlines() if l.startswith("\tmipi_dsi_")) for f in funcs}
    print(f"{driver.name} carries the vendor tables exactly: "
          + ", ".join(f"{f} {n} statements" for f, n in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
