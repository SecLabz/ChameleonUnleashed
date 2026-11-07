import { unzipSync } from './update-unzip.mjs';
import { sha256 } from './update-sha256.mjs';

export const MAXIMUM_SIZE = 4 * 1024 * 1024;
export const MINIMUM_ZIP_SIZE = 22; // Empty ZIP end-of-directory record.
export const UPDATE_CHUNK_BYTES = 1024;
export const RECOVERY_LAYOUT = 2;
const MAX_ZIP_ENTRIES = 32;
const MAX_ZIP_ENTRY_BYTES = 1024 * 1024;
const NORDIC_INIT_MAX = 512;
const HARDWARE_ULTRA = 0;
const SOFTDEVICE_S140_7_2_0 = 0x100;
const SHA256_BYTES = 32;
const ECDSA_P256_SIGNATURE_BYTES = 64;
const HASH_SHA256 = 3;
const SIGNATURE_ECDSA_P256_SHA256 = 0;
const OP_INIT = 1;
const FirmwareType = { application: 0, bootloader: 2, softdevice_bootloader: 3 };
// Field identifiers from Nordic dfu-cc.proto.
const Wire = { varint: 0, bytes: 2 };
const Packet = { signedCommand: 2 };
const SignedCommand = { command: 1, signatureType: 2, signature: 3 };
const Command = { opcode: 1, init: 2 };
const Init = {
  version: 1,
  hardware: 2,
  softdevices: 3,
  type: 4,
  sdSize: 5,
  blSize: 6,
  appSize: 7,
  hash: 8,
  debug: 9,
  bootValidation: 10
};
const Hash = { type: 1, bytes: 2 };

const Flash = {
  softdevice: 0x1000,
  recovery: 0x27000,
  recoveryEnd: 0x3b000,
  application: 0x3c000,
  applicationEnd: 0xc7000,
  systemStage: 0xbc000,
  bootloader: 0xf3000,
  bootloaderEnd: 0xfe000
};
const IMAGE_VECTOR_BYTES = 2 * Uint32Array.BYTES_PER_ELEMENT;
const RECOVERY_MAGIC = new TextEncoder().encode('CURECOV2');
const RECOVERY_DESCRIPTOR_BYTES = RECOVERY_MAGIC.length + 2 * Uint32Array.BYTES_PER_ELEMENT;
const RECOVERY_FOOTER_OFFSET = Flash.recoveryEnd - Flash.recovery - RECOVERY_DESCRIPTOR_BYTES;
const RECOVERY_CRC_OFFSET = RECOVERY_FOOTER_OFFSET + RECOVERY_MAGIC.length + Uint32Array.BYTES_PER_ELEMENT;
const BEGIN_HEADER_BYTES = 8;
const BEGIN_RESTORE = 0x01;
const BEGIN_WIPE = 0x02;
const decode = (bytes) => new TextDecoder('utf-8', { fatal: true }).decode(bytes);

function readInteger(bytes, position) {
  let value = 0;
  for (let shift = 0; shift < 35 && position < bytes.length; shift += 7) {
    const byte = bytes[position++];
    if (shift === 28 && byte & 0xf0) {
      throw new Error('Invalid Nordic integer.');
    }
    value += (byte & 127) * 2 ** shift;
    if (!(byte & 128)) {
      return [value, position];
    }
  }
  throw new Error('Truncated Nordic packet.');
}

function fields(bytes, repeated = []) {
  let position = 0;
  const integer = () => {
    let value;
    [value, position] = readInteger(bytes, position);
    return value;
  };
  const result = new Map();
  while (position < bytes.length) {
    const tag = integer();
    const number = tag >>> 3;
    const wire = tag & 7;
    if (!number || (result.has(number) && !repeated.includes(number))) {
      throw new Error('Invalid duplicate Nordic field.');
    }
    let value = integer();
    if (wire === Wire.bytes) {
      if (value > bytes.length - position) {
        throw new Error('Truncated Nordic packet.');
      }
      const end = position + value;
      value = bytes.subarray(position, end);
      position = end;
    } else if (wire !== Wire.varint) {
      throw new Error('Unsupported Nordic packet.');
    }
    if (number === Init.softdevices && repeated.includes(number)) {
      const values = result.get(number) ?? [];
      if (wire === Wire.varint) {
        values.push(value);
      } else {
        let offset = 0;
        while (offset < value.length) {
          let item;
          [item, offset] = readInteger(value, offset);
          values.push(item);
        }
      }
      result.set(number, values);
    } else {
      result.set(number, value);
    }
  }
  return result;
}

