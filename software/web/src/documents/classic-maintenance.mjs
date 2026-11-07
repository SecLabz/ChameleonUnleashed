export function factoryTrailer(last = false) {
  return [...Array(6).fill(255), 255, 7, 128, last ? 188 : 105, ...Array(6).fill(255)];
}

export function repairBlock0(before, uid, geometry, observedSak) {
  if (before.length !== 16 || ![4, 7].includes(uid.length)) {
    throw new Error('Block-0 repair requires 16 bytes and a four- or seven-byte UID');
  }
  const after = [...before];
  after.splice(0, uid.length, ...uid);
  const offset = uid.length === 4 ? 5 : 7;
  if (uid.length === 4) {
    after[4] = uid.reduce((bcc, byte) => bcc ^ byte, 0);
  }
  if (geometry.blocks === 128 && observedSak !== undefined && ![8, 16].includes(observedSak)) {
    throw new Error('2K repair requires a compatible observed SAK');
  }
  after[offset] = geometry.blocks === 128 && observedSak !== undefined ? observedSak : geometry.sak;
  after[offset + 1] = geometry.atqa[1] | (uid.length === 7 ? 0x40 : 0);
  after[offset + 2] = geometry.atqa[0];
  return after;
}

export function maintenanceSectors(text, count) {
  if (!text) {
    return [];
  }
  const sectors =
    text.trim() === 'all'
      ? Array.from({ length: count }, (_, index) => index)
      : text.split(',').map((part) => (/^\d+$/.test(part.trim()) ? Number(part.trim()) : NaN));
  if (sectors.some((sector) => !Number.isInteger(sector) || sector < 0 || sector >= count)) {
    throw new Error(`Enter sector indices from 0 to ${count - 1}, separated by commas, or all`);
  }
  return [...new Set(sectors)];
}
