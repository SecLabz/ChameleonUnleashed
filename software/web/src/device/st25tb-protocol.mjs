export const ST25TB = {
  INFO: 2305,
  READ: 2306,
  DUMP: 2307,
  WRITE: 2311,
  TYPE: 2000,
  UID_BYTES: 8,
  BLOCK_BYTES: 4,
  BLOCKS: 16,
  SYSTEM_BLOCK: 255,
  COUNTER_BLOCKS: [5, 6],
  IDENTITY_BYTES: 11,
  TEAROFF_PATH: '/api/st25tb/tearoff',
  BYTES: 20,
  DONE: 3,
  SUCCESS: 0,
  TRANSPORT_ERROR: 5,
  CURRENT_VALID: 1,
  CLEANUP_COMPLETE: 2,
  POLL_MS: 1000
};

export function st25tbName(uid) {
  // UID header: family, ST manufacturer, product.
  if (uid?.[0] !== 0xd0 || uid[1] !== 0x02) {
    return 'ST25TB/SRx';
  }
  if (uid[2] === 0x33) {
    return 'ST25TB512-AT';
  }
  if (uid[2] === 0x1b) {
    return 'SRI512 / ST25TB512-AC';
  }
  return uid[2] >= 0x18 && uid[2] <= 0x1b ? 'SRI512' : 'ST25TB/SRx';
}

export function st25tbIdentity(data) {
  const [chip_id, blocks, block_size] = data.slice(ST25TB.UID_BYTES);
  if (
    data.length !== ST25TB.IDENTITY_BYTES ||
    ![0, ST25TB.BLOCKS].includes(blocks) ||
    block_size !== ST25TB.BLOCK_BYTES
  ) {
    return null;
  }

  return {
    protocol: 'st25tb',
    uid: Array.from(data.slice(0, ST25TB.UID_BYTES)),
    chip_id,
    blocks,
    block_size
  };
}

export function encodeSt25tbTearoff(uid, block, target) {
  return new Uint8Array([
    ...uid,
    block,
    target & 255,
    (target >>> 8) & 255,
    (target >>> 16) & 255,
    target >>> 24
  ]);
}

const CURRENT = 4;
const TARGET = 8;
const ATTEMPTS = 12;
const DELAY_US = 16;
const SECONDS = 18;

export function decodeSt25tbTearoff(data) {
  const [state, outcome, block, flags] = data;
  if (data.length !== ST25TB.BYTES || state > ST25TB.DONE || outcome > ST25TB.TRANSPORT_ERROR) {
    throw new Error('Invalid tearoff state');
  }

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  // Four header bytes, three LE32 fields, two LE16 fields.
  return {
    state,
    outcome,
    block,
    flags,
    current: view.getUint32(CURRENT, true),
    target: view.getUint32(TARGET, true),
    attempts: view.getUint32(ATTEMPTS, true),
    delay_us: view.getUint16(DELAY_US, true),
    seconds: view.getUint16(SECONDS, true)
  };
}
