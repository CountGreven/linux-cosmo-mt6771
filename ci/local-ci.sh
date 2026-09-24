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

# Schema validation of OUR device tree against the bindings, plus the two binding files this branch
# edits. Needs dtschema (dt-validate); COSMO_DT_VENV points at a venv holding it. A missing validator
# fails the job: a check that cannot run must not read as a check that passed.
# Only complaints naming mt6771 or cosmo count; the rest of the tree's dtbs_check noise is upstream's.
step dtbs_check "$out/dtbs_check.log" <<EOF
set -e
cd "$repo"
export PATH="${COSMO_DT_VENV:-$out/venv}/bin:\$PATH"
command -v dt-validate >/dev/null || { echo "dt-validate not found: pip install dtschema yamllint (venv: ${COSMO_DT_VENV:-$out/venv})"; exit 1; }
make O="$out/build" defconfig >/dev/null
make O="$out/build" DT_SCHEMA_FILES="interrupt-controller/mediatek,mt6577-sysirq.yaml vendor-prefixes.yaml" dt_binding_check
# CHECK_DTBS=y with a dtb target validates that dtb alone, not every dtb in the tree
rm -f "$out/build/arch/arm64/boot/dts/mediatek/mt6771-planet-cosmo.dtb"
if ! make O="$out/build" -j$jobs CHECK_DTBS=y mediatek/mt6771-planet-cosmo.dtb 2> "$out/dtbs_check.stderr"; then
    cat "$out/dtbs_check.stderr"
    echo "the build itself failed, so nothing was checked"
    exit 1
fi
cat "$out/dtbs_check.stderr"
if grep -E "mt6771|cosmo" "$out/dtbs_check.stderr"; then
    echo "dtbs_check has complaints about our device trees"
    exit 1
fi
echo "dtbs_check had nothing to say about our device trees"
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
# checkpatch exits non-zero on warnings as well as errors, and some of ours are structural: the
# bindings for planet,cosmo and mediatek,mt6771 do not exist upstream yet, so every compatible we
# write is "un-documented" until those land. Gating on that would mean a permanently red board and a
# gate nobody reads. Errors fail the job; warnings are printed and counted, and the binding task is
# what clears them.
./scripts/checkpatch.pl --terse --no-signoff "\$patches"/*.patch | tee "$out/checkpatch.raw" || true
errors=\$(awk -F'[ ,]' '/^total:/ {s += \$2} END {print s + 0}' "$out/checkpatch.raw")
warnings=\$(awk '/^total:/ {for (i = 1; i <= NF; i++) if (\$(i + 1) ~ /^warning/) s += \$i} END {print s + 0}' "$out/checkpatch.raw")
echo "checkpatch: \$errors errors, \$warnings warnings"
[ "\$errors" = "0" ] || { echo "checkpatch: errors must be fixed"; exit 1; }
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
