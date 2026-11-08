"""Build recovery installations and publish unchanged standard Nordic DFU ZIPs."""
import hashlib
import json
from pathlib import Path
import struct
import zlib
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from nordic_update import NORDIC_INIT_MAX

ROOT = Path(__file__).resolve().parents[2]
RECOVERY_START, RECOVERY_END = 0x27000, 0x3b000
METADATA, APPLICATION_START, APPLICATION_END = 0x3b000, 0x3c000, 0xc7000
COMMITTED, LAYOUT = 0x43555732, 2
FLASH_PAGE_SIZE = 4096
FLASH_ERASED_BYTE = 255
WORD = struct.Struct('<I')
RECOVERY_DESCRIPTOR = struct.Struct('<8sII')
RECOVERY_MAGIC = b'CURECOV2'
RECOVERY_FOOTER_OFFSET = RECOVERY_END - RECOVERY_START - RECOVERY_DESCRIPTOR.size
BOOTLOADER_START, BOOTLOADER_END = 0xf3000, 0xfe000
BOOTLOADER_ABI = BOOTLOADER_END - RECOVERY_DESCRIPTOR.size
SYSTEM_STAGE = 0xbc000
SOFTDEVICE_START = 0x1000
RAM_START, RAM_END = 0x20000000, 0x20040000
IMAGE_VECTOR_SIZE = 2 * WORD.size
CACHE_HEADER = struct.Struct('<4I')
CACHE_CRC_OFFSET = CACHE_HEADER.size - WORD.size


def read_hex(path):
    memory, base = {}, 0
    ended = False
    for line in path.read_text().splitlines():
        if not line.startswith(':') or ended:
            raise ValueError('Invalid Intel HEX record')
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 255:
            raise ValueError('Invalid Intel HEX length or checksum')
        count, address, kind = record[0], int.from_bytes(record[1:3], 'big'), record[3]
        data = record[4:4 + count]
        if kind == 0:
            for offset, byte in enumerate(data):
                target = base + address + offset
                if target in memory:
                    raise ValueError('Overlapping Intel HEX records')
                memory[target] = byte
        elif kind == 1:
            ended = True
        elif kind in (2, 4) and count == 2:
            base = int.from_bytes(data, 'big') << (4 if kind == 2 else 16)
        elif kind not in (3, 5):
            raise ValueError('Unsupported Intel HEX record')
    if not ended:
        raise ValueError('Missing Intel HEX EOF')
    return memory


def write_hex(path, memory):
    def record(kind, address, data):
        raw = bytes([len(data)]) + address.to_bytes(2, 'big') + bytes([kind]) + data
        return ':' + (raw + bytes([-sum(raw) & 255])).hex().upper()
    lines, upper = [], None
    addresses = sorted(memory)
    i = 0
    while i < len(addresses):
        address = addresses[i]
        if address >> 16 != upper:
            upper = address >> 16
            lines.append(record(4, 0, upper.to_bytes(2, 'big')))
        data = bytearray([memory[address]])
        i += 1
        while i < len(addresses) and len(data) < 16 and addresses[i] == address + len(data) and addresses[i] >> 16 == upper:
            data.append(memory[addresses[i]])
            i += 1
        lines.append(record(0, address & 0xffff, bytes(data)))
    path.write_text('\n'.join([*lines, ':00000001FF']) + '\n')


def version_number(version):
    major, minor, patch = map(int, version.split('.'))
    if major > 255 or minor > 255 or patch > 65535:
        raise ValueError('Firmware version must fit MAJOR(8).MINOR(8).PATCH(16)')
    return major << 24 | minor << 16 | patch


def prepare(objects):
    app = read_hex(objects / 'application.hex')
    if not app or min(app) != APPLICATION_START or max(app) >= APPLICATION_END:
        raise ValueError('Application HEX is outside the mutable application partition')
    application = bytes(app.get(i, FLASH_ERASED_BYTE) for i in range(APPLICATION_START, max(app) + 1))
    sp, pc = struct.unpack_from('<II', application)
    if not (RAM_START < sp <= RAM_END and not sp % 8 and pc & 1 and
            APPLICATION_START <= pc - 1 < APPLICATION_START + len(application)):
        raise ValueError('Application vectors do not target the recovery layout')
    recovery = read_hex(objects / 'recovery.hex')
    if not recovery or min(recovery) != RECOVERY_START or max(recovery) >= RECOVERY_END - RECOVERY_DESCRIPTOR.size:
        raise ValueError('Recovery HEX is outside the permanent supervisor partition')
    supervisor = bytes(recovery.get(i, FLASH_ERASED_BYTE) for i in range(RECOVERY_START, RECOVERY_END - RECOVERY_DESCRIPTOR.size))
    footer = RECOVERY_DESCRIPTOR.pack(RECOVERY_MAGIC, LAYOUT, zlib.crc32(supervisor))
    installation = dict(enumerate(supervisor + footer, RECOVERY_START))
    # The signed BIN includes FF-filled gaps too. Emit those bytes explicitly so
    # an SWD programmer cannot preserve stale code while updating sparse HEX.
    installation.update(enumerate(application, APPLICATION_START))
    write_hex(objects / 'installation.hex', installation)


