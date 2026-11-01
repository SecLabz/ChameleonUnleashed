import { decodeNfcvSlot, encodeNfcvSlot } from './nfcv-slot.mjs';
import { hex } from '../device/transport.mjs';
import { decodeDesfireSlot, encodeDesfireSlot } from './desfire.mjs';

export const TYPES = {
  100: 'EM410x',
  104: 'EM410x Electra',
  150: 'PAC/Stanley',
  170: 'Viking',
  180: 'Jablotron',
  200: 'HID Prox',
  201: 'ioProx',
  310: 'IDTECK',
  1000: 'Classic Mini',
  1001: 'Classic 1K',
  1002: 'Classic 2K',
  1003: 'Classic 4K',
  1100: 'NTAG213',
  1101: 'NTAG215',
  1102: 'NTAG216',
  1103: 'Ultralight',
  1104: 'Ultralight C',
  1105: 'Ultralight EV1 48',
  1106: 'Ultralight EV1 128',
  1300: 'DESFire',
  1500: 'SLIX-L',
  1501: 'SLIX',
  1107: 'NTAG210',
  1108: 'NTAG212',
  2000: 'ST25TB512-AT',
  3000: 'ISO14443-4',
  3001: 'SEOS'
};

export const TYPE2_PROFILES = {
  1100: 'ntag213',
  1101: 'ntag215',
  1102: 'ntag216',
  1103: 'ultralight',
  1104: 'ultralight_c',
  1105: 'ultralight_ev1_48',
  1106: 'ultralight_ev1_128',
  1107: 'ntag210',
  1108: 'ntag212'
};

export const typeName = (type) => TYPES[type] || `Type ${type}`;

export const LF_RECORD_BYTES = { 100: 5, 104: 13, 150: 8, 170: 4, 180: 5, 200: 13, 201: 16, 310: 8 };

export function family(type) {
  if (type >= 1000 && type <= 1003) {
    return 'classic';
  }
  if (type >= 1100 && type <= 1108) {
    return 'type2';
  }
  if (type === 1300) {
    return 'desfire';
  }
  if ([1500, 1501].includes(type)) {
    return 'nfcv';
  }
  if (type === 2000) {
    return 'st25tb';
  }
  return [100, 104, 150, 170, 180, 200, 201, 310].includes(type) ? 'lf' : null;
}

export const read16 = (bytes) => bytes[0] * 256 + bytes[1];

export const read32 = (bytes) =>
  (bytes[0] * 0x1000000 + bytes[1] * 0x10000 + bytes[2] * 0x100 + bytes[3]) >>> 0;

export function be32(value) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new Error('Refresh slots before changing this place');
  }
  return [value >>> 24, (value >>> 16) & 255, (value >>> 8) & 255, value & 255];
}

export const be16 = (value) => [value >> 8, value & 255];

export const isByte = (value) => Number.isInteger(value) && value >= 0 && value <= 255;

export const bytesValid = (bytes) => Array.isArray(bytes) && bytes.every(isByte);

export function nickname(text) {
  const bytes = new TextEncoder().encode(text.trim());
  if (!bytes.length || bytes.length > 32 || /[\x00-\x1f\x7f]/.test(text)) {
    throw new Error('Use a name of 1–32 UTF-8 bytes without control characters');
  }
  return bytes;
}

export function decodeSlotImage(bytes, name = 'Tag') {
  if (bytes instanceof Uint8Array && bytes[0] === 2) {
    const type = read16(bytes.slice(1, 3));
    if (type === 1300) {
      return decodeDesfireSlot(bytes, name);
    }

    if ([1500, 1501].includes(type)) {
      if (bytes.length < 5 || read16(bytes.slice(3, 5)) !== bytes.length - 5) {
        throw new Error('Invalid SLIX image length');
      }
      return decodeNfcvSlot(Array.from(bytes.slice(5)), type, name);
    }

    if (
      bytes.length !== 82 ||
      read16(bytes.slice(1, 3)) !== 2000 ||
      read16(bytes.slice(3, 5)) !== 77 ||
      bytes[12] !== 0xd0 ||
      bytes[11] !== 2 ||
      bytes[10] !== 0x33
    ) {
      throw new Error('Invalid ST25TB image');
    }

    return {
      name,
      type: 2000,
      kind: 'st25tb',
      unitBytes: 4,
      card: {
        protocol: 'st25tb',
        uid: Array.from(bytes.slice(5, 13)).reverse(),
        blocks: 16,
        block_size: 4,
        chip_id: bytes[81],
        system: Array.from(bytes.slice(77, 81))
      },
      geometry: { name: 'ST25TB512-AT', blocks: 16 },
      memory: Array.from({ length: 16 }, (_, block) =>
        Array.from(bytes.slice(13 + block * 4, 17 + block * 4))
      ),
      locks: Array(16).fill(null),
      warnings: []
    };
  }

  if (!(bytes instanceof Uint8Array) || bytes.length < 9 || bytes.length > 4500 || bytes[0] !== 1) {
    throw new Error('Invalid Chameleon image');
  }

  const type = read16(bytes.slice(1, 3));
  const kind = family(type);
  const unitBytes = bytes[3];
  const memoryBytes = read16(bytes.slice(4, 6));
  const uidLength = bytes[6];
  const atsLength = bytes[7];
  const optionLength = bytes[8];
  if (!kind || ['st25tb', 'nfcv'].includes(kind) || !unitBytes || memoryBytes % unitBytes || !memoryBytes) {
    throw new Error('Unsupported Chameleon image geometry');
  }

  const expectedUnitBytes = kind === 'classic' ? 16 : 4;
  const expectedOptionBytes = kind === 'classic' ? 7 : 5;
  const invalidFields =
    kind === 'lf'
      ? uidLength ||
        atsLength ||
        optionLength ||
        unitBytes !== memoryBytes ||
        unitBytes !== LF_RECORD_BYTES[type]
      : ![4, 7, 10].includes(uidLength) ||
        unitBytes !== expectedUnitBytes ||
        optionLength !== expectedOptionBytes;
  if (invalidFields) {
    throw new Error('Invalid Chameleon image fields');
  }

  let offset = 9;
  const card = { uid: [], atqa: [], sak: 0, ats: [] };
  if (uidLength) {
    card.uid = Array.from(bytes.slice(offset, offset + uidLength));
    offset += uidLength;
    card.atqa = Array.from(bytes.slice(offset, offset + 2)).reverse();
    offset += 2;
    card.sak = bytes[offset];
    offset++;
    card.ats = Array.from(bytes.slice(offset, offset + atsLength));
    offset += atsLength;
  }

  if (offset + optionLength + memoryBytes !== bytes.length) {
    throw new Error('Truncated Chameleon image');
  }

  const options = Array.from(bytes.slice(offset, offset + optionLength));
  offset += optionLength;
  const units = Array.from({ length: memoryBytes / unitBytes }, (_, unit) =>
    Array.from(bytes.slice(offset + unit * unitBytes, offset + (unit + 1) * unitBytes))
  );

  let memoryUnits = units.length;
  if (
    kind === 'classic' &&
    (options[0] > 4 || options.slice(1, 6).some((value) => value > 1) || options[6] > 2)
  ) {
    throw new Error('Invalid Classic emulator settings');
  }
  if (kind === 'type2') {
    memoryUnits = read16(options.slice(3));
    if (!memoryUnits || memoryUnits > units.length || options[0] > 1 || options[1] > 4 || options[2] > 1) {
      throw new Error('Invalid Type 2 emulator settings');
    }
  }

  return {
    name,
    type,
    kind,
    unitBytes,
    card,
    memory: units.slice(0, memoryUnits),
    emulator: { options, extra: units.slice(memoryUnits) }
  };
}

