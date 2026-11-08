"""Inspect unmodified Nordic Secure DFU ZIPs and verify their original signatures."""
import hashlib
import io
import json
from pathlib import Path
import re
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
MAX_ZIP = 4 * 1024 * 1024
MAX_ZIP_ENTRIES = 32
MAX_ZIP_ENTRY_SIZE = 1024 * 1024
NORDIC_INIT_MAX = 512
HARDWARE_ULTRA = 0
SOFTDEVICE_S140_7_2_0 = 0x100
SHA256_BYTES = hashlib.sha256().digest_size
ECDSA_P256_COMPONENT_BYTES = 32
ECDSA_P256_SIGNATURE_BYTES = 2 * ECDSA_P256_COMPONENT_BYTES
ECDSA_P256_PUBLIC_KEY_BYTES = 2 * ECDSA_P256_COMPONENT_BYTES
FW_APPLICATION, FW_BOOTLOADER, FW_SOFTDEVICE_BOOTLOADER = 0, 2, 3
HASH_SHA256 = 3
SIGNATURE_ECDSA_P256_SHA256 = 0
OP_INIT = 1


# Field identifiers from Nordic dfu-cc.proto.
class Packet:
    SIGNED_COMMAND = 2


class SignedCommand:
    COMMAND, SIGNATURE_TYPE, SIGNATURE = 1, 2, 3


class Command:
    OPCODE, INIT = 1, 2


class Init:
    VERSION, HARDWARE, SOFTDEVICES, TYPE = 1, 2, 3, 4
    SD_SIZE, BL_SIZE, APP_SIZE, HASH, DEBUG, BOOT_VALIDATION = 5, 6, 7, 8, 9, 10


class Hash:
    TYPE, BYTES = 1, 2


class Wire:
    VARINT, BYTES = 0, 2


def read_integer(data, position):
    value = 0
    for shift in range(0, 35, 7):
        if position >= len(data):
            raise ValueError('Truncated protobuf integer')
        byte = data[position]
        position += 1
        if shift == 28 and byte & 0xf0:
            raise ValueError('Protobuf integer overflow')
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, position
    raise ValueError('Invalid protobuf integer')


def fields(data, repeated=()):
    position = 0

    def integer():
        nonlocal position
        value, position = read_integer(data, position)
        return value

    result = {}
    while position < len(data):
        tag = integer()
        number, wire = tag >> 3, tag & 7
        if not number or (number in result and number not in repeated):
            raise ValueError('Invalid or duplicate protobuf field')
        value = integer()
        if wire == Wire.BYTES:
            if value > len(data) - position:
                raise ValueError('Truncated protobuf bytes')
            value, position = data[position:position + value], position + value
        elif wire != Wire.VARINT:
            raise ValueError('Unsupported protobuf wire type')
        if number == Init.SOFTDEVICES and number in repeated:
            values = result.setdefault(number, [])
            if wire == Wire.VARINT:
                values.append(value)
            else:
                offset = 0
                while offset < len(value):
                    item, offset = read_integer(value, offset)
                    values.append(item)
        else:
            result[number] = value
    return result


def public_key():
    from cryptography.hazmat.primitives.asymmetric import ec
    source = (ROOT / 'firmware/bootloader/src/dfu_public_key.c').read_text().split('= {')[1]
    little = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', source))
    if len(little) != ECDSA_P256_PUBLIC_KEY_BYTES:
        raise ValueError('Invalid Nordic public key')
    return ec.EllipticCurvePublicNumbers(int.from_bytes(little[:ECDSA_P256_COMPONENT_BYTES], 'little'),
                                       int.from_bytes(little[ECDSA_P256_COMPONENT_BYTES:], 'little'), ec.SECP256R1()).public_key()


