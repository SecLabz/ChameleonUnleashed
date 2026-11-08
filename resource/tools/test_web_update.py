"""Standard Nordic signatures, bounded installers and interrupted flash transactions."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from zipfile import ZipFile, ZIP_DEFLATED

from build_recovery import assets
from nordic_update import inspect_packet, inspect_zip, fields, ROOT
from web_update import (RECOVERY_START, RECOVERY_END, METADATA, APPLICATION_START,
                        APPLICATION_END, COMMITTED, read_hex, write_hex, prepare, publish,
                        installation_package)


def integer(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def scalar(number, value):
    return integer(number << 3) + integer(value)


def blob(number, value):
    return integer((number << 3) | 2) + integer(len(value)) + value


def signed_packet(payload, version=0x1000001, kind=0, sd_size=0, bl_size=0, requirements=None):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    init = (scalar(1, version) + scalar(2, 0)
            + (blob(3, integer(0x100)) if requirements is None else requirements) + scalar(4, kind)
            + scalar(5, sd_size) + scalar(6, bl_size) + scalar(7, len(payload) if kind == 0 else 0)
            + blob(8, scalar(1, 3) + blob(2, hashlib.sha256(payload).digest()[::-1])))
    key = serialization.load_pem_private_key((ROOT / 'resource/dfu_key/chameleon.pem').read_bytes(), password=None)
    r, s = utils.decode_dss_signature(key.sign(init, ec.ECDSA(hashes.SHA256())))
    signature = r.to_bytes(32, 'little') + s.to_bytes(32, 'little')
    return blob(2, blob(1, scalar(1, 1) + blob(2, init)) + scalar(2, 0) + blob(3, signature))


def requirement_cases():
    return {
        'packed': (blob(3, integer(0x101) + integer(0x100) + integer(0x102)), True),
        'unpacked': (scalar(3, 0x100) + scalar(3, 0x101), True),
        'mixed': (scalar(3, 0x101) + blob(3, integer(0x100)) + scalar(3, 0x102), True),
        'unsupported': (blob(3, integer(0x101) + integer(0x102)), False),
        'truncated': (blob(3, b'\x80'), False),
        'overflow': (blob(3, b'\x80\x80\x80\x80\x10'), False),
    }


def archive(application, packet, system=None, system_packet=None):
    output = io.BytesIO()
    with ZipFile(output, 'w', compression=ZIP_DEFLATED) as z:
        manifest = {'application': {'bin_file': 'application.bin', 'dat_file': 'application.dat'}}
        z.writestr('application.bin', application)
        z.writestr('application.dat', packet)
        if system is not None:
            manifest['softdevice_bootloader'] = {'bin_file': 'sd_bl.bin', 'dat_file': 'sd_bl.dat'}
            z.writestr('sd_bl.bin', system)
            z.writestr('sd_bl.dat', system_packet)
        z.writestr('manifest.json', json.dumps({'manifest': manifest}))
    return output.getvalue()


def fixtures():
    prefix = bytearray([0x32] * (RECOVERY_END - RECOVERY_START))
    struct.pack_into('<II', prefix, 0, 0x20040000, RECOVERY_START + 9)
    prefix[-16:] = struct.pack('<8sII', b'CURECOV2', 2, zlib.crc32(prefix[:-16]))
    main = struct.pack('<II', 0x20038000, APPLICATION_START + 9) + bytes([0x57] * 4997)
    old_main = main[:8] + bytes([0x81] * (len(main) - 8))
    application = bytes(prefix) + bytes([255] * 4096) + main
    old = bytes(prefix) + bytes([255] * 4096) + old_main
    changed = bytearray(application)
    changed[1024] ^= 1
    struct.pack_into('<I', changed, len(prefix) - 4, zlib.crc32(changed[:len(prefix) - 16]))
    legacy = struct.pack('<II', 0x20038000, RECOVERY_START + 9) + bytes([0x75] * 4997)
    sd = bytes([0x93] * 516)
    bl = struct.pack('<II', 0x20038000, 0xf3009) + bytes([0xa1] * 4100)
    return {name: (payload, signed_packet(payload, version=version, kind=kind, sd_size=sd_size, bl_size=bl_size))
            for name, payload, version, kind, sd_size, bl_size in [
                ('old', old, 0x1000000, 0, 0, 0), ('normal', application, 0x1000001, 0, 0, 0),
                ('changed', bytes(changed), 0x1000001, 0, 0, 0), ('legacy', legacy, 1, 0, 0, 0),
                ('system', sd + bl, 1, 3, len(sd), len(bl))]}


class WebUpdateTests(unittest.TestCase):
    def test_softdevice_lists_accept_any_matching_requirement_and_reject_malformed_values(self):
        payload = fixtures()['normal'][0]
        for name, (requirements, valid) in requirement_cases().items():
            with self.subTest(name=name):
                packet = signed_packet(payload, requirements=requirements)
                if valid:
                    self.assertEqual(inspect_packet(packet)['sha256'], hashlib.sha256(payload).hexdigest())
                else:
                    with self.assertRaises(ValueError):
                        inspect_packet(packet)

    def test_interrupted_installations_and_nordic_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            lines = ['#include <stdint.h>']
            for name, (payload, packet) in fixtures().items():
                for suffix, data in [('payload', payload), ('packet', packet)]:
                    lines.append('static const uint8_t ' + name + '_' + suffix + '[] = {' + ','.join(map(str, data)) + '};')
            (directory / 'update-fixtures.h').write_text('\n'.join(lines))
            binary = directory / 'test-update'
            command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-O2',
                       '-fsanitize=address,undefined', '-I', str(directory), '-I', str(ROOT / 'firmware/common'),
                       str(ROOT / 'firmware/common/nordic_update.c'), str(ROOT / 'firmware/common/web_update.c'),
                       str(ROOT / 'resource/tools/tests/web_update.c'), '-o', str(binary)]
            if sys.platform != 'darwin':
                command.append('-lcrypto')
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True, timeout=60)
            subprocess.run([str(binary), '--stale-abi'], check=True, timeout=10)

    def test_original_nordic_signatures_and_corrupt_payloads(self):
        data = fixtures()
        normal, packet = data['normal']
        package = archive(normal, packet)
        self.assertEqual(inspect_zip(package)['application']['payload'], normal)
        self.assertEqual(inspect_packet(packet)['sha256'], hashlib.sha256(normal).hexdigest())
        with self.assertRaises(ValueError):
            inspect_zip(archive(normal[:-1] + b'bad', packet))
        corrupt = bytearray(packet)
        corrupt[-1] ^= 1
        from cryptography.exceptions import InvalidSignature
        with self.assertRaises(InvalidSignature):
            inspect_packet(bytes(corrupt))
        for invalid in [b'', b'\x12\xff', packet + b'\x00', blob(1, b'unsigned')]:
            with self.assertRaises((ValueError, KeyError)):
                inspect_packet(invalid)

    def test_standard_zip_and_initial_flash_cache_match(self):
        payload = bytearray(fixtures()['normal'][0])
        payload[APPLICATION_START - RECOVERY_START + 128:APPLICATION_START - RECOVERY_START + 256] = b'\xff' * 128
        payload = bytes(payload)
        packet = signed_packet(payload)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_hex(directory / 'application.hex', {address: byte for address, byte in
                enumerate(payload[APPLICATION_START - RECOVERY_START:], APPLICATION_START) if byte != 255})
            # The real ELF has no footer: packaging owns its CRC and ABI footer.
            write_hex(directory / 'recovery.hex', dict(enumerate(payload[:RECOVERY_END - RECOVERY_START - 16], RECOVERY_START)))
            prepare(directory)
            before = read_hex(directory / 'installation.hex')
            self.assertTrue(all(address in before for address in range(APPLICATION_START, max(before) + 1)))
            actual = bytes(before.get(i, 255) for i in range(RECOVERY_START, max(before) + 1))
            self.assertEqual(actual, payload)
            package = archive(actual, packet)
            (directory / 'chameleon-unleashed-app.zip').write_bytes(package)
            (directory / 'updates').mkdir()
            (directory / 'updates/catalog.json').write_bytes(b'previous catalogue')
            manifest = publish(directory, directory / 'updates', '1.0.1')
            self.assertEqual((directory / 'updates/catalog.json').read_bytes(), b'previous catalogue')
            self.assertTrue(manifest['path'].endswith('.zip'))
            self.assertEqual((directory / 'updates' / manifest['path']).read_bytes(), package)
            memory = read_hex(directory / 'installation.hex')
            cache = bytes(memory[METADATA + i] for i in range(528))
            magic, size, length, crc = struct.unpack_from('<4I', cache)
            self.assertEqual((magic, size, length), (COMMITTED, len(actual), len(packet)))
            self.assertEqual(cache[16:16 + length], packet)
            self.assertEqual(crc, zlib.crc32(cache[:12] + bytes(4) + cache[16:]))
            self.assertEqual(bytes(memory[METADATA + 4092 + i] for i in range(4)), struct.pack('<I', COMMITTED))

    def test_frontend_protocol_and_zip_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name, data in assets('https://example.test/').items():
                (directory / name).write_bytes(data)
            for name, (payload, packet) in fixtures().items():
                if name == 'system':
                    continue
                (directory / (name + '.zip')).write_bytes(archive(payload, packet,
                    *(fixtures()['system'] if name == 'legacy' else (None, None))))
            payload = fixtures()['normal'][0]
            for name, (requirements, _) in requirement_cases().items():
                packet = signed_packet(payload, requirements=requirements)
                (directory / ('requirements-' + name + '.zip')).write_bytes(archive(payload, packet))
            (directory / 'update.test.mjs').write_bytes((ROOT / 'resource/tools/tests/web_update.test.mjs').read_bytes())
            subprocess.run(['node', '--test', str(directory / 'update.test.mjs')], check=True, timeout=30)

    def test_migration_preserves_original_signed_entries(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for kind in ['bootloader', 'application']:
                with ZipFile(directory / (kind + '.zip'), 'w') as package:
                    package.writestr(kind + '.bin', (kind + ' signed payload').encode())
                    package.writestr(kind + '.dat', (kind + ' signed init packet').encode())
                    package.writestr('manifest.json', json.dumps({'manifest': {kind: {
                        'bin_file': kind + '.bin', 'dat_file': kind + '.dat'}}}))
            installation_package(directory / 'bootloader.zip', directory / 'application.zip', directory / 'install.zip')
            with ZipFile(directory / 'install.zip') as package:
                manifest = json.loads(package.read('manifest.json'))['manifest']
                self.assertEqual(list(manifest), ['bootloader', 'application'])
                for kind in manifest:
                    self.assertEqual(package.read(manifest[kind]['dat_file']), (kind + ' signed init packet').encode())

    def test_real_official_release_when_downloaded(self):
        path = ROOT / '.cache/updates/official-v2.3.0.zip'
        if not path.exists():
            self.skipTest('Public release download is an optional integration fixture')
        images = inspect_zip(path.read_bytes())
        self.assertEqual(images['application']['version'], 1)
        self.assertEqual(images['softdevice_bootloader']['version'], 1)
        softdevice = read_hex(ROOT / 'firmware/objects/softdevice.hex')
        sd_size = images['softdevice_bootloader']['sd_size']
        self.assertEqual(images['softdevice_bootloader']['payload'][:sd_size], bytes(softdevice.get(i, 255) for i in range(0x1000, 0x1000 + sd_size)))

    def test_hex_checksums_and_partition_bounds(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / 'test.hex'
            memory = {0x27000: 1, 0x40000: 2, 0x10001014: 3}
            write_hex(path, memory)
            self.assertEqual(read_hex(path), memory)
            lines = path.read_text().splitlines()
            lines[1] = lines[1][:-2] + ('00' if lines[1][-2:] != '00' else '01')
            path.write_text('\n'.join(lines) + '\n')
            with self.assertRaises(ValueError): read_hex(path)
            write_hex(directory / 'application.hex', {APPLICATION_START - 1: 1})
            with self.assertRaises(ValueError): prepare(directory)


if __name__ == '__main__': unittest.main()
