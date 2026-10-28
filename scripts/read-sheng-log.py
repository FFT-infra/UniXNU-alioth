#!/usr/bin/env python3
"""Read the persistent boot log back and decode it into one directory per boot.

This is the reader half of docs/zh/porting/log-capture.md steps 4-6 for this board. The
format is the one package/BootLog.c writes: block 0 is the header, blocks 1..32 the kernel
message buffer, then text slots of LogSlotBlocks blocks each, starting at LogSlotFirst. The
window is the last kLogReserveBlocks of userdata, and the header names its own absolute LBA,
so a header that is present always agrees with where it was read from.

The device is read through whatever recovery is running with adb up (this board uses TWRP in
a boot slot), because the reader image the sheng example uses is built for sm8550. The source
partition is only read, never written.

  read-sheng-log.py --port <adb serial> --out build/sheng-logs
  read-sheng-log.py --image tmp/log-usb103.bin --out build/sheng-logs

Output, matching the sheng example's convention:

  <out>/<tag>-<boot id>/log.bin      the raw window
                        /header.txt  the decoded header
                        /msgbuf.txt  the kernel message buffer
                        /slots.txt   every written slot, oldest first
                        /latest.txt  the newest slot

Exit codes keep three different faults apart:
  0  a header was found and decoded
  2  the window could not be read at all (adb/dd failed or timed out)
  3  the window was read but has no header or is short (nothing was written there)
  4  decoded, but the header's LBA does not match where the window was read
"""
import argparse
import os
import struct
import subprocess
import sys

BLOCK = 4096
MSG_FIRST, MSG_BLOCKS = 1, 32
SLOT_FIRST, SLOT_BLOCKS = 33, 16
MAGIC = b'SHLOG001'
# Where the window sits, in the units dd uses. /dev/block/by-name/userdata is a partition, so
# dd's skip counts blocks from the partition start, not from the disk: 59,086,843 is the last
# 8192 blocks of a 59,095,035-block partition. The absolute LBA (61,872,123 here) is what the
# header records, and the two differ by the partition's first_lba (0x2a8000); passing the
# absolute value to dd seeks past the end of the partition and reads nothing at all.
DEFAULT_FIRST_LBA = 0x2a8000
DEFAULT_SKIP = 59086843
DEFAULT_COUNT = 8192


def read_via_adb(serial, skip, count, path):
    target = '/tmp/sheng-log.bin'
    cmd = ['adb']
    if serial:
        cmd += ['-s', serial]
    # stdin must be closed: `adb shell` forwards its own stdin to the remote command, so a
    # caller whose stdin never ends (a background job, a pipe) leaves dd running forever.
    rc = subprocess.run(cmd + ['shell', 'dd if=/dev/block/by-name/userdata of=%s bs=4096 skip=%d count=%d'
                               % (target, skip, count)], stdin=subprocess.DEVNULL).returncode
    if rc:
        return False
    rc = subprocess.run(cmd + ['pull', target, path], stdin=subprocess.DEVNULL).returncode
    return rc == 0


def text(raw):
    end = raw.find(b'\0')
    if end >= 0:
        raw = raw[:end]
    return raw.decode('latin1', 'replace')


def header_fields(d):
    version, header_bytes = struct.unpack_from('<II', d, 8)
    boot_id, first_lba = struct.unpack_from('<QQ', d, 16)
    (window_blocks, msg_first, msg_blocks, msg_bytes, slot_first, slot_blocks,
     slot_count, slots_written) = struct.unpack_from('<8I', d, 32)
    uptime_s, rounds, write_errors, gpt_open, gpt_last, state = struct.unpack_from('<6I', d, 64)
    tag = text(d[88:120])
    (export_first, export_blocks, export_bytes, export_generation) = struct.unpack_from('<4I', d, 120)
    export_below, = struct.unpack_from('<I', d, 136)
    diag_first, diag_bytes = struct.unpack_from('<2I', d, 140)
    return {
        'magic': d[0:8].decode('latin1'),
        'version': version, 'header_bytes': header_bytes,
        'boot_id': boot_id, 'first_lba': first_lba,
        'window_blocks': window_blocks, 'msg_first': msg_first, 'msg_blocks': msg_blocks,
        'msg_bytes': msg_bytes, 'slot_first': slot_first, 'slot_blocks': slot_blocks,
        'slot_count': slot_count, 'slots_written': slots_written,
        'uptime_s': uptime_s, 'rounds': rounds, 'write_errors': write_errors,
        'gpt_open': gpt_open, 'gpt_last': gpt_last, 'state': state, 'tag': tag,
        'export_first': export_first, 'export_blocks': export_blocks,
        'export_bytes': export_bytes, 'export_generation': export_generation,
        'export_below': export_below, 'diag_first': diag_first, 'diag_bytes': diag_bytes,
    }


