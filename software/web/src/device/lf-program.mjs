import { be16, encodeSlotImage } from '../documents/slot-image.mjs';
import { COMMAND } from './commands.mjs';

const SNAPSHOT_BYTES = 36;
const PLAN_BYTES = 69;

const word = (bytes, offset) => new DataView(Uint8Array.from(bytes).buffer).getUint32(offset);

export function decodeLfChip(bytes) {
  if (
    bytes.length < SNAPSHOT_BYTES ||
    bytes[0] !== 1 ||
    bytes[1] > 1 ||
    bytes[2] & 0x80 ||
    bytes[3] > 2 ||
    bytes.slice(32, 36).some(Boolean)
  ) {
    throw new Error('Invalid T55xx inspection reply');
  }
  if (!bytes[1]) {
    if (bytes.slice(1, SNAPSHOT_BYTES).some(Boolean)) {
      throw new Error('Invalid inconclusive T55xx reply');
    }
    return null;
  }
  if ((bytes[2] & 7) !== 7) {
    throw new Error('Incomplete T55xx detection');
  }
  return Array.from({ length: 7 }, (_, index) =>
    bytes[2] & (1 << index) ? word(bytes, 4 + index * 4) : null
  );
}

export function lfChipInfo(blocks) {
  if (!blocks) {
    return null;
  }
  const config = blocks[0];
  const modulation = {
    0: 'NRZ',
    1: 'PSK1',
    2: 'PSK2',
    3: 'PSK3',
    4: 'FSK1',
    5: 'FSK2',
    6: 'FSK1a',
    7: 'FSK2a',
    8: 'Manchester',
    16: 'Biphase',
    24: 'Diphase'
  }[(config >>> 12) & 31];
  const clock = [8, 16, 32, 40, 50, 64, 100, 128][(config >>> 18) & 7];
  return {
    modulation: modulation || 'Unknown modulation',
    clock,
    passwordEnabled: !!(config & 0x10),
    programmingSupported: !(config & 0x00020310)
  };
}

function prefix(operation) {
  return [1, operation, 0, 0, 0, 0, 0];
}

async function lfCommand(device, payload) {
  try {
    return await device.command(COMMAND.LF_T55XX, payload);
  } catch (error) {
    if (error.httpStatus === 404) {
      throw Object.assign(new Error('Update Chameleon firmware to check and program T55xx tags.'), {
        status: 0x67
      });
    }
    throw error;
  }
}

export async function inspectLfChip(device) {
  const bytes = await lfCommand(device, prefix(0));
  if (bytes.length !== SNAPSHOT_BYTES) {
    throw new Error('Invalid T55xx inspection length');
  }
  return decodeLfChip(bytes);
}

export async function prepareLfCopy(device, source) {
  if (source.kind !== 'lf') {
    throw new Error('An RFID source is required');
  }
  encodeSlotImage(source);
  const data = [...source.memory[0]];
  // Match the firmware's default OEM for C15001 and ACTPHID.
  if (source.type === 200 && [22, 42].includes(data[0]) && data[11] === 0 && data[12] === 0) {
    data[11] = 3;
    data[12] = 132;
  }
  const clock = source.type === 100 ? source.lf?.clock || 0 : 0;
  const request = [...prefix(1), ...be16(source.type), clock, data.length, ...data];
  const reply = await lfCommand(device, request);
  if (reply.length !== PLAN_BYTES || reply[36] < 2 || reply[36] > 7) {
    throw new Error('Invalid T55xx preparation reply');
  }
  const chip = decodeLfChip(reply);
  const blocks = Array.from({ length: reply[36] }, (_, index) => word(reply, 37 + index * 4));
  if (
    !chip ||
    blocks.some((_, index) => chip[index] === null) ||
    ((blocks[0] >>> 5) & 7) + 1 !== blocks.length ||
    (blocks[0] ^ chip[0]) & 0xf0000011
  ) {
    throw new Error('Invalid T55xx write plan');
  }
  const changes = blocks.filter((value, index) => value !== chip[index]).length;
  return { chip, blocks, changes, request, fingerprint: Array.from(reply.slice(65, 69)) };
}

export async function programLfCopy(device, plan) {
  device.check();
  const request = [...plan.request];
  request[1] = 2;
  let reply;
  try {
    reply = await lfCommand(device, [...request, ...plan.fingerprint]);
  } catch (error) {
    if ([0x41, 0x42, 0x60, 0x67, 0x69, 0x76].includes(error.status) || error.httpStatus === 409) {
      throw error;
    }
    throw new Error(`Programming may be incomplete. Read the tag before retrying. ${error.message}`);
  }
  let actual;
  try {
    actual = decodeLfChip(reply);
  } catch {
    throw new Error('T55xx verification unavailable. Read the tag before retrying.');
  }
  if (
    reply.length !== PLAN_BYTES ||
    ![1, 2].includes(reply[3]) ||
    !actual ||
    plan.blocks.some((value, index) => value !== actual[index])
  ) {
    throw new Error('T55xx verification failed. Read the tag before retrying.');
  }
  return actual;
}
