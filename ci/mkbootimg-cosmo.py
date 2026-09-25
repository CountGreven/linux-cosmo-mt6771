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
# No earlycon and no keep_bootcon: both write to the UART that LK tells the kernel to use as a console,
# and mainline -- unlike MediaTek's patched printk -- honours that literally. An unclocked UART that never
# reports TX-ready blocks printk forever.
#
# panic=5 rather than panic=0, and the reason matters for the workflow: the console log lives in the
# ramoops buffer in DRAM, which survives a reset but not a power-down. With panic=0 a crashed kernel sits
# there until someone holds the power button, and a full power-off takes the log with it -- which is how
# one attempt's evidence was lost. Rebooting itself after five seconds gets us a warm reset and keeps the
# buffer, without depending on how the device was restarted.
CMDLINE = ("bootopt=64S3,32N2,64N2 log_buf_len=4M printk.disable_uart=1 "
           # The panel is portrait (1080x2160) in a landscape clamshell; the vendor draws at 270 degrees.
           # rotate:3 drew a narrow strip; try the other direction. Dynamic debug on the Type-C stack and
           # the USB controller: every CC state, attach and role change goes to the console/kern.log.
           "console=tty0 fbcon=rotate:1 panic=5 "
           "dyndbg=\"module tcpm +p; module tcpci +p; module tcpci_mt6370 +p; module mtu3 +p; module xhci_mtk +p; module xhci_hcd +p; module usbcore +p; file drivers/base/dd.c +p\" "
           # The Type-C connector sits under the tcpc under the MT6370 on i2c11 and links to the USB
           # controller both ways; fw_devlink reported the cycle as fixed and still left
           # 11017000.i2c "deferred probe pending: (reason unknown)", so the MT6370 never probed.
           "fw_devlink=permissive "
           # The real Debian root, mounted READ-ONLY.
           #
           # Gemian roots from /dev/mmcblk0p43, plain ext4 -- the root=/dev/dm-0 that LK puts on the
           # cmdline is an Android leftover its initrd ignores. MMC_MTK, MMC_BLOCK and EXT4_FS are all
           # built in, so this kernel can mount it without an initramfs, and our cmdline is appended
           # after LK's so this root= is the one that wins.
           #
           # ro, deliberately and until there is a reason to change it: this is Fredrik's working Debian
           # install, and an experimental kernel with half its drivers unproven has no business writing to
           # it. A read-only mount still proves the boot and still lets userspace talk to us through
           # pstore.
           # rw since 2026-09-25: the kernel reaches systemd, and a read-only root cannot keep a log.
           # Debian's rsyslog writes /var/log/kern.log and journald /var/log/journal; that is how the
           # vendor's boot logs were recovered, and it is the only channel mainline has until it has network.
           "root=/dev/mmcblk0p43 rootwait rw "
           # LK's own arguments carry init=/init (its Android initrd). The last init= wins, and the Debian
           # root has no /init, so name systemd explicitly.
           "init=/sbin/init "
           # Bring ramoops up as early as the driver allows, via its own module parameters.
           #
           # Configured through the device tree, ramoops cannot probe until
           # of_platform_default_populate_init creates its platform device at arch_initcall_sync -- and we
           # keep dying before that, which leaves the console dead exactly where we need it. Given these
           # parameters, ramoops_init registers a dummy platform device itself at postcore_initcall, four
           # levels earlier, and the console comes up with it.
           #
           # Values match the reservation in the board dts and MediaTek's own geometry, which the vendor
           # kernel reads back: 0xe0000 total at 0x54410000, a 0x40000 console zone, 0x10000 pmsg.
           "ramoops.mem_address=0x54410000 ramoops.mem_size=0xe0000 "
           # Console 64 KiB at the end of the region, no pmsg: the console zone lands at 0x544e0000,
           # which is where the vendor kernel has read our writes back from every time (11-console-zone).
           "ramoops.console_size=0x10000 ramoops.record_size=0x1000 "
           "ramoops.pmsg_size=0 ramoops.ftrace_size=0")


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