def inspect_packet(packet, verify=True):
    if not 1 <= len(packet) <= NORDIC_INIT_MAX:
        raise ValueError('Invalid Nordic init packet size')
    outer = fields(packet)
    if set(outer) != {Packet.SIGNED_COMMAND}:
        raise ValueError('A signed Nordic packet is required')
    signed = fields(outer[Packet.SIGNED_COMMAND])
    if (set(signed) != {SignedCommand.COMMAND, SignedCommand.SIGNATURE_TYPE, SignedCommand.SIGNATURE}
            or signed[SignedCommand.SIGNATURE_TYPE] != SIGNATURE_ECDSA_P256_SHA256
            or len(signed[SignedCommand.SIGNATURE]) != ECDSA_P256_SIGNATURE_BYTES):
        raise ValueError('ECDSA-P256 signed command required')
    command = fields(signed[SignedCommand.COMMAND])
    if command.get(Command.OPCODE, OP_INIT) != OP_INIT or Command.INIT not in command:
        raise ValueError('Nordic init command required')
    info = fields(command[Command.INIT], repeated=(Init.SOFTDEVICES, Init.BOOT_VALIDATION))
    digest = fields(info.get(Init.HASH, b''))
    requirements = info.get(Init.SOFTDEVICES, [])
    if (info.get(Init.HARDWARE) != HARDWARE_ULTRA or info.get(Init.DEBUG, 0)
            or digest.get(Hash.TYPE) != HASH_SHA256 or len(digest.get(Hash.BYTES, b'')) != SHA256_BYTES
            or SOFTDEVICE_S140_7_2_0 not in requirements):
        raise ValueError('Firmware must target Ultra / S140 7.2.0 without debug bypass')
    if verify:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec, utils
        signature = signed[SignedCommand.SIGNATURE]
        der = utils.encode_dss_signature(int.from_bytes(signature[:ECDSA_P256_COMPONENT_BYTES], 'little'),
                                        int.from_bytes(signature[ECDSA_P256_COMPONENT_BYTES:], 'little'))
        public_key().verify(der, command[Command.INIT], ec.ECDSA(hashes.SHA256()))
    return {'type': info.get(Init.TYPE, FW_APPLICATION), 'version': info.get(Init.VERSION),
            'sd_size': info.get(Init.SD_SIZE, 0), 'bl_size': info.get(Init.BL_SIZE, 0),
            'app_size': info.get(Init.APP_SIZE, 0), 'sha256': digest[Hash.BYTES][::-1].hex()}


def inspect_zip(data, verify=True):
    if len(data) > MAX_ZIP:
        raise ValueError('Firmware ZIP is too large')
    images = {}
    with ZipFile(io.BytesIO(data)) as package:
        entries = package.infolist()
        names = [entry.filename for entry in entries]
        if (len(entries) > MAX_ZIP_ENTRIES or len(set(names)) != len(names)
                or any(entry.file_size > MAX_ZIP_ENTRY_SIZE or entry.flag_bits & 1 for entry in entries)
                or sum(entry.file_size for entry in entries) > MAX_ZIP):
            raise ValueError('Invalid or oversized firmware archive')
        document = json.loads(package.read('manifest.json'))['manifest']
        if set(document) - {'application', 'bootloader', 'softdevice_bootloader'}:
            raise ValueError('Unsupported Nordic manifest component')
        for kind, entry in document.items():
            packet = package.read(entry['dat_file'])
            payload = package.read(entry['bin_file'])
            info = inspect_packet(packet, verify)
            expected_type = {'application': FW_APPLICATION, 'bootloader': FW_BOOTLOADER,
                             'softdevice_bootloader': FW_SOFTDEVICE_BOOTLOADER}[kind]
            if (info['type'] != expected_type or len(payload) != sum(info[name] for name in ('sd_size', 'bl_size', 'app_size'))
                    or hashlib.sha256(payload).hexdigest() != info['sha256']):
                raise ValueError('Nordic payload differs from its signed init packet')
            images[kind] = {'packet': packet, 'payload': payload, **info}
    return images
