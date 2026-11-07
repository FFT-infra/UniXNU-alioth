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
    # version 5 adds the frozen early-boot snapshot. Older images stop before these bytes, so read
    # them only when the header says it carries them, and treat a zero block as "not taken".
    # The threshold is the exact v5 size, not a lower bound: sizeof(struct boot_log_header) is 160
    # for version 5 (152 for version 4, plus the two uint32 at 148..155, padded to 8). header_bytes
    # is the only thing a reader has to tell whether a section exists, so the next version must be
    # able to derive its own starting value from this one; a fuzzy range would let it guess wrong.
    frozen_first, frozen_round = (struct.unpack_from('<2I', d, 148)
                                  if header_bytes >= 160 else (0, 0))
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
        'frozen_first': frozen_first, 'frozen_round': frozen_round,
    }


def slot_text(d, h, index):
    off = (h['slot_first'] + index * h['slot_blocks']) * BLOCK
    return text(d[off:off + h['slot_blocks'] * BLOCK])


# The diagnostic export (package/BootDiagExport.c) does not live in the log window. NativeUfs.cpp
# keeps a kExportBlocks = 65536 block (256 MiB) region immediately below the 8192-block log
# window, and the header's diag_first is a block offset inside that region, not inside userdata.
# Reading it at --skip would return the log window again, which is why the two are separate reads.
EXPORT_BELOW_LOG_BLOCKS = 65536
DIAG_MAGIC = b'SHEXR001'
DIAG_STREAM_MAGIC = b'M2VCACH1'


def read_diag_via_adb(serial, skip, count, path):
    target = '/tmp/sheng-diag.bin'
    cmd = ['adb']
    if serial:
        cmd += ['-s', serial]
    rc = subprocess.run(cmd + ['shell', 'dd if=/dev/block/by-name/userdata of=%s bs=4096 skip=%d count=%d'
                               % (target, skip, count)], stdin=subprocess.DEVNULL).returncode
    if rc:
        return False
    return subprocess.run(cmd + ['pull', target, path], stdin=subprocess.DEVNULL).returncode == 0


def safe_parts(path):
    """A relative path from the device, or None. The blob is written by the kernel from whatever
    the filesystem holds, so a name that walks upwards or is absolute is dropped, not followed."""
    parts = [p for p in path.replace('\\', '/').split('/') if p not in ('', '.')]
    if not parts or any(p == '..' for p in parts):
        return None
    return parts


def bgrx_to_bmp(raw, path):
    """The panel's scanout, as BootDiagExport.c writes it: 'SHOT', width, height, stride, then
    width*4-byte BGRX rows. BMP so the file opens without a dependency; the exporter stores BGRX
    and BMP wants BGR, so the fourth byte is dropped rather than reordered."""
    if len(raw) < 16 or raw[0:4] != b'SHOT':
        return False
    width, height, stride = struct.unpack_from('<3I', raw, 4)
    if not width or not height or stride < width * 4 or 16 + stride * height > len(raw):
        return False
    row_bytes = (width * 3 + 3) & ~3
    image = bytearray(row_bytes * height)
    for y in range(height):
        src = 16 + (height - 1 - y) * stride   # BMP rows are bottom-up
        dst = y * row_bytes
        for x in range(width):
            b, g, r = raw[src + x * 4], raw[src + x * 4 + 1], raw[src + x * 4 + 2]
            image[dst + x * 3:dst + x * 3 + 3] = bytes((b, g, r))
    size = 54 + len(image)
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', size, 0, 0, 54))
        f.write(struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(image), 2835, 2835, 0, 0))
        f.write(image)
    return True


