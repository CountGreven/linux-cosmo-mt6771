#!/usr/bin/env bash
# The same checks as .github/workflows/cosmo.yml, run on this machine.
#
# GitHub Actions is the intended home; this exists so the work is gated while that account is locked,
# and afterwards as the fast local pass before pushing.
#
#   ci/local-ci.sh              # dtbs + memory + checkpatch  (fast: the usual gate)
#   ci/local-ci.sh --full       # also build Image
#   ci/local-ci.sh --job dtbs   # one job
#
# Exit code is the verdict, so a kanban task can use this as its `test:` command.
set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${COSMO_CI_OUT:-/storage/kernel/build/ci}"
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_USER=ci KBUILD_BUILD_HOST=mainframe02
jobs="${COSMO_CI_JOBS:-$(nproc)}"
mkdir -p "$out"

want_full=0
only=""
while [ $# -gt 0 ]; do
    case "$1" in
        --full) want_full=1 ;;
        --job) only="${2:-}"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

failed=0
step() {  # step <name> <logfile>; body on stdin
    local name="$1" log="$2"
    [ -n "$only" ] && [ "$only" != "$name" ] && return 0
    printf '\n=== %s ===\n' "$name"
    if bash -s > "$log" 2>&1; then
        printf '%s: ok (log: %s)\n' "$name" "$log"
    else
        printf '%s: FAILED (log: %s)\n' "$name" "$log"
        tail -30 "$log"
        failed=1
    fi
}

# ccache keeps the rebuild honest but quick; the kernel is large and this runs on every task.
if command -v ccache >/dev/null; then
    export CROSS_COMPILE="ccache aarch64-linux-gnu-"
fi

step dtbs "$out/dtbs.log" <<EOF
set -e
cd "$repo"
make O="$out/build" defconfig >/dev/null
# keep dtc's own chatter so it can be judged; the build's stderr is the evidence
make O="$out/build" -j$jobs W=1 dtbs 2> >(tee "$out/dtc.stderr" >&2)
# dtc must be silent about OUR files; upstream's warnings are upstream's business
if grep -E "mediatek/(mt6771|.*cosmo)[^ ]*\.(dts|dtsi)" "$out/dtc.stderr"; then
    echo "dtc complained about our device trees"
    exit 1
fi
echo "dtc had nothing to say about our device trees"
EOF

# Reads the dtb the dtbs job built, so `--job memory` alone needs a prior `--job dtbs`.
step memory "$out/memory.log" <<EOF
set -e
cd "$repo"
python3 ci/check-memory.py "$out/build/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb"
EOF

step checkpatch "$out/checkpatch.log" <<EOF
set -e
cd "$repo"
base="\$(git merge-base HEAD origin/master 2>/dev/null || git rev-parse HEAD~1)"
echo "commits under review:"
git log --oneline "\$base"..HEAD
# Only the kernel changes are judged by kernel standards: cosmo-notes/ and .github/ are ours and
# never go upstream, so they must not fail a check that exists to keep patches submittable.
patches="$out/patches"
rm -rf "\$patches"; mkdir -p "\$patches"
git format-patch -o "\$patches" "\$base"..HEAD -- \
    arch drivers include Documentation sound net fs kernel mm lib scripts >/dev/null
if [ -z "\$(ls -A "\$patches" 2>/dev/null)" ]; then
    echo "no kernel changes in this range — nothing for checkpatch to judge"
    exit 0
fi
./scripts/checkpatch.pl --terse --no-signoff "\$patches"/*.patch
EOF

if [ "$want_full" = 1 ] || [ "$only" = "build" ]; then
    step build "$out/build.log" <<EOF
set -e
cd "$repo"
make O="$out/build" defconfig
make O="$out/build" -j$jobs Image
ls -l "$out/build/arch/arm64/boot/Image"
EOF
fi

# Builds and packs the bring-up kernel, then holds the blob to the vendor's size (LK loads it into a fixed
# buffer). Slow, so it is not in the default pass: run `--job image-size` or --full.
#   COSMO_IMAGE_CONFIG=defconfig  measures arm64 defconfig + ci/cosmo.config instead (the first attempt)
if [ "$want_full" = 1 ] || [ "$only" = "image-size" ]; then
    step image-size "$out/image-size.log" <<EOF
set -e
cd "$repo"
b="$out/build-image"
mkdir -p "\$b"
if [ "${COSMO_IMAGE_CONFIG:-cosmo_defconfig}" = defconfig ]; then
    make O="\$b" defconfig >/dev/null
    ./scripts/kconfig/merge_config.sh -m -O "\$b" "\$b/.config" ci/cosmo.config >/dev/null
    make O="\$b" olddefconfig >/dev/null
else
    # allnoconfig, so what ci/cosmo_defconfig does not name is off
    make O="\$b" allnoconfig KCONFIG_ALLCONFIG="$repo/ci/cosmo_defconfig" >/dev/null
    # a symbol dropped for an unmet dependency would not fail the build, only the boot
    missing=0
    for s in \$(grep -oE '^CONFIG_[A-Z0-9_a-z]+=[ym0-9]+' ci/cosmo_defconfig); do
        grep -qx "\$s" "\$b/.config" || { echo "dropped by Kconfig: \$s"; missing=1; }
    done
    [ "\$missing" = 0 ]
fi
make O="\$b" -j$jobs Image mediatek/mt6771-planet-cosmo.dtb
python3 ci/mkbootimg-cosmo.py --image "\$b/arch/arm64/boot/Image" \
    --dtb "\$b/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb" --out "$out/boot-test.img"
python3 ci/check-image-size.py "$out/boot-test.img"
EOF
fi

step commits "$out/commits.log" <<EOF
set -e
cd "$repo"
base="\$(git merge-base HEAD origin/master 2>/dev/null || git rev-parse HEAD~1)"
# A commit is either kernel or ours, never both: cosmo-notes/ and .github/ must never ride along in a
# patch sent upstream, and a mixed commit makes cherry-picking for submission a manual edit.
bad=0
for c in \$(git rev-list "\$base"..HEAD); do
    files="\$(git show --name-only --format= "\$c")"
    kernel=0; ours=0
    echo "\$files" | grep -qE '^(arch|drivers|include|Documentation|sound|net|fs|kernel|mm|lib|scripts)/' && kernel=1
    echo "\$files" | grep -qE '^(ci|\.github)/' && ours=1
    if [ "\$kernel" = 1 ] && [ "\$ours" = 1 ]; then
        echo "mixed commit \$(git log -1 --format='%h %s' "\$c")"
        echo "\$files" | sed 's/^/    /'
        bad=1
    fi
done
[ "\$bad" = 0 ] && echo "no commit mixes kernel changes with notes/CI"
exit \$bad
EOF

step notes "$out/notes.log" <<EOF
set -e
cd "$repo"
# Working notes live in a separate private repo (/storage/kernel/cosmo-notes). This tree carries code
# and CI only, so nothing private travels with a rebase or rides along in a patch.
if [ -e cosmo-notes ]; then
    echo "cosmo-notes/ must not exist here: notes belong in the private notes repo"
    exit 1
fi
echo "no notes in the kernel tree"
EOF

printf '\n'
if [ "$failed" = 0 ]; then
    echo "local-ci: all checks passed"
else
    echo "local-ci: FAILED — see the logs above"
fi
exit $failed
