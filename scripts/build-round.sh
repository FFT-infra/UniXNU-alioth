#!/bin/sh
# Build one round: kernel -> payload -> bootimg, from this repository's feeds pins.
#
# Run on orb (the Linux toolchain lives there). Every path is derived from this script's
# location, so the workspace can move or be mounted at another prefix.
#
#   TAG=ren2 ADDON=gpuaddon-align.gpuaddon scripts/build-round.sh [payload args]
#
# Extra arguments after the tag go to "unixnu build payload" unchanged, so a round can add a
# boot argument without editing this file:
#   TAG=rail2 scripts/build-round.sh --kernel-arg pcie=13 --kernel-arg pcie_rail=2
set -x
# unixnu reads git output as UTF-8; a localized git message in a non-UTF-8 locale aborts it.
export LC_ALL=C
export LANG=C
DEVICE=$(cd "$(dirname "$0")/.." && pwd) || exit 1
BASE=$(cd "$DEVICE/.." && pwd) || exit 1
WORKSPACE=$(cd "$BASE/.." && pwd) || exit 1
cd "$BASE/UniXNU" || exit 1
if [ -f "$WORKSPACE/tmp/kbuild-env.sh" ]; then
  . "$WORKSPACE/tmp/kbuild-env.sh"
fi
# kbuild-env.sh is written for this workspace on orb; make the cache path follow the checkout.
export UNIXNU_KERNELCACHE="$BASE/UniXNU/out/ipsw/21H461/kernelcache.decompressed"

TAG=${TAG:-round}
EXTRA_ARGS="$*"
LOCK="$WORKSPACE/tmp/build.lock"
MINE="round $TAG $(date +%s)"

test ! -e "$LOCK" || { echo "HELD: $(cat "$LOCK")"; exit 1; }
RUNNING=$(pgrep -cf "unixnu build" 2>/dev/null); RUNNING=${RUNNING:-0}
test "$RUNNING" -le 1 || { echo "BUILD RUNNING ($RUNNING)"; exit 1; }
( set -C; echo "$MINE" > "$LOCK" ) || { echo "HELD (race): $(cat "$LOCK")"; exit 1; }
trap "test \"\$(cat \"\$LOCK\" 2>/dev/null)\" = \"\$MINE\" && rm -f \"\$LOCK\"" EXIT INT TERM

# The branch this round builds from. Anything else means someone else moved the
# base repository and this script would silently build the wrong tree.
BRANCH=$(git rev-parse --abbrev-ref HEAD)
case "$BRANCH" in main|test|test-gemini|align-main-*) ;; *) echo "NOT ON main/test/test-gemini/align-main-*: $BRANCH"; exit 1 ;; esac

WORK="$BASE/UniXNU/out/alioth/ipados-17.7.11-21H461/alioth"
OUT="$WORKSPACE/tmp/boot-$TAG.img"
DEV="$DEVICE"
DRV=""

./unixnu defconfig alioth_defconfig -D "$DEV" $DRV || exit 1
# The feed fetch was completed from the Mac checkout; the shared unixnu.lock and out/feeds
# are current. Re-running it here hits an orb-side Git index-format error unrelated to the tree.
./unixnu tree -D "$DEV" $DRV || exit 1
python3 "$WORKSPACE/tmp/sync-tree-scripts.py" "$WORK/tree/scripts" "$DEV/scripts" || exit 1

# What the build resolved, not what a checkout says. ArmSmmu4K and
# GopFramebuffer come from the base repository's own checkouts, so check the checkout itself
# rather than a status entry that unselected components never produce.
path=kits/ArmSmmu4K; pin=83969cd
test -d "$path" || { echo "MISSING: $path"; exit 1; }
head=$(git -C "$path" rev-parse --short HEAD 2>/dev/null)
case "$head" in "$pin"*) echo "resolved $path $head" ;; *) echo "WRONG PIN: $path is $head, expected $pin"; exit 1 ;; esac
for path in drivers/GopFramebuffer drivers/AdrenoMetal drivers/Adreno740; do
  printf 'resolved %s %s\n' "$path" "$(git -C "$path" rev-parse --short HEAD 2>/dev/null || echo none)"
done

# The A650 driver is a submodule of this repository and a component in the base catalog.
# The tree links it as drivers/Adreno650, and build-adreno650.py defaults to that path.
ADRENO650_DRIVER="$DEVICE/drivers/Adreno650"
test -d "$ADRENO650_DRIVER" || { echo "NO DRIVER: $ADRENO650_DRIVER"; exit 1; }
export ADRENO650_DRIVER
printf 'driver %s\n' "$(git -C "$ADRENO650_DRIVER" rev-parse --short HEAD 2>/dev/null || echo 'no git')"

# The device repository pins ShengHidInput as a submodule whose URL is the local
# shared checkout, and git refuses the file protocol by default. Scope the
# permission to this call.
if [ ! -e "$DEV/drivers/ShengHidInput/NativeHidInput.cpp" ]; then
  git -C "$DEV" -c protocol.file.allow=always submodule update --init drivers/ShengHidInput || exit 1
fi

./unixnu build kernel -D "$DEV" $DRV || exit 1
echo KERNEL_SHA
sha256sum "$WORK/kernel/kernel.macho"
stat -c "%s bytes" "$WORK/kernel/kernel.macho"

./unixnu build payload -D "$DEV" $DRV --uefi "$BASE/uefi-alioth" \
  --unique-chip-id 0xe835c44a614a1914 \
  --trust-cache "$BASE/UniXNU/build/adreno-metal-plugin/owner-trust.bundle" \
  --data-ramdisk "$WORKSPACE/tmp/scratch-data-3g.dbr" \
  --gpu-addon "$WORKSPACE/tmp/${ADDON:-gpuaddon-align.gpuaddon}" --kernel-arg sheng-build-tag=$TAG $EXTRA_ARGS || exit 1

./unixnu bootimg -D "$DEV" $DRV --uefi "$BASE/uefi-alioth" \
  --dtb "$BASE/uefi-alioth/Resources/DTBs/alioth.dtb" -o "$OUT" || exit 1
echo BOOTIMG_SHA
sha256sum "$OUT"
# sha changing only says the file changed; this says whose kernel is inside it.
strings "$OUT" | grep -o "sheng-build-tag=[a-z0-9]*" | head -1
# Which addon went in, by name and hash. Without this the console of a stale-addon build reads
# exactly like a lost race (askers>0, marks=0) and a whole round batch gets spent on it.
echo "ADDON ${ADDON:-gpuaddon-align.gpuaddon} $(sha256sum "$WORKSPACE/tmp/${ADDON:-gpuaddon-align.gpuaddon}" 2>/dev/null | cut -c1-16)"
echo LINK_OK
