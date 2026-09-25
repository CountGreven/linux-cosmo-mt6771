#!/usr/bin/env bash
# Try a freshly built kernel on the Cosmo by kexec from the running mainline kernel, without LK's menu.
#
#   ci/kexec-test.sh                 # copy Image+dtb from the CI build dir, load, jump
#   ci/kexec-test.sh --load-only     # copy and load; jump by hand with: sudo kexec -e
#   ci/kexec-test.sh --built-dtb     # hand over our built dtb instead of the running tree (see below)
#   ci/kexec-test.sh --set fbcon=rotate:3   # rewrite one key=value in the running cmdline (repeatable)
#   ci/kexec-test.sh --dry-run
#
# What survives the hop: DRAM, so the outgoing kernel's ramoops console is readable in the new one.
# What does not: if the new kernel hangs, hold the button; LK autoboots the slot image (the last one
# written with ci/flash-test.sh --no-reboot), which is therefore always a known-good kernel.
#
# The running kernel must have CONFIG_KEXEC (dbafc2496c63 and later).
#
# The device tree handed over is the RUNNING one (/sys/firmware/fdt), not the built dtb, and that is not
# a convenience: our dts leaves the firmware-owned regions (atf 0x54600000, tee, SPM, SSPM, the LK
# framebuffer, ccci) to LK, which injects them at boot and panics on duplicates. A kexec'd kernel that
# gets our built dtb sees ATF's DRAM as free memory; on 2026-09-25 that kernel came up on one CPU with
# all seven secondaries failing PSCI CPU_ON, and could not kexec again ("CPUs are stuck in the kernel").
# --built-dtb exists for the day the dts itself is under test, and then the firmware regions have to be
# merged in first (TODO in 19-kexec.org).
set -euo pipefail

ci=/storage/kernel/build/ci/build-image
host="${COSMO_SSH:-cosmo-eth}"
image="$ci/arch/arm64/boot/Image"
dtb="$ci/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb"
dry=0 load_only=0 live_dtb=1
sets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --load-only) load_only=1 ;;
        --live-dtb) live_dtb=1 ;;
        --built-dtb) live_dtb=0; echo "WARNING: the built dtb lacks the firmware regions LK injects; expect a broken kernel" >&2 ;;
        --image) image="$2"; shift ;;
        --dtb) dtb="$2"; shift ;;
        --set) sets+=("$2"); shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"; }

running="$(ssh_ 'uname -r; grep -c . /sys/kernel/kexec_loaded 2>/dev/null || echo nokexec')"
echo "running: $(echo "$running" | head -1)"
case "$running" in *nokexec*) echo "the running kernel has no kexec support" >&2; exit 1 ;; esac
case "$running" in 4.4.*) echo "refusing: that is the vendor kernel, boot mainline from the slot first" >&2; exit 1 ;; esac

echo "image:   $image ($(stat -c %s "$image") bytes, $(sha256sum "$image" | cut -c1-12))"
echo "dtb:     $([ $live_dtb = 1 ] && echo '/sys/firmware/fdt (live)' || echo "$dtb")"
[ $dry = 1 ] && { echo "dry run; nothing copied"; exit 0; }

ssh_ 'mkdir -p ~/kexec'
scp -q "$image" "$host:~/kexec/Image"
[ $live_dtb = 1 ] || scp -q "$dtb" "$host:~/kexec/cosmo.dtb"
ssh_ "sha256sum ~/kexec/Image | cut -c1-12"

if [ $live_dtb = 1 ]; then
    ssh_ 'sudo -n cp /sys/firmware/fdt ~/kexec/cosmo.dtb && sudo -n chown $(id -u) ~/kexec/cosmo.dtb'
fi
if [ ${#sets[@]} = 0 ]; then
    ssh_ 'sudo -n kexec -l ~/kexec/Image --dtb=$HOME/kexec/cosmo.dtb --reuse-cmdline && cat /sys/kernel/kexec_loaded'
else
    # Start from the running cmdline (LK's arguments included) and replace each key's value; a key that is
    # not there yet is appended. The result is printed so the log says what was actually booted.
    cmdline="$(ssh_ 'cat /proc/cmdline')"
    for kv in "${sets[@]}"; do
        key="${kv%%=*}"
        if printf '%s' "$cmdline" | grep -qE "(^| )$key="; then
            cmdline="$(printf '%s' "$cmdline" | sed -E "s|(^\| )$key=[^ ]*|\1$kv|")"
        else
            cmdline="$cmdline $kv"
        fi
    done
    echo "cmdline: $cmdline"
    ssh_ "sudo -n kexec -l ~/kexec/Image --dtb=\$HOME/kexec/cosmo.dtb --command-line '$cmdline' && cat /sys/kernel/kexec_loaded"
fi
echo "=== loaded"
[ $load_only = 1 ] && { echo "not jumping; run: ssh $host sudo kexec -e"; exit 0; }
echo "=== jumping (the ssh session will drop)"
ssh_ 'sudo -n kexec -e' || true