def slot_text(d, h, index):
    off = (h['slot_first'] + index * h['slot_blocks']) * BLOCK
    return text(d[off:off + h['slot_blocks'] * BLOCK])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', help='adb serial; read the window from the running recovery')
    ap.add_argument('--image', help='decode a window already copied off the device')
    ap.add_argument('--out', default='build/sheng-logs')
    ap.add_argument('--skip', type=int, default=DEFAULT_SKIP,
                    help='blocks from the start of the userdata partition to the window')
    ap.add_argument('--first-lba', type=lambda v: int(v, 0), default=DEFAULT_FIRST_LBA,
                    help='the partition first_lba from board.toml, added back for the header check')
    ap.add_argument('--count', type=int, default=DEFAULT_COUNT)
    ap.add_argument('--tag', default='alioth', help='directory prefix when the header has none')
    args = ap.parse_args()
    # None means the option was not given; '' means adb with no serial, which is what the
    # unattended script passes when SERIAL is unset. Both must stay distinguishable.
    if args.port is None and not args.image:
        ap.error('one of --port or --image is required')

    out_root = os.path.abspath(args.out)
    os.makedirs(out_root, exist_ok=True)
    raw_path = os.path.join(out_root, '.window.bin')

    if args.image:
        with open(args.image, 'rb') as src, open(raw_path, 'wb') as dst:
            dst.write(src.read())
    elif not read_via_adb(args.port, args.skip, args.count, raw_path):
        print('read-sheng-log: could not read the window over adb', file=sys.stderr)
        return 2

    d = open(raw_path, 'rb').read()
    if len(d) < BLOCK * 40 or d[0:8] != MAGIC:
        # A short read and an unwritten window are different faults: the first means the read
        # never happened (a degraded link, a wrong skip), the second means the writer did not
        # run. Exit codes keep them apart for the caller.
        print('read-sheng-log: window has no header (%d bytes read, wanted %d)'
              % (len(d), args.count * BLOCK), file=sys.stderr)
        os.unlink(raw_path)
        return 3

    h = header_fields(d)
    name = '%s-%016x' % (h['tag'] or args.tag, h['boot_id'])
    dest = os.path.join(out_root, name)
    os.makedirs(dest, exist_ok=True)
    os.replace(raw_path, os.path.join(dest, 'log.bin'))

    with open(os.path.join(dest, 'header.txt'), 'w') as f:
        for key in ('magic version header_bytes boot_id first_lba window_blocks msg_first msg_blocks '
                    'msg_bytes slot_first slot_blocks slot_count slots_written uptime_s rounds '
                    'write_errors gpt_open gpt_last state tag export_first export_blocks export_bytes '
                    'export_generation export_below diag_first diag_bytes').split():
            value = h[key]
            f.write('%-18s %s\n' % (key, ('0x%x' % value) if key in ('boot_id', 'first_lba') else value))

    with open(os.path.join(dest, 'msgbuf.txt'), 'w') as f:
        off = h['msg_first'] * BLOCK
        f.write(text(d[off:off + h['msg_blocks'] * BLOCK]))

    slots = []
    for i in range(min(h['slots_written'], h['slot_count'])):
        body = slot_text(d, h, i)
        if body.strip():
            slots.append((i, body))
    with open(os.path.join(dest, 'slots.txt'), 'w') as f:
        for i, body in slots:
            f.write('===== slot %d =====\n%s\n' % (i, body))

    latest = slot_text(d, h, (h['slots_written'] - 1) % h['slot_count']) if h['slots_written'] else ''
    with open(os.path.join(dest, 'latest.txt'), 'w') as f:
        f.write(latest)

    # The header records the absolute LBA; the read was at --skip blocks into a partition that
    # starts at --first-lba. A mismatch means the window moved (the partition geometry changed),
    # which is worth a distinct exit code rather than a warning.
    read_at = args.first_lba + args.skip
    moved = False
    if not args.image and h['first_lba'] != read_at:
        print('read-sheng-log: header names LBA %d but the window was read at %d (partition '
              'first_lba %d + skip %d); the log window moved'
              % (h['first_lba'], read_at, args.first_lba, args.skip), file=sys.stderr)
        moved = True

    print('read-sheng-log: %s' % dest)
    print('  boot %016x  uptime %us  rounds %u  slots %u/%u  errors %u  state %u'
          % (h['boot_id'], h['uptime_s'], h['rounds'], h['slots_written'], h['slot_count'],
             h['write_errors'], h['state']))
    return 4 if moved else 0


if __name__ == '__main__':
    sys.exit(main())
