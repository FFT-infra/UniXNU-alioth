#!/bin/sh
# One unattended round: flash the test image, wait for the board to return to the bootloader
# by itself, read the persistent log back through the recovery, and leave the board in the
# bootloader on the test slot.
#
# This is docs/zh/porting/log-capture.md section 6 for this board. The reader here is TWRP in
# the other boot slot, not the sheng Linux image: this board has no sm8550 reader build, and
# TWRP already provides adb and a raw block device.
#
# Fail-closed: a stage that fails records its name and exits non-zero. Nothing is retried.
#
#   run-sheng-unattended.sh <test.img> [timeout seconds]
#
# Environment: SERIAL selects the board when several are attached.

set -u

IMG=${1:?usage: run-sheng-unattended.sh <test.img> [timeout]}
TIMEOUT=${2:-180}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${SHENG_LOG_OUT:-$HERE/../build/sheng-logs}
SERIAL=${SERIAL:-}

say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { say "FAILED at $1"; exit 1; }

[ -f "$IMG" ] || fail "image $IMG does not exist"

FB="fastboot"
[ -n "$SERIAL" ] && FB="fastboot -s $SERIAL"
ADBM="adb"
[ -n "$SERIAL" ] && ADBM="adb -s $SERIAL"

say "stage 1: board must already be in the bootloader"
$FB devices | grep -q fastboot || fail "no fastboot device"

say "stage 2: test slot must carry TWRP on the other slot for the read-back"
say "stage 3: flash $IMG to boot_b"
$FB flash boot_b "$IMG" >/dev/null 2>&1 || fail "flash boot_b"
$FB --set-active=b >/dev/null 2>&1 || fail "set-active=b"
$FB reboot >/dev/null 2>&1 || fail "reboot"

say "stage 4: wait up to ${TIMEOUT}s for the board to return to the bootloader by itself"
i=0
RETURNED=0
while [ "$i" -lt "$TIMEOUT" ]; do
    sleep 3
    i=$((i + 3))
    if $FB devices 2>/dev/null | grep -q fastboot; then RETURNED=1; break; fi
done
[ "$RETURNED" = 1 ] || fail "no return to the bootloader within ${TIMEOUT}s (test slot holds the image)"
say "returned after ${i}s"

say "stage 5: boot the reader on slot a"
$FB --set-active=a >/dev/null 2>&1 || fail "set-active=a"
$FB reboot >/dev/null 2>&1 || fail "reader reboot"
i=0
UP=0
while [ "$i" -lt 180 ]; do
    sleep 3
    i=$((i + 3))
    if $ADBM devices 2>/dev/null | grep -qE 'recovery|device$'; then UP=1; break; fi
done
[ "$UP" = 1 ] || fail "reader did not bring adb up"

say "stage 6: read the window and decode it"
# A dd left running by an interrupted read keeps the block device claimed and every later
# read returns nothing, so clear it first (measured: two runs stuck at 0 bytes until the
# reader was rebooted).
$ADBM shell 'killall -9 dd 2>/dev/null; true' >/dev/null 2>&1
# Bounded, because a degraded UFS link in the reader turns the 32 MiB read into a many-hour
# transfer that looks exactly like a hang (measured: 8 KiB in 2.7 s, then nothing at all).
# The slot is restored either way; only the exit status carries the failure.
python3 "$HERE/read-sheng-log.py" --port "$SERIAL" --out "$OUT" &
RPID=$!
n=0
while kill -0 $RPID 2>/dev/null && [ "$n" -lt 300 ]; do sleep 5; n=$((n + 5)); done
if kill -0 $RPID 2>/dev/null; then
    kill -9 $RPID 2>/dev/null
    READ_OK=0
    say "read did not finish in ${n}s (degraded reader link?)"
else
    wait $RPID
    READ_OK=$?
fi

say "stage 7: back to the test slot and the board's own bootloader"
# Not "$ADBM reboot bootloader": this reader is TWRP, whose cmdline carries
# twrpfastboot=1, and that lands in TWRP's own fastboot (serial 9c6a8eb6, empty
# version-bootloader, is-userspace=no) rather than the board's ABL. Reach the ABL the way
# the board does it on its own: boot the test slot and let its timed reset return.
$FB --set-active=b >/dev/null 2>&1 || fail "set-active=b after read"
$ADBM reboot >/dev/null 2>&1 || fail "reader reboot"
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
    sleep 3
    i=$((i + 3))
    if $FB devices 2>/dev/null | grep -q fastboot; then break; fi
done
$FB devices | grep -q fastboot || fail "final fastboot check"
[ "$READ_OK" = 0 ] || fail "read stage"
say "done"