def unpack_diag(blob, dest):
    """SHEXR001, then the M2VCACH1 stream BootDiagExport.c streams: the blob header's second word
    is the stream length, and the stream is the same shape the pipeline-cache export uses."""
    if len(blob) < 16 or blob[0:8] != DIAG_MAGIC:
        return None, 'no SHEXR001 header (the boot did not run sheng-diag-export, or the region moved)'
    stream_bytes, = struct.unpack_from('<I', blob, 8)
    stream = blob[16:16 + stream_bytes]
    if len(stream) < stream_bytes:
        return None, 'stream is %d bytes, the header names %d' % (len(stream), stream_bytes)
    if stream[0:8] != DIAG_STREAM_MAGIC:
        return None, 'stream has no M2VCACH1 header'
    at = 8
    root_len, = struct.unpack_from('<I', stream, at)
    at += 4 + root_len
    count, = struct.unpack_from('<I', stream, at)
    at += 4
    files, skipped = [], []
    files_root = os.path.join(dest, 'files')
    for _ in range(count):
        path_len, mode, uid, gid = struct.unpack_from('<4I', stream, at)
        size, = struct.unpack_from('<Q', stream, at + 16)
        at += 24
        name = stream[at:at + path_len].decode('utf-8', 'replace')
        at += path_len
        body = stream[at:at + size]
        at += size
        parts = safe_parts(name)
        if parts is None:
            skipped.append(name)
            continue
        out = os.path.join(files_root, *parts)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(body)
        files.append((name, len(body)))
    shot = os.path.join(files_root, 'screen.bgrx')
    screen = None
    if os.path.isfile(shot):
        with open(shot, 'rb') as f:
            raw = f.read()
        screen = os.path.join(dest, 'screen.bmp')
        if not bgrx_to_bmp(raw, screen):
            screen = None
    return {'files': files, 'skipped': skipped, 'screen': screen}, None


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
    ap.add_argument('--diag', action='store_true',
                    help='also read the diagnostic export named by the header and unpack it')
    ap.add_argument('--diag-blob', help='unpack an export blob already copied off the device')
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
                    'export_generation export_below diag_first diag_bytes '
                    'frozen_first frozen_round').split():
            value = h[key]
            f.write('%-18s %s\n' % (key, ('0x%x' % value) if key in ('boot_id', 'first_lba') else value))

    with open(os.path.join(dest, 'msgbuf.txt'), 'w') as f:
        off = h['msg_first'] * BLOCK
        f.write(text(d[off:off + h['msg_blocks'] * BLOCK]))

    # version 5: the frozen early-boot snapshot, taken once at uptime frozen_round's round and never
    # rewritten. It is the copy that still holds the boot-time IOLog after a long run, so it gets its
    # own file: comparing its first timestamp with msgbuf.txt's is the check that the freeze worked.
    # An older image, or one whose run ended before the freeze, has frozen_first == 0; write an
    # explicit note rather than an empty file so a missing freeze is visible and not mistaken for one.
    with open(os.path.join(dest, 'frozen.txt'), 'w') as f:
        if h['frozen_first']:
            off = h['frozen_first'] * BLOCK
            f.write(text(d[off:off + h['msg_blocks'] * BLOCK]))
        else:
            f.write('# no frozen snapshot: header version %d, frozen_first 0 (not taken)\n' % h['version'])

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

    # The diagnostic export: the unified log, the crash reports and the panel's scanout, written
    # by the kernel itself, so they survive a boot whose userspace never answers. The header
    # names it only when BootDiagExport.c ran; diag_bytes 0 means the boot did not ask for it.
    if args.diag or args.diag_blob:
        blob_path = os.path.join(dest, 'diag.bin')
        if args.diag_blob:
            with open(args.diag_blob, 'rb') as src, open(blob_path, 'wb') as dst:
                dst.write(src.read())
        elif not h['diag_bytes']:
            print('read-sheng-log: the header carries no diagnostic export (diag_bytes 0); '
                  'the boot did not pass sheng-diag-export', file=sys.stderr)
            return 5
        else:
            blocks = (h['diag_bytes'] + BLOCK - 1) // BLOCK
            export_skip = args.skip - EXPORT_BELOW_LOG_BLOCKS + h['diag_first']
            if not read_diag_via_adb(args.port, export_skip, blocks, blob_path):
                print('read-sheng-log: could not read the diagnostic export at block %d' % export_skip,
                      file=sys.stderr)
                return 2
        with open(blob_path, 'rb') as f:
            blob = f.read()
        # Both the files and their list live under diag/; unpack_diag makes dest/files itself only
        # when it has a file to write, so the list's directory is made here.
        diag_dir = os.path.join(dest, 'diag')
        os.makedirs(diag_dir, exist_ok=True)
        summary, error = unpack_diag(blob, diag_dir)
        if error:
            print('read-sheng-log: diagnostic export: %s' % error, file=sys.stderr)
            return 6
        with open(os.path.join(dest, 'diag', 'files.list'), 'w') as f:
            for name, size in sorted(summary['files']):
                f.write('%10d %s\n' % (size, name))
            for name in summary['skipped']:
                f.write('%10s %s\n' % ('skipped', name))
        print('  diag %d files in diag/files, %d dropped, screen %s'
              % (len(summary['files']), len(summary['skipped']), summary['screen'] or 'none'))

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
