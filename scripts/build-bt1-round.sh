#!/bin/sh
# bt1: the button thread polls instead of blocking, and the log slots tell the truth.
#
# The three identical rounds read irq=0 rep=1. irq=0 means no interrupt ever arrived, so the
# plain assert_wait blocked on the first pass and the thread never read the levels again -
# rep=1 was that single sample, not a button being reported. assert_wait_timeout makes the
# same thread a poller when the line is dead.
#
# This round's job is to compile and boot; it is not a verdict on the interrupt path.
set -x
BASE=/home/xinyang/Lab/Bridge/WorkSpace/alioth
DEV=$BASE/NEW/UniXNU-alioth-btn
cd $BASE/NEW/UniXNU || exit 1

LOCK=$BASE/tmp/build.lock
if [ -e "$LOCK" ]; then echo "bt1: held by $(cat "$LOCK")"; exit 3; fi
echo "bt1 $(date +%s)" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT INT TERM

git -C "$DEV" status --short > $BASE/tmp/bt1-status-before.txt

# Only the mounts this build writes to. Every artifact (build/, out/, the XNU objects) lands
# under $BASE, and /tmp holds 38 MiB of scratch that is freed as it goes - measuring / against
# a 2 GiB threshold refuses the build over space the build never touches, which is what
# happened when orb's / sat at 1.6 GiB of /nix and system files.
for M in "$BASE"; do
    FREE_KB=$(df -Pk "$M" | awk 'NR==2 {print $4}')
    [ "$FREE_KB" -gt 2097152 ] || { echo "bt1: $M only $((FREE_KB/1024)) MiB"; exit 4; }
    echo "bt1: $M has $((FREE_KB/1024)) MiB free"
done
TMP_KB=$(df -Pk /tmp | awk 'NR==2 {print $4}')
echo "bt1: /tmp has $((TMP_KB/1024)) MiB free (scratch only; the artifacts go to $BASE)"
python3 $BASE/tmp/check-cmdline.py "$DEV/board.toml" || exit 5

. $BASE/tmp/kbuild-env.sh
export ADRENO650_PACKAGE=$DEV/package
export ADRENO650_DRIVER=$BASE/reference/repo/Adreno650
export ADRENO650_KITS=$DEV/kits
DRV="--local-module driver:ShengUfs=../NativeUfs-alioth --local-module driver:ArmCpu=../ArmCpu-alioth "
OUT=$BASE/tmp/boot-bt1.img
SHARED_OUT=$BASE/NEW/UniXNU/out/alioth

if [ ! -f "$DEV/drivers/ShengHidInput/NativeHidInput.cpp" ]; then
    git -C "$DEV" -c protocol.file.allow=always submodule update --init drivers/ShengHidInput || exit 9
fi

./unixnu defconfig alioth_defconfig -D "$DEV" $DRV || exit 1
./unixnu tree -D "$DEV" $DRV || exit 1
python3 $BASE/tmp/sync-tree-scripts.py \
  "$SHARED_OUT/ipados-17.7.11-21H461/alioth/tree/scripts" "$DEV/scripts" || exit 1
./unixnu build kernel -D "$DEV" $DRV || exit 1
sha256sum $SHARED_OUT/ipados-17.7.11-21H461/alioth/kernel/kernel.macho | tee $BASE/tmp/bt1-kernel-sha.txt
./unixnu build payload -D "$DEV" $DRV --uefi ../uefi-alioth \
  --unique-chip-id 0xe835c44a614a1914 \
  --trust-cache $BASE/NEW/UniXNU/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk $BASE/tmp/scratch-data-3g.dbr \
  --gpu-addon $BASE/tmp/gpu-parity3.gpuaddon \
  --kernel-arg sheng-build-tag=bt1 || exit 1
./unixnu bootimg -D "$DEV" $DRV --uefi ../uefi-alioth \
  --dtb ../uefi-alioth/Resources/DTBs/alioth.dtb -o "$OUT" || exit 1

git -C "$DEV" status --short > $BASE/tmp/bt1-status-after.txt
diff -q $BASE/tmp/bt1-status-before.txt $BASE/tmp/bt1-status-after.txt >/dev/null \
    && echo "bt1: source unchanged" \
    || { echo "bt1: SOURCE CHANGED - void"; exit 6; }
sha256sum "$OUT"
echo BUILD_DONE
