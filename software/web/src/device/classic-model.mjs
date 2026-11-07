// NXP AN10833 Fig. 1: Plus S/X in SL1 shares SAK 08 with Classic 1K.
// Only the known historical bytes and matching ATQA/UID layout establish 2K.
const PLUS_HISTORY = [
  [0xc1, 5, 0x2f, 0x2f, 0, 0x35, 0xc7],
  [0xc1, 5, 0x2f, 0x2f, 1, 0xbc, 0xd6]
];

export function decodeClassicAts(frame, card) {
  if (!frame.length) {
    return { ats: [] };
  }
  if (frame.length < 4 || frame.length > 64 || frame[0] !== frame.length - 2) {
    throw new Error('Invalid Classic ATS framing');
  }
  let crc = 0x6363;
  for (const byte of frame.slice(0, -2)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0x8408 : 0);
    }
  }
  if (frame.at(-2) !== (crc & 255) || frame.at(-1) !== crc >>> 8) {
    throw new Error('Invalid Classic ATS CRC');
  }
  const ats = Array.from(frame.slice(0, -2));
  const result = { ats };
  const t0 = ats[1];
  let historical = 2;
  for (const flag of [0x10, 0x20, 0x40]) {
    if (t0 & flag) {
      historical++;
    }
  }
  const expectedAtqa = { 4: 4, 7: 0x44, 10: 0x84 }[card.uid?.length];
  if (
    card.sak === 8 &&
    expectedAtqa !== undefined &&
    card.atqa?.length === 2 &&
    card.atqa[0] === 0 &&
    card.atqa[1] === expectedAtqa &&
    !(t0 & 0x80) &&
    (t0 & 15) <= 8 &&
    PLUS_HISTORY.some(
      (history) =>
        ats.length - historical === history.length &&
        history.every((byte, index) => byte === ats[historical + index])
    )
  ) {
    result.type = 1002;
  }
  return result;
}