export function encodeSlotImage(document) {
  if (document.kind === 'desfire') {
    return encodeDesfireSlot(document);
  }

  if (document.kind === 'nfcv') {
    const raw = encodeNfcvSlot(document);
    return new Uint8Array([2, ...be16(document.type), ...be16(raw.length), ...raw]);
  }

  if (document.kind === 'st25tb') {
    const { card, memory } = document;
    const system = card.system ?? document.capture?.system;
    if (
      document.type !== 2000 ||
      card.protocol !== 'st25tb' ||
      !bytesValid(card.uid) ||
      card.uid.length !== 8 ||
      card.uid[0] !== 0xd0 ||
      card.uid[1] !== 2 ||
      card.uid[2] !== 0x33 ||
      !bytesValid(system) ||
      system.length !== 4 ||
      !Number.isInteger(card.chip_id) ||
      card.chip_id < 0 ||
      card.chip_id > 255 ||
      memory.length !== 16 ||
      memory.some((block) => !bytesValid(block) || block.length !== 4)
    ) {
      throw new Error('ST25TB save requires all user blocks and system data');
    }

    return new Uint8Array([
      2,
      ...be16(2000),
      0,
      77,
      ...[...card.uid].reverse(),
      ...memory.flat(),
      ...system,
      card.chip_id
    ]);
  }

  const type = document.type ?? { 20: 1000, 64: 1001, 128: 1002, 256: 1003 }[document.memory.length];
  const kind = family(type);
  const unitBytes = document.unitBytes ?? 16;
  const memory = [...document.memory, ...(document.emulator?.extra || [])];
  if (!kind || !memory.length || memory.some((unit) => !bytesValid(unit) || unit.length !== unitBytes)) {
    throw new Error('Saving requires a complete, valid image');
  }

  const card = document.card;
  const uid = kind === 'lf' ? [] : card.uid;
  const ats = kind === 'lf' ? [] : card.ats || [];
  if (
    !bytesValid(uid) ||
    !bytesValid(ats) ||
    ats.length > 255 ||
    (kind !== 'lf' &&
      (![4, 7, 10].includes(uid.length) ||
        !bytesValid(card.atqa) ||
        card.atqa.length !== 2 ||
        !Number.isInteger(card.sak) ||
        card.sak < 0 ||
        card.sak > 255))
  ) {
    throw new Error('Invalid tag identity');
  }

  const options = document.emulator?.options ?? (kind === 'classic' ? [0, 0, 0, 0, 0, 0, 1] : []);
  if (!bytesValid(options)) {
    throw new Error('Invalid emulator options');
  }

  const wire = new Uint8Array([
    1,
    ...be16(type),
    unitBytes,
    ...be16(memory.length * unitBytes),
    uid.length,
    ats.length,
    options.length,
    ...uid,
    ...(uid.length ? [...card.atqa].reverse().concat(card.sak, ats) : []),
    ...options,
    ...memory.flat()
  ]);
  decodeSlotImage(wire);
  return wire;
}

export function identityLabel(document) {
  if (document.card.protocol === 'nfc_b') {
    return `PUPI ${hex(document.card.pupi)}`;
  }
  if (document.kind === 'lf') {
    return document.memory.length ? `ID ${hex(document.memory[0])}` : 'No decoded ID';
  }
  return `UID ${hex(document.card.uid)}`;
}
