#!/usr/bin/env python3
"""Refuse a boot image whose payload was not rebuilt from the kernel that was just built.

The XNU kernel does not live in boot.img's kernel section: that section is the boot shim
plus the UEFI firmware volume, gzipped, followed by the device tree. The kernel travels
inside the payload ramdisk (EFI/UniXNU/Payloads/kernel.macho). Running `build kernel` and
then `bootimg` without `build payload` therefore packs the previous kernel and says nothing
about it; the device boots, the console looks healthy, and every measurement is taken on
stale code. That is what boot-usb65 did: the kernel.macho was new, the image still held the
old TOUCHSTAT format.

The payload's kernel is not byte-identical to out/<...>/kernel/kernel.macho, because
build payload first runs patch-xnu-entry-probe.py over its staged copy. The build records
both values in payload/payload.json, so this checks the two links of the chain there and
both must hold:

  1. payload.json kernel_sha256 matches the built kernel    (build payload ran after it)
  2. the kernel inside the image matches payload_kernel_sha256 (bootimg packed that payload)

  check-payload-kernel.py --kernel OUT/kernel/kernel.macho --image tmp/boot-usbNN.img

Exit 0 when both hold, 1 when either does not, 2 when the inputs cannot be read.
Needs mtools (mcopy) on PATH.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

KERNEL_IN_PAYLOAD = '::/EFI/UniXNU/Payloads/kernel.macho'
RECORD = Path('payload/payload.json')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def ramdisk_from_boot_image(image, out):
    """Write the ramdisk section of an Android boot image to out, return its byte count."""
    data = Path(image).read_bytes()
    if len(data) < 48:
        raise ValueError('image shorter than a boot header')
    kernel_size, _, ramdisk_size, _, _, _, _, page, version, _ = struct.unpack_from('<10I', data, 8)
    if version not in (0, 1, 2):
        raise ValueError(f'unexpected boot header version {version}')
    if not page or not ramdisk_size:
        raise ValueError('boot header has no page size or no ramdisk')
    offset = page * (1 + -(-kernel_size // page))
    if offset + ramdisk_size > len(data):
        raise ValueError('ramdisk section runs past the end of the image')
    out.write_bytes(data[offset:offset + ramdisk_size])
    return ramdisk_size


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernel', required=True, help='the kernel.macho the last build produced')
    parser.add_argument('--image', required=True, help='the boot image about to be flashed')
    parser.add_argument('--out', help='the device output directory holding payload/ (default: two levels up from --kernel)')
    args = parser.parse_args()

    mcopy = shutil.which('mcopy')
    if mcopy is None:
        print('check-payload-kernel: mcopy not found; install mtools', file=sys.stderr)
        return 2

    kernel = Path(args.kernel)
    image = Path(args.image)
    for path in (kernel, image):
        if not path.is_file():
            print(f'check-payload-kernel: {path} not found', file=sys.stderr)
            return 2
    out = Path(args.out) if args.out else kernel.parent.parent
    record_path = out / RECORD
    if not record_path.is_file():
        print(f'check-payload-kernel: {record_path} not found; run build payload', file=sys.stderr)
        return 2
    record = json.loads(record_path.read_text())

    failures = []

    built_hash = sha256(kernel)
    if record.get('kernel_sha256') != built_hash:
        failures.append(f'payload was built from a different kernel: record {record.get("kernel_sha256")} '
                        f'vs built {built_hash}')
    staged_hash = record.get('payload_kernel_sha256')
    with tempfile.TemporaryDirectory(prefix='check-payload-') as scratch_name:
        scratch = Path(scratch_name)
        ramdisk = scratch / 'ramdisk.img'
        try:
            size = ramdisk_from_boot_image(image, ramdisk)
        except ValueError as error:
            print(f'check-payload-kernel: {error}', file=sys.stderr)
            return 2
        extracted = scratch / 'kernel.macho'
        result = subprocess.run([mcopy, '-n', '-i', str(ramdisk), KERNEL_IN_PAYLOAD, str(extracted)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            print(f'check-payload-kernel: cannot read {KERNEL_IN_PAYLOAD} from {image.name} '
                  f'(ramdisk {size} bytes): {result.stderr.strip()}', file=sys.stderr)
            return 2
        packed_hash = sha256(extracted)
        packed_size = extracted.stat().st_size

    if packed_hash != staged_hash:
        failures.append(f'the image packs a different kernel than the payload recorded: '
                        f'image {packed_hash} vs record {staged_hash}')

    if failures:
        print(f'check-payload-kernel: REFUSING {image.name}', file=sys.stderr)
        for line in failures:
            print(f'  {line}', file=sys.stderr)
        print(f'  built kernel  {built_hash} ({kernel.stat().st_size} bytes)', file=sys.stderr)
        print('  run build payload, then bootimg, then check again', file=sys.stderr)
        return 1

    print(f'check-payload-kernel: {image.name} carries the payload kernel {packed_hash} ({packed_size} bytes), '
          f'built from kernel {built_hash}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
