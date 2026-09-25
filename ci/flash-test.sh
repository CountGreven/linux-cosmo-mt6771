#!/usr/bin/env bash
# Flash a test kernel to the sacrificial slot, reboot, and collect what the attempt left behind.
#
#   ci/flash-test.sh --image boot-test.img              # the whole cycle
#   ci/flash-test.sh --image boot-test.img --dry-run    # say what would happen
#   ci/flash-test.sh --image boot-test.img --no-reboot  # write, verify, clear pstore; Fredrik reboots
#   ci/flash-test.sh --collect                          # just read pstore from a device that came back
#
# The guards are in here rather than in a prompt, so a worker can run the cycle unattended:
#   - only the sacrificial partition may be written, by name and by number;
#   - the image must carry an ANDROID! header and fit the slot;
#   - a backup of the target must exist locally before anything is written;
#   - the write is verified by reading it back and comparing hashes;
#   - if the device does not return, the script says so and stops rather than guessing.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host="${COSMO_SSH:-cosmo}"
backups="${COSMO_BACKUPS:-/storage/kernel/cosmo-backups}"
# The one slot Fredrik agreed to sacrifice. Everything else is his working system, his recovery, or the
# bootloader; writing to those is how a phone becomes a paperweight.
target_part="${COSMO_TARGET_PART:-42}"
target_name="${COSMO_TARGET_NAME:-UBPORTS}"
# The vendor kernel (Gemian) names the eMMC /dev/block/mmcblk0*, mainline /dev/mmcblk0*; both give the
# GPT name in sysfs, so the guard reads that rather than gdisk on a path only one of them has.
dev="/dev/mmcblk0p${target_part}"
wait_secs="${COSMO_WAIT:-180}"

image=""
dry=0
collect_only=0
no_reboot=0
while [ $# -gt 0 ]; do
    case "$1" in
        --image) image="${2:-}"; shift ;;
        --dry-run) dry=1 ;;
        --collect) collect_only=1 ;;
        --no-reboot) no_reboot=1 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
ssh_ro() { timeout 120 ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"; }

collect() {
    say "=== pstore after the attempt"
    ssh_ro "sudo -n ls -la /sys/fs/pstore/" || return 1
    for f in console-ramoops dmesg-ramoops-0 pmsg-ramoops-0; do
        if ssh_ro "sudo -n test -f /sys/fs/pstore/$f"; then
            local out="$here/../build/ci/pstore-$(date +%Y%m%d-%H%M)-$f.txt"
            mkdir -p "$(dirname "$out")"
            ssh_ro "sudo -n cat /sys/fs/pstore/$f" > "$out"
            say "  $f -> $out ($(wc -l < "$out") lines)"
            # What we are actually looking for: evidence our kernel ran at all.
            grep -iE "Linux version|mt6771|simple-framebuffer|Kernel panic|Unable to handle" "$out" | head -5 | sed 's/^/    /'
        fi
    done
    say "=== which system is running now"
    ssh_ro "cat /proc/cmdline | tr ' ' '\\n' | grep -E 'bootpartition|^console' ; uname -r"
}

if [ "$collect_only" = 1 ]; then
    collect
    exit $?
fi

[ -n "$image" ] || { echo "need --image" >&2; exit 2; }
[ -f "$image" ] || { echo "no such image: $image" >&2; exit 2; }

# 1. The image must be what we think it is.
magic="$(head -c 8 "$image")"
[ "$magic" = "ANDROID!" ] || { echo "refusing: $image has no ANDROID! header (got '$magic')" >&2; exit 3; }
size=$(stat -c %s "$image")
slot_size=$(( 32 * 1024 * 1024 ))
[ "$size" -le "$slot_size" ] || { echo "refusing: image is $size bytes, slot is $slot_size" >&2; exit 3; }

# 2. The target must be the sacrificial slot, by name as well as by number.
name_on_device="$(ssh_ro "sed -n 's/^PARTNAME=//p' /sys/class/block/mmcblk0p${target_part}/uevent")"
[ "$name_on_device" = "$target_name" ] || {
    echo "refusing: partition $target_part is '$name_on_device', expected '$target_name'" >&2; exit 3; }

# 3. A backup must exist before we overwrite anything.
[ -s "$backups/$target_name.img" ] || {
    echo "refusing: no backup at $backups/$target_name.img — take one first" >&2; exit 3; }

say "image:   $image ($((size/1024/1024)) MiB)"
say "target:  $dev ($target_name), backup present"
say "restore: sudo dd if=$backups/$target_name.img of=$dev bs=1M"
[ "$dry" = 1 ] && { say "(dry run, nothing written)"; exit 0; }

# 4. Write it, then read it back and compare.
say "=== writing"
local_hash="$(sha256sum "$image" | cut -d' ' -f1)"
if ! scp -o BatchMode=yes -q "$image" "$host:/tmp/boot-test.img"; then
    echo "copy to device failed" >&2; exit 4
fi
if ! ssh_ro "sudo -n dd if=/tmp/boot-test.img of=$dev bs=1M conv=fsync 2>&1 | tail -1"; then
    echo "write failed" >&2; exit 4
fi
mib=$(( (size + 1048575) / 1048576 ))
remote_hash="$(ssh_ro "sudo -n dd if=$dev bs=1M count=$mib 2>/dev/null | head -c $size | sha256sum | cut -d' ' -f1")"
[ "$local_hash" = "$remote_hash" ] || { echo "verify failed: $local_hash != $remote_hash" >&2; exit 4; }
say "verified: $local_hash"

# 5. Clear pstore so whatever appears afterwards belongs to this attempt.
say "=== clearing old pstore records"
ssh_ro "sudo -n rm -f /sys/fs/pstore/* 2>/dev/null; sudo -n ls /sys/fs/pstore/ | wc -l" | sed 's/^/  records left: /'

# 6. Since the kernel boots (2026-09-25), a successful attempt never "comes back": the reboot and the
#    wait below only make sense for a kernel that dies. With --no-reboot the script stops here and
#    Fredrik picks the slot himself.
if [ "$no_reboot" = 1 ]; then
    say "=== written and verified; not rebooting. Pick '$target_name' in the boot menu when ready."
    exit 0
fi
# 6b. Reboot and wait. The boot menu may need a human; say so rather than pretending.
say "=== rebooting into the menu; select '$target_name' to run the test kernel"
ssh_ro "sudo -n systemctl reboot" >/dev/null 2>&1 || ssh_ro "sudo -n reboot" >/dev/null 2>&1 || true
sleep 20
for _ in $(seq 1 $((wait_secs / 10))); do
    if timeout 8 ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" true 2>/dev/null; then
        say "device is back after roughly $SECONDS seconds"
        collect
        exit 0
    fi
    sleep 10
done

say "device did not come back within ${wait_secs}s."
say "That is a result, not necessarily a failure: a test kernel that hangs looks exactly like this."
say "Next: pick '$target_name' or 'debian_kde' in the boot menu, then run: ci/flash-test.sh --collect"
exit 5