export function inspectPacket(packet) {
  if (!packet?.length || packet.length > NORDIC_INIT_MAX) {
    throw new Error('Invalid Nordic init packet.');
  }
  const outer = fields(packet);
  if (outer.size !== 1 || !outer.has(Packet.signedCommand)) {
    throw new Error('Signed Nordic firmware is required.');
  }
  const signed = fields(outer.get(Packet.signedCommand));
  if (
    signed.size !== Object.keys(SignedCommand).length ||
    signed.get(SignedCommand.signatureType) !== SIGNATURE_ECDSA_P256_SHA256 ||
    signed.get(SignedCommand.signature)?.length !== ECDSA_P256_SIGNATURE_BYTES
  ) {
    throw new Error('ECDSA-P256 signed Nordic firmware is required.');
  }
  const command = fields(signed.get(SignedCommand.command));
  if ((command.get(Command.opcode) ?? OP_INIT) !== OP_INIT || !command.has(Command.init)) {
    throw new Error('Invalid Nordic init command.');
  }
  const info = fields(command.get(Command.init), [Init.softdevices, Init.bootValidation]);
  const hash = fields(info.get(Init.hash) ?? new Uint8Array());
  const compatible = info.get(Init.softdevices)?.includes(SOFTDEVICE_S140_7_2_0);
  if (
    info.get(Init.hardware) !== HARDWARE_ULTRA ||
    info.get(Init.debug) ||
    !compatible ||
    hash.get(Hash.type) !== HASH_SHA256 ||
    hash.get(Hash.bytes)?.length !== SHA256_BYTES
  ) {
    throw new Error('Choose firmware for Chameleon Ultra with S140 7.2.0.');
  }
  return {
    type: info.get(Init.type) ?? FirmwareType.application,
    version: info.get(Init.version),
    sdSize: info.get(Init.sdSize) ?? 0,
    blSize: info.get(Init.blSize) ?? 0,
    size: info.get(Init.appSize) ?? 0,
    hash: [...hash.get(Hash.bytes)]
      .reverse()
      .map((v) => v.toString(16).padStart(2, '0'))
      .join('')
  };
}

export async function inspectFirmware(bytes, signal) {
  if (!(bytes instanceof Uint8Array) || bytes.length > MAXIMUM_SIZE || bytes.length < MINIMUM_ZIP_SIZE) {
    throw new Error('Choose a standard Nordic DFU ZIP for Chameleon Ultra.');
  }
  let count = 0;
  let expanded = 0;
  const names = new Set();
  const files = unzipSync(bytes, {
    filter: (entry) => {
      if (
        ++count > MAX_ZIP_ENTRIES ||
        entry.originalSize > MAX_ZIP_ENTRY_BYTES ||
        (expanded += entry.originalSize) > MAXIMUM_SIZE ||
        names.has(entry.name) ||
        !/^[a-zA-Z0-9][a-zA-Z0-9._/-]{0,180}$/.test(entry.name) ||
        entry.name.split('/').some((part) => part === '..' || part === '__proto__')
      ) {
        throw new Error('Invalid or oversized firmware ZIP.');
      }
      names.add(entry.name);
      return entry.name === 'manifest.json' || /\.(bin|dat)$/.test(entry.name);
    }
  });
  if (!files['manifest.json']) {
    throw new Error('Choose the Nordic DFU ZIP, not a source or resources archive.');
  }
  const manifest = JSON.parse(decode(files['manifest.json'])).manifest;
  if (
    !manifest ||
    Object.keys(manifest).some(
      (kind) => !['application', 'bootloader', 'softdevice_bootloader'].includes(kind)
    )
  ) {
    throw new Error('Unsupported Nordic manifest.');
  }
  const images = {};
  for (const [kind, entry] of Object.entries(manifest)) {
    const packet = files[entry.dat_file];
    const payload = files[entry.bin_file];
    const info = inspectPacket(packet);
    const type = FirmwareType[kind];
    if (
      !payload ||
      info.type !== type ||
      payload.length !== info.size + info.sdSize + info.blSize ||
      (await sha256(payload, signal)) !== info.hash
    ) {
      throw new Error('Firmware ZIP is incomplete or corrupt. Download it again.');
    }
    images[kind] = { ...info, packet, payload };
  }
  const application = images.application;
  if (
    !application ||
    application.size < IMAGE_VECTOR_BYTES ||
    application.size > Flash.applicationEnd - Flash.recovery
  ) {
    throw new Error('The ZIP must contain a compatible Ultra application.');
  }
  const recovery =
    application.payload.length >= Flash.application - Flash.recovery + IMAGE_VECTOR_BYTES &&
    RECOVERY_MAGIC.every((value, i) => application.payload[RECOVERY_FOOTER_OFFSET + i] === value);
  const system = images.softdevice_bootloader ?? images.bootloader;
  if (images.softdevice_bootloader && images.bootloader) {
    throw new Error('Ambiguous bootloader images.');
  }
  if (
    !recovery &&
    (!system ||
      application.size > Flash.systemStage - Flash.recovery ||
      system.blSize < IMAGE_VECTOR_BYTES ||
      system.blSize > Flash.bootloaderEnd - Flash.bootloader ||
      system.sdSize > Flash.recovery - Flash.softdevice)
  ) {
    throw new Error('A complete Ultra DFU ZIP including its bootloader is required to leave web recovery.');
  }
  if (recovery && system) {
    throw new Error(
      'Use the application DFU ZIP for web updates. Complete Unleashed installers are installed through classic DFU.'
    );
  }
  const prefixView = new DataView(
    application.payload.buffer,
    application.payload.byteOffset,
    application.payload.byteLength
  );
  return {
    application,
    system,
    restore: !recovery,
    version: application.version,
    size: application.size,
    recoveryCrc: recovery ? prefixView.getUint32(RECOVERY_CRC_OFFSET, true) : 0
  };
}

export function beginRequest(info, wipe = false) {
  const a = info.application.packet;
  const s = info.restore ? info.system.packet : new Uint8Array();
  const body = new Uint8Array(BEGIN_HEADER_BYTES + a.length + s.length);
  if (body.length > UPDATE_CHUNK_BYTES) {
    throw new Error('Nordic init packets are too large for this installer.');
  }
  body[0] = (info.restore ? BEGIN_RESTORE : 0) | (wipe && info.restore ? BEGIN_WIPE : 0);
  new DataView(body.buffer).setUint16(2, a.length, true);
  new DataView(body.buffer).setUint16(4, s.length, true);
  body.set(a, BEGIN_HEADER_BYTES);
  body.set(s, BEGIN_HEADER_BYTES + a.length);
  return body;
}
