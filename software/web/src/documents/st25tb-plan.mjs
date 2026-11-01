import { ST25TB } from '../device/st25tb-protocol.mjs';

const equal = (before, after) =>
  before?.length === after?.length && before.every((value, index) => value === after[index]);

export const st25tbCounter = (bytes) =>
  (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;

export function planSt25tb(document, physical) {
  if (
    document.type !== ST25TB.TYPE ||
    physical.uid.join() !== document.card.uid.join() ||
    physical.memory.length !== ST25TB.BLOCKS
  ) {
    throw new Error('Physical ST25TB512-AT identity changed');
  }

  const writes = [];
  const tearoffs = [];
  for (let block = 0; block < ST25TB.BLOCKS; block++) {
    const after = document.memory[block];
    const before = physical.memory[block];
    if (!after || !before || equal(after, before)) {
      continue;
    }

    const item = { block, before, after };
    const counterIncreases =
      ST25TB.COUNTER_BLOCKS.includes(block) && st25tbCounter(after) > st25tbCounter(before);
    if (counterIncreases) {
      tearoffs.push(item);
    } else {
      writes.push(item);
    }
  }

  const system = document.card.system ?? document.capture?.system;
  let otp = null;
  if (system && physical.system && !equal(system, physical.system)) {
    if (system.some((value, index) => (value | physical.system[index]) !== physical.system[index])) {
      throw new Error('sysOTP bits can only change from 1 to 0');
    }
    otp = { block: ST25TB.SYSTEM_BLOCK, before: physical.system, after: system };
  }
  return { writes, tearoffs, otp };
}
