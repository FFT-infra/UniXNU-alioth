#!/bin/sh
# btn6: the panel blink, as the brightness verdict.
#
# Mode 2 leads with three off/on blinks (0x28 then 0x10 dark for 1 s, 0x11 then 0x29 back),
# then the level sweep. The blink needs neither a read-back nor an observer judging a small
# brightness change: if the packets reach the panel the screen goes dark and comes back.
#
# btn5 hung because sendPacket polled twice per packet for events this board never produces
# (the vendor only takes the TPG FIFO path in secure_mode, dsi_ctrl.c:1170-1180, and completion
# is an interrupt nothing here attaches, alioth.dts:30921-30922). Both polls are now optional
# and non-fatal, and stats[7] counts how many packets were launched.
#
# Structure: this branch has its own device worktree (NEW/UniXNU-alioth-btn, branch btn-work),
# so board.toml is edited and committed here rather than patched in a shared file. The build
# output root is the shared out/alioth on purpose: a fresh --out root is missing the archives
# and seeds that unixnu tree does not rebuild, and wifi3 died there twice.
#
# Locks:
#   tmp/build.lock  serialises the build (out/ and the kernel source tree are shared)
#   tmp/board.lock  serialises the board (fastboot, adb and the serial port are exclusive)
set -x
BASE=/home/xinyang/Lab/Bridge/WorkSpace/alioth
DEV=$BASE/NEW/UniXNU-alioth-btn
cd $BASE/NEW/UniXNU || exit 1

LOCK=$BASE/tmp/build.lock
if [ -e "$LOCK" ]; then echo "btn6: $LOCK held by $(cat "$LOCK")"; exit 3; fi
echo "btn6 $(date +%s)" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT INT TERM

# Source state before and after; a difference means another line changed the tree mid-build.
snapshot_status() {
    git -C "$DEV" status --short
    git -C $BASE/reference/repo/Adreno650 status --short
}
snapshot_status > $BASE/tmp/btn6-status-before.txt

# Both mounts that receive writes: the workspace holds out/, / holds /tmp's intermediates.
for M in / "$BASE"; do
    FREE_KB=$(df -Pk "$M" | awk 'NR==2 {print $4}')
    [ "$FREE_KB" -gt 2097152 ] || { echo "btn6: $M has only $((FREE_KB/1024)) MiB"; exit 4; }
    echo "btn6: $M has $((FREE_KB/1024)) MiB free"
done

python3 $BASE/tmp/check-cmdline.py "$DEV/board.toml" || { echo "btn6: command line too long"; exit 5; }

. $BASE/tmp/kbuild-env.sh
export ADRENO650_PACKAGE=$DEV/package
export ADRENO650_DRIVER=$BASE/reference/repo/Adreno650
export ADRENO650_KITS=$DEV/kits
DRV="--local-module driver:ShengUfs=../NativeUfs-alioth --local-module driver:ArmCpu=../ArmCpu-alioth "
OUT=$BASE/tmp/boot-btn6.img
SHARED_OUT=$BASE/NEW/UniXNU/out/alioth

# A fresh device worktree has an empty submodule directory, and git refuses the file-protocol
# clone `unixnu tree` then attempts ("transport 'file' not allowed"), because this branch's
# submodule URL is the local shared checkout. Allowing it here is scoped to this call and the
# source is the same repository the gitlink names, so nothing is fetched from anywhere new.
if [ ! -f "$DEV/drivers/ShengHidInput/NativeHidInput.cpp" ]; then
    git -C "$DEV" -c protocol.file.allow=always submodule update --init drivers/ShengHidInput || exit 9
    echo "btn6: initialised the ShengHidInput submodule from the local checkout"
fi

./unixnu defconfig alioth_defconfig -D "$DEV" $DRV || exit 1
./unixnu tree -D "$DEV" $DRV || exit 1
python3 $BASE/tmp/sync-tree-scripts.py \
  "$SHARED_OUT/ipados-17.7.11-21H461/alioth/tree/scripts" \
  "$DEV/scripts" || exit 1
./unixnu build kernel -D "$DEV" $DRV || exit 1
# The kernel this round built, recorded before anything else touches it.
KERNEL=$SHARED_OUT/ipados-17.7.11-21H461/alioth/kernel/kernel.macho
sha256sum "$KERNEL" | tee $BASE/tmp/btn6-kernel-sha.txt
./unixnu build payload -D "$DEV" $DRV --uefi ../uefi-alioth \
  --unique-chip-id 0xe835c44a614a1914 \
  --trust-cache $BASE/NEW/UniXNU/build/adreno-metal-plugin/owner-trust.bundle \
  --data-ramdisk $BASE/tmp/scratch-data-3g.dbr \
  --gpu-addon $BASE/tmp/gpu-parity3.gpuaddon \
  --kernel-arg sheng-build-tag=btn6 || exit 1
./unixnu bootimg -D "$DEV" $DRV --uefi ../uefi-alioth \
  --dtb ../uefi-alioth/Resources/DTBs/alioth.dtb -o "$OUT" || exit 1

# The image must carry this round's kernel: compare the kernel's sha against what the payload
# recorded, so "the boot image is this build" is evidence rather than an assumption.
python3 - "$SHARED_OUT/ipados-17.7.11-21H461/alioth/payload/payload.json" "$BASE/tmp/btn6-kernel-sha.txt" <<'PY'
import json, sys
payload = json.load(open(sys.argv[1]))
built = open(sys.argv[2]).read().split()[0]
# payload.json records the input kernel as kernel_sha256 and the packed one as
# payload_kernel_sha256; the first is what this round's build must equal.
recorded = payload['kernel_sha256']
print('btn6: built kernel %s, payload input %s' % (built[:16], recorded[:16]))
if recorded == built:
    print('btn6: MATCH')
else:
    print('btn6: MISMATCH - the image is not this build')
    raise SystemExit(7)
PY

snapshot_status > $BASE/tmp/btn6-status-after.txt
if diff -q $BASE/tmp/btn6-status-before.txt $BASE/tmp/btn6-status-after.txt >/dev/null; then
    echo "btn6: source unchanged through the build"
else
    echo "btn6: SOURCE CHANGED DURING THE BUILD - this round's reading is void"
    diff $BASE/tmp/btn6-status-before.txt $BASE/tmp/btn6-status-after.txt
    exit 6
fi
sha256sum "$OUT"
echo BUILD_DONE
