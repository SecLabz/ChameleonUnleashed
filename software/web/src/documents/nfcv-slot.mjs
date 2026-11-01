// Existing 112/192-byte slot records retain their reserved data and lock offsets.
const validBytes = (bytes, length) =>
  Array.isArray(bytes) &&
  bytes.length === length &&
  bytes.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 255);

// User blocks, reserved storage blocks, and shared-codec model.
function geometry(type) {
  if (type === 1500) {
    return [8, 16, 4];
  }
  if (type === 1501) {
    return [28, 32, 3];
  }
  return [];
}

export function validateNfcvSlot(raw, type) {
  const [blockCount, storedBlocks] = geometry(type);
  if (
    !blockCount ||
    !validBytes(raw, 32 + storedBlocks * 5) ||
    raw[0] !== 0xe0 ||
    raw[1] !== 4 ||
    raw[2] !== (type === 1500 ? 3 : 1) ||
    (type === 1501 && (raw[3] & 0x18) !== 0x10) ||
    raw[11] & (type === 1500 ? 0xe0 : 0xe8) ||
    raw.slice(32 + storedBlocks * 4).some((byte) => byte > 1)
  ) {
    throw new Error('Invalid SLIX slot image');
  }
}

export function decodeNfcvSlot(raw, type, name) {
  validateNfcvSlot(raw, type);

  const [blockCount, storedBlocks, model] = geometry(type);
  const flags = raw[11];
  const locks = raw.slice(32 + storedBlocks * 4, 32 + storedBlocks * 4 + blockCount).map(Boolean);
  const metadata = [
    86,
    73,
    67,
    49,
    (flags & 3) | ((flags & 16) >> 2) | ((flags & 4) << 1) | ((flags & 8) << 1),
    ...raw.slice(8, 11),
    4,
    blockCount,
    0,
    0,
    ...locks.map(Number)
  ];
  const offsets = type === 1500 ? [20, 24, 28] : [28];
  const secrets = offsets.flatMap((offset) => raw.slice(offset, offset + 4).reverse());

  return {
    name,
    type,
    kind: 'nfcv',
    unitBytes: 4,
    card: {
      protocol: 'nfc_v',
      uid: raw.slice(0, 8),
      blocks: blockCount,
      block_size: 4,
      dsfid: raw[8],
      afi: raw[9],
      ic_ref: raw[10]
    },
    geometry: { name: type === 1500 ? 'SLIX-L' : 'SLIX', blocks: blockCount },
    memory: Array.from({ length: blockCount }, (_, index) => raw.slice(32 + index * 4, 36 + index * 4)),
    locks,
    emulator: { model, metadata, secrets, legacy: [...raw] },
    warnings: []
  };
}

export function encodeNfcvSlot(document) {
  const { card, memory, emulator, locks } = document;
  const type = document.type;
  const [blockCount, storedBlocks, model] = geometry(type);
  if (
    !blockCount ||
    card?.protocol !== 'nfc_v' ||
    card.blocks !== blockCount ||
    card.block_size !== 4 ||
    !validBytes(card.uid, 8) ||
    !Array.isArray(memory) ||
    memory.length !== blockCount ||
    memory.some((byte) => !validBytes(byte, 4)) ||
    !Array.isArray(locks) ||
    locks.length !== blockCount ||
    locks.some((byte) => byte !== null && typeof byte !== 'boolean') ||
    emulator?.model !== model ||
    !validBytes(emulator.metadata, 12 + blockCount) ||
    !validBytes(emulator.secrets, model === 3 ? 4 : 12)
  ) {
    throw new Error('SLIX save requires complete memory and explicit emulator defaults');
  }

  const metadata = emulator.metadata;
  const flags = metadata[4];
  if (
    metadata.slice(0, 4).join() !== '86,73,67,49' ||
    metadata.slice(8, 12).join() !== [4, blockCount, 0, 0].join() ||
    metadata.slice(12).some((byte) => byte > 1) ||
    flags & (type === 1500 ? 0x80 : 0x90)
  ) {
    throw new Error('Invalid SLIX metadata');
  }
  if (flags & 0x60) {
    throw new Error('This emulator does not support password-protected EAS or AFI');
  }

  let raw = Array(32 + storedBlocks * 5).fill(0);
  if (emulator.legacy !== undefined) {
    validateNfcvSlot(emulator.legacy, type);
    raw = [...emulator.legacy];
  }

  raw.splice(0, 8, ...card.uid);
  for (const [index, field] of ['dsfid', 'afi', 'ic_ref'].entries()) {
    const value = card[field] ?? metadata[5 + index];
    if (!Number.isInteger(value) || value < 0 || value > 255) {
      throw new Error('Invalid SLIX system metadata');
    }
    raw[8 + index] = value;
  }

  raw[11] = (flags & 3) | ((flags & 4) << 2) | ((flags & 8) >> 1) | ((flags & 16) >> 1);
  const offsets = type === 1500 ? [20, 24, 28] : [28];
  offsets.forEach((offset, index) =>
    raw.splice(offset, 4, ...emulator.secrets.slice(index * 4, index * 4 + 4).reverse())
  );

  raw.splice(32, blockCount * 4, ...memory.flat());
  raw.splice(
    32 + storedBlocks * 4,
    blockCount,
    ...locks.map((value, index) => (value === null ? metadata[12 + index] : +value))
  );

  validateNfcvSlot(raw, type);
  return raw;
}