def publish(objects, destination, version):
    from nordic_update import inspect_zip
    data = (objects / 'chameleon-unleashed-app.zip').read_bytes()
    image = inspect_zip(data)['application']
    expected = read_hex(objects / 'installation.hex')
    binary = bytes(expected.get(i, FLASH_ERASED_BYTE) for i in range(RECOVERY_START, max(expected) + 1))
    if image['payload'] != binary:
        raise ValueError('Nordic ZIP differs from the freshly built installation')
    destination.mkdir(parents=True, exist_ok=True)
    for old in destination.iterdir():
        if old.is_file() and (old.name == 'latest.json' or old.suffix == '.cuw' or
                             (old.suffix == '.zip' and len(old.stem) == 64)):
            old.unlink()
    identity = hashlib.sha256(data).hexdigest()
    (destination / (identity + '.zip')).write_bytes(data)
    manifest = {'format': 2, 'layout': LAYOUT, 'hardware': 'ultra', 'version': version,
                'size': len(data), 'sha256': identity, 'path': identity + '.zip'}
    (destination / 'latest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    # The cache is internal installation state, outside the public file format.
    # In the signed Nordic BIN this whole page is FF. Runtime hashing substitutes
    # that same padding, avoiding a signature/cache circular dependency.
    packet = image['packet']
    cache = CACHE_HEADER.pack(COMMITTED, image['app_size'], len(packet), 0) + packet.ljust(NORDIC_INIT_MAX, b'\0')
    checksum = zlib.crc32(cache)
    cache = cache[:CACHE_CRC_OFFSET] + WORD.pack(checksum) + cache[CACHE_HEADER.size:]
    expected.update(enumerate(cache, METADATA))
    expected.update(enumerate(WORD.pack(COMMITTED), METADATA + FLASH_PAGE_SIZE - WORD.size))
    write_hex(objects / 'installation.hex', expected)
    return manifest


def add_to_cdn(archive, directory, manifest):
    if not archive.is_file():
        raise ValueError('Build the matching CDN archive before packaging firmware')
    with tempfile.TemporaryDirectory(dir=archive.parent) as temporary:
        output = Path(temporary) / 'cdn.zip'
        with ZipFile(archive) as original, ZipFile(output, 'w', compression=ZIP_DEFLATED) as package:
            for info in original.infolist():
                if not info.filename.startswith('updates/'):
                    package.writestr(info, original.read(info))
            for name in ['latest.json', manifest['path']]:
                package.write(directory / name, 'updates/' + name)
        output.replace(archive)


def installation_package(bootloader, application, destination):
    """Compose the two separately signed images without re-flashing SoftDevice.

    nrfutil's generator rejects BL+APP without SD, while Nordic's mobile archive
    readers and our CLI support separate bootloader and application entries.
    Neither signed init packet nor firmware payload is modified here.
    """
    manifest, files = {}, {}
    for kind, source in [('bootloader', bootloader), ('application', application)]:
        with ZipFile(source) as package:
            document = json.loads(package.read('manifest.json'))['manifest']
            if set(document) != {kind}:
                raise ValueError('Expected separately signed bootloader and application packages')
            entry = dict(document[kind])
            for field, extension in [('bin_file', '.bin'), ('dat_file', '.dat')]:
                name = kind + extension
                files[name] = package.read(entry[field])
                entry[field] = name
            manifest[kind] = entry
    with ZipFile(destination, 'w', compression=ZIP_DEFLATED) as package:
        package.writestr('manifest.json', json.dumps({'manifest': manifest}, indent=2))
        for name, data in files.items():
            package.writestr(name, data)


if __name__ == '__main__':
    import argparse
    from project_version import read_version
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objects', type=Path, default=ROOT / 'firmware/objects')
    parser.add_argument('--output', type=Path, default=ROOT / '.cache/web/cdn/updates')
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--bootloader-abi', action='store_true')
    options = parser.parse_args()
    if options.bootloader_abi:
        import os
        import subprocess
        nm = str(Path(os.environ.get('GNU_INSTALL_ROOT', '')) / 'arm-none-eabi-nm')
        symbols = {}
        for line in subprocess.check_output([nm, '-n', str(options.objects / 'bootloader.out')], text=True).splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] in {'__start_dfu_trans', 'usb_dfu_transport'}:
                symbols[parts[2]] = int(parts[0], 16)
        if symbols.get('__start_dfu_trans') != symbols.get('usb_dfu_transport') or len(symbols) != 2:
            raise ValueError('USB must remain the first Nordic DFU transport; refusing reordered bootloader')
        path = options.objects / 'bootloader.hex'
        memory = read_hex(path)
        body = bytes(memory.get(i, FLASH_ERASED_BYTE) for i in range(BOOTLOADER_START, BOOTLOADER_ABI))
        memory.update(enumerate(body, BOOTLOADER_START))
        memory.update(enumerate(WORD.pack(zlib.crc32(body)), BOOTLOADER_ABI + RECOVERY_DESCRIPTOR.size - WORD.size))
        write_hex(path, memory)
        (options.objects / 'bootloader.bin').write_bytes(bytes(memory.get(i, FLASH_ERASED_BYTE) for i in range(BOOTLOADER_START, BOOTLOADER_END)))
    elif options.publish:
        manifest = publish(options.objects, options.output, read_version())
        add_to_cdn(ROOT / '.cache/web/chameleon-unleashed-cdn.zip', options.output, manifest)
    else:
        prepare(options.objects)
