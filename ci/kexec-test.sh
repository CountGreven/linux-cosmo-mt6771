#!/usr/bin/env bash
# Try a freshly built kernel on the Cosmo by kexec from the running mainline kernel, without LK's menu.
#
#   ci/kexec-test.sh                 # copy Image+dtb from the CI build dir, load, jump
#   ci/kexec-test.sh --load-only     # copy and load; jump by hand with: sudo kexec -e
#   ci/kexec-test.sh --live-dtb      # hand over the running (LK-modified) tree instead of our dtb
#   ci/kexec-test.sh --set fbcon=rotate:3   # rewrite one key=value in the running cmdline (repeatable)
#   ci/kexec-test.sh --dry-run
#
# What survives the hop: DRAM, so the outgoing kernel's ramoops console is readable in the new one.
# What does not: if the new kernel hangs, hold the button; LK autoboots the slot image (the last one
# written with ci/flash-test.sh --no-reboot), which is therefore always a known-good kernel.
#
# The running kernel must have CONFIG_KEXEC (dbafc2496c63 and later). Use --live-dtb only when the dts
# is unchanged: the built dtb is the one under test, but LK's additions to /chosen are not in it.
set -euo pipefail

ci=/storage/kernel/build/ci/build-image
host="${COSMO_SSH:-cosmo-eth}"
image="$ci/arch/arm64/boot/Image"
dtb="$ci/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb"
dry=0 load_only=0 live_dtb=0
sets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --load-only) load_only=1 ;;
        --live-dtb) live_dtb=1 ;;
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
    ssh_ 'cp /sys/firmware/fdt ~/kexec/cosmo.dtb'
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
